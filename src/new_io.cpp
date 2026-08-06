#include <_ansi.h>
#include <_syslist.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#undef errno
extern int errno;

#include "circle_glue.h"
#include "bmx_dirent_info.h"
#include "remote/developer_log_ring.h"
#include <assert.h>

#include <malloc.h>
#include <sys/unistd.h>
#include <circle/serial.h>
#include <circle/timer.h>

#include <ff.h>

#ifdef BMX_NEW_IO_HOST_TEST
#define BMX_NEW_IO_POSIX(name) bmx_##name
#else
#define BMX_NEW_IO_POSIX(name) name
#endif

// This is a replacement io.cpp specifically for BMX. Small files can be
// cached in memory to keep seek-heavy VICE media fast on slow SD cards. The
// cache is capped at Circle's largest reusable heap bucket so closing a file
// cannot permanently consume low heap. Larger files remain FatFs-backed and
// use direct seek/read/write operations.
//
// When a file is opened for READ ONLY, fatfs is used to open
// the file.  As long as the client never seeks, the file will
// not be loaded into ram and the disk still backs the data.  As
// soon as seek is called, a small file will be loaded into RAM and from then
// on RAM backs the data. Large files always remain FatFs-backed. The FatFs
// handle remains open in both cases.
//
// Newly truncated write-only files start in the bounded cache. If they grow
// past the limit, their contents are spilled once and subsequent I/O is
// direct. Other write-only modes use FatFs directly.
//
// Read-write files up to the cache limit use the RAM copy. Larger files are
// never copied in full and all random access goes directly through FatFs.

#define MAX_OPEN_FILES 10
#define MAX_OPEN_DIRS 10
#define BMC64_PATH_MAX 256
#define BMC64_DIRENT_FAT_ATTR_VALID 0x0100
#define BMC64_FILE_CACHE_LIMIT (512U * 1024U)

#ifndef BMC64_IO_STATS
#define BMC64_IO_STATS 0
#endif

static const char *pattern = "*";

static char currentDir[BMC64_PATH_MAX];

static bool copy_string(char *dst, size_t dst_size, const char *src) {
  if (dst_size == 0) {
    return false;
  }
  if (src == nullptr) {
    dst[0] = '\0';
    return true;
  }

  size_t len = strlen(src);
  if (len >= dst_size) {
    dst[0] = '\0';
    return false;
  }

  memcpy(dst, src, len + 1);
  return true;
}

static bool append_string(char *dst, size_t dst_size, const char *src) {
  size_t used = strlen(dst);
  size_t len = strlen(src);
  if (used >= dst_size || len >= dst_size - used) {
    return false;
  }

  memcpy(dst + used, src, len + 1);
  return true;
}

static bool has_fatfs_volume_prefix(const char *path) {
  const char *colon = strchr(path, ':');
  if (colon == nullptr) {
    return false;
  }

  const char *slash = strchr(path, '/');
  return slash == nullptr || colon < slash;
}

static bool is_absolute_path(const char *path) {
  return path[0] == '/' || has_fatfs_volume_prefix(path);
}

static bool is_root_path(const char *path) {
  if (strcmp(path, "/") == 0) {
    return true;
  }

  const char *colon = strchr(path, ':');
  return colon != nullptr &&
         (colon[1] == '\0' || (colon[1] == '/' && colon[2] == '\0'));
}

static void strip_trailing_slash(char *path) {
  size_t len = strlen(path);
  while (len > 1 && path[len - 1] == '/' && !is_root_path(path)) {
    path[--len] = '\0';
  }
}

static bool append_path_component(char *path, size_t path_size,
                                  const char *component) {
  if (component[0] == '\0') {
    return true;
  }

  size_t len = strlen(path);
  if (len > 0 && path[len - 1] != '/') {
    if (!append_string(path, path_size, "/")) {
      return false;
    }
  }

  return append_string(path, path_size, component);
}

static void move_current_dir_to_parent() {
  strip_trailing_slash(currentDir);
  if (is_root_path(currentDir)) {
    return;
  }

  char *last_slash = strrchr(currentDir, '/');
  if (last_slash == nullptr) {
    copy_string(currentDir, sizeof currentDir, "/");
    return;
  }

  if (last_slash == currentDir) {
    currentDir[1] = '\0';
    return;
  }

  const char *colon = strchr(currentDir, ':');
  if (colon != nullptr && last_slash == colon + 1) {
    last_slash[1] = '\0';
    return;
  }

  *last_slash = '\0';
}

/**
 * @fn int strend(const char *s, const char *t)
 * @brief Searches the end of string s for string t
 * @param s the string to be searched
 * @param t the substring to locate at the end of string s
 * @return one if the string t occurs at the end of the string s, and zero otherwise
 */
int strend(const char *s, const char *t)
{
    size_t ls = strlen(s); // find length of s
    size_t lt = strlen(t); // find length of t
    if (ls >= lt)  // check if t can fit in s
    {
        // point s to where t should start and compare the strings from there
        return (0 == memcmp(t, s + (ls - lt), lt));
    }
    return 0; // t was longer than s
}

static void reverse(char *x, int begin, int end) {
  char c;

  if (begin >= end)
    return;

  c = *(x + begin);
  *(x + begin) = *(x + end);
  *(x + end) = c;

  reverse(x, ++begin, --end);
}

static void itoa2(int i, char *dst) {
  int q = 0;
  int j;
  do {
    j = i % 10;
    dst[q] = '0' + j;
    q++;
    i = i / 10;
  } while (i > 0);
  dst[q] = '\0';

  reverse(dst, 0, strlen(dst) - 1);
}

CSerialDevice *g_serial;

static int SerialWriteAll(const char *ptr, int len) {
   if (!g_serial) {
      return len;
   }

   int written = 0;
   unsigned idle_loops = 0;
   while (written < len) {
      int result = g_serial->Write(ptr + written, len - written);
      if (result > 0) {
         written += result;
         idle_loops = 0;
         continue;
      }

      if (++idle_loops > 200000) {
         break;
      }
      CTimer::SimpleusDelay(1);
   }

   unsigned drain_loops = 0;
   while (written > 0 && g_serial->IsTransmitting()) {
      if (++drain_loops > 2000000) {
         break;
      }
      CTimer::SimpleusDelay(1);
   }

   return written;
}

static void logm(const char *msg) {
   if (g_serial) {
      SerialWriteAll(msg, strlen(msg));
   }
}

static void logi(int i) {
   char nn[16];
   itoa2(i,nn);
   if (g_serial) {
      SerialWriteAll(nn, strlen(nn));
   }
}

struct CirclePath {
   CirclePath(const char* p) {
      ok = false;
      error = 0;
      path[0] = '\0';

      if (p == nullptr) {
         error = EFAULT;
         return;
      }

      size_t len = strlen(p);
      if (len == 0) {
         ok = true;
         return;
      }

      if (is_absolute_path(p)) {
         // Absolute
         if (!copy_string(path, sizeof path, p)) {
            error = ENAMETOOLONG;
            return;
         }
         strip_trailing_slash(path);
         ok = true;
         return;
      } 

      // Relative
      if (!copy_string(path, sizeof path, currentDir)) {
         error = ENAMETOOLONG;
         return;
      }
      if (len == 1 && p[0] == '.') {
         // Treat as current dir
         strip_trailing_slash(path);
         ok = true;
         return;
      }

      // Handle ./ at start but we don't in the middle.
      const char *relative = p;
      if (len >= 2 && p[0] == '.' && p[1] == '/') {
         relative = p + 2;
      }

      if (!append_path_component(path, sizeof path, relative)) {
         error = ENAMETOOLONG;
         return;
      }

      // Fat fs doesn't like trailing slashes for dirs
      strip_trailing_slash(path);
      ok = true;
   }

   bool ok;
   int error;
   char path[BMC64_PATH_MAX];
};

struct CircleFile {
  FIL file;
  int in_use;
  char fname[BMC64_PATH_MAX];

  char *contents;       // bytes for a bounded in-memory file
  unsigned allocated;  // bytes allocated for contents
  unsigned size;       // logical file size
  unsigned position;   // logical file position
  int mode;            // O_RDONLY, O_WRONLY, or O_RDWR
  int flags;           // complete open flags
  int written_to;      // at least one write was performed
  int cached;          // contents/position back I/O instead of FIL
  int fopen_called;    // FIL is open and must be closed
};

struct CircleDir {
  CircleDir() {
    mEntry.d_ino = 0;
    mEntry.d_name[0] = 0;
    dir.pat = pattern;
    in_use = 0;
    last_entry = nullptr;
    last_size = 0;
    last_fat_attributes = 0;
    last_info_valid = 0;
  }

  FATFS_DIR dir;
  int in_use;
  struct dirent mEntry;
  const struct dirent *last_entry;
  uint64_t last_size;
  unsigned int last_fat_attributes;
  int last_info_valid;
};

CircleFile fileTab[MAX_OPEN_FILES];
CircleDir dirTab[MAX_OPEN_DIRS];
static CGlueIOStats g_ioStats;

extern "C" int _close(int fildes);

void CGlueStdioInit(CSerialDevice *serial) {
  g_serial = serial;
  setvbuf(stdout, nullptr, _IONBF, 0);
  setvbuf(stderr, nullptr, _IONBF, 0);

  // Initialize stdio, stderr and stdin
  fileTab[0].in_use = 1;
  fileTab[1].in_use = 1;
  fileTab[2].in_use = 1;

  copy_string(currentDir, sizeof currentDir, "/");
  CGlueStdioResetIOStats();
}

void CGlueStdioGetIOStats(CGlueIOStats *stats) {
  if (stats != nullptr) {
    *stats = g_ioStats;
  }
}

void CGlueStdioResetIOStats(void) {
  memset(&g_ioStats, 0, sizeof g_ioStats);
#if BMC64_IO_STATS
  for (unsigned int i = 3; i < MAX_OPEN_FILES; ++i) {
    if (fileTab[i].in_use) {
      ++g_ioStats.open_files;
    }
  }
  g_ioStats.peak_open_files = g_ioStats.open_files;
#endif
}

static int g_bootStatNum = 0;
static int *g_bootStatWhat;
static const char **g_bootStatFile;
static int *g_bootStatSize;

// Set global vars pointing to bootstat info
void CGlueStdioInitBootStat (int num,
        int *bootStatWhat,
        const char **bootStatFile,
        int *bootStatSize) {
   g_bootStatNum = num;
   g_bootStatWhat = bootStatWhat;
   g_bootStatFile = bootStatFile;
   g_bootStatSize = bootStatSize;
}

int CGlueStdioShutdown(void) {
  int errors = 0;

  fflush(nullptr);

  for (unsigned int i = 3; i < MAX_OPEN_FILES; ++i) {
    if (fileTab[i].in_use && _close(i) != 0) {
      ++errors;
    }
  }

  for (unsigned int i = 0; i < MAX_OPEN_DIRS; ++i) {
    if (dirTab[i].in_use && f_closedir(&dirTab[i].dir) != FR_OK) {
      ++errors;
    }
    dirTab[i].in_use = 0;
  }

  return errors == 0 ? 0 : -1;
}

static int FindFreeFileSlot(void) {
  int slotNr = -1;

  for (const CircleFile &slot : fileTab) {
    if (slot.in_use == 0) {
      slotNr = &slot - fileTab;
      break;
    }
  }

  return slotNr;
}

static char *strdup2(const char *s) {
  char *d = (char *)malloc(strlen(s) + 1);
  if (d == nullptr)
    return nullptr;
  strcpy(d, s);
  return d;
}

static void set_fatfs_errno(FRESULT result) {
  switch (result) {
    case FR_NO_FILE:
    case FR_INVALID_NAME:
      errno = ENOENT;
      break;
    case FR_NO_PATH:
    case FR_INVALID_DRIVE:
      errno = ENOTDIR;
      break;
    case FR_EXIST:
      errno = EEXIST;
      break;
    case FR_NOT_ENOUGH_CORE:
      errno = ENOMEM;
      break;
    case FR_TOO_MANY_OPEN_FILES:
      errno = ENFILE;
      break;
    case FR_DENIED:
    case FR_WRITE_PROTECTED:
      errno = EACCES;
      break;
    case FR_INVALID_OBJECT:
      errno = EBADF;
      break;
    default:
      errno = EIO;
      break;
  }
}

static FRESULT measured_seek(FIL *file, FSIZE_t offset) {
#if BMC64_IO_STATS
  const uint64_t begin = CTimer::GetClockTicks64();
#endif
  const FRESULT result = f_lseek(file, offset);
#if BMC64_IO_STATS
  const uint64_t elapsed = CTimer::GetClockTicks64() - begin;
  ++g_ioStats.seek_calls;
  g_ioStats.seek_us += elapsed;
  if (elapsed > g_ioStats.seek_max_us) g_ioStats.seek_max_us = elapsed;
  if (result != FR_OK) ++g_ioStats.io_errors;
#endif
  return result;
}

static FRESULT measured_read(FIL *file, void *buffer, UINT bytes,
                             UINT *bytes_read) {
#if BMC64_IO_STATS
  const uint64_t begin = CTimer::GetClockTicks64();
#endif
  const FRESULT result = f_read(file, buffer, bytes, bytes_read);
#if BMC64_IO_STATS
  const uint64_t elapsed = CTimer::GetClockTicks64() - begin;
  ++g_ioStats.read_calls;
  if (result == FR_OK) g_ioStats.read_bytes += *bytes_read;
  else ++g_ioStats.io_errors;
  g_ioStats.read_us += elapsed;
  if (elapsed > g_ioStats.read_max_us) g_ioStats.read_max_us = elapsed;
#endif
  return result;
}

static FRESULT measured_write(FIL *file, const void *buffer, UINT bytes,
                              UINT *bytes_written) {
#if BMC64_IO_STATS
  const uint64_t begin = CTimer::GetClockTicks64();
#endif
  const FRESULT result = f_write(file, buffer, bytes, bytes_written);
#if BMC64_IO_STATS
  const uint64_t elapsed = CTimer::GetClockTicks64() - begin;
  ++g_ioStats.write_calls;
  if (result == FR_OK) g_ioStats.write_bytes += *bytes_written;
  else ++g_ioStats.io_errors;
  g_ioStats.write_us += elapsed;
  if (elapsed > g_ioStats.write_max_us) g_ioStats.write_max_us = elapsed;
#endif
  return result;
}

static bool sync_file(CircleFile &file) {
#if BMC64_IO_STATS
  ++g_ioStats.sync_calls;
#endif
  const FRESULT result = f_sync(&file.file);
  if (result == FR_OK) return true;
#if BMC64_IO_STATS
  ++g_ioStats.io_errors;
#endif
  set_fatfs_errno(result);
  return false;
}

static void note_file_open() {
#if BMC64_IO_STATS
  ++g_ioStats.open_files;
  if (g_ioStats.open_files > g_ioStats.peak_open_files) {
    g_ioStats.peak_open_files = g_ioStats.open_files;
  }
#endif
}

static void note_file_close(int fildes) {
#if BMC64_IO_STATS
  if (fildes >= 3 && g_ioStats.open_files > 0) --g_ioStats.open_files;
#else
  (void) fildes;
#endif
}

static int ensure_file_capacity(CircleFile &file, unsigned required) {
  if (required <= file.allocated) {
    return 0;
  }
  if (required > BMC64_FILE_CACHE_LIMIT) {
    errno = EFBIG;
    return -1;
  }

  unsigned new_allocated = file.allocated ? file.allocated : 1024U;
  while (new_allocated < required) {
    if (new_allocated > BMC64_FILE_CACHE_LIMIT / 2U) {
      new_allocated = BMC64_FILE_CACHE_LIMIT;
    } else {
      new_allocated *= 2U;
    }
  }

  char *new_contents;
  if (file.contents == nullptr) {
    new_contents = (char *)malloc(new_allocated);
  } else {
    new_contents = (char *)realloc(file.contents, new_allocated);
  }

  if (new_contents == nullptr) {
    errno = ENOMEM;
    return -1;
  }

  file.contents = new_contents;
  file.allocated = new_allocated;
  return 0;
}

static void release_file_contents(CircleFile &file) {
  if (file.contents != nullptr) {
    free(file.contents);
    file.contents = nullptr;
  }
  file.allocated = 0;
}


static int FindFreeDirSlot(void) {
  int slotNr = -1;

  for (const CircleDir &slot : dirTab) {
    if (!slot.in_use) {
      slotNr = &slot - dirTab;
      break;
    }
  }

  return slotNr;
}

static CircleDir *FindCircleDirFromDIR(DIR *dir) {
  for (CircleDir &slot : dirTab) {
    if (slot.in_use && dir == reinterpret_cast<DIR *>(&slot.dir)) {
      return &slot;
    }
  }
  return nullptr;
}

static bool read_exact(FIL *file, char *buffer, unsigned size) {
  unsigned received = 0;
  while (received < size) {
    UINT count = 0;
    const UINT request = static_cast<UINT>(size - received);
    const FRESULT result =
        measured_read(file, buffer + received, request, &count);
    if (result != FR_OK) {
      set_fatfs_errno(result);
      return false;
    }
    if (count == 0) {
      errno = EIO;
      return false;
    }
    received += count;
  }
  return true;
}

static bool write_exact(FIL *file, const char *buffer, unsigned size) {
  unsigned written = 0;
  while (written < size) {
    UINT count = 0;
    const UINT request = static_cast<UINT>(size - written);
    const FRESULT result =
        measured_write(file, buffer + written, request, &count);
    if (result != FR_OK) {
      set_fatfs_errno(result);
      return false;
    }
    if (count == 0) {
      errno = EIO;
      return false;
    }
    written += count;
  }
  return true;
}

// Cache a known-small file with one allocation. The logical position is
// preserved; only the FatFs position is changed while filling the cache.
static bool cache_file(CircleFile &file) {
  if (file.cached) return true;
  if (file.size > BMC64_FILE_CACHE_LIMIT) return false;

#if BMC64_IO_STATS
  const uint64_t begin = CTimer::GetClockTicks64();
  ++g_ioStats.cached_open_calls;
  if (file.size != 0) ++g_ioStats.slurp_calls;
#endif

  char *contents = nullptr;
  if (file.size != 0) {
    contents = static_cast<char *>(malloc(file.size));
    if (contents == nullptr) {
      errno = ENOMEM;
      return false;
    }
    if (measured_seek(&file.file, 0) != FR_OK ||
        !read_exact(&file.file, contents, file.size)) {
      free(contents);
      if (errno == 0) errno = EIO;
      return false;
    }
  }

  file.contents = contents;
  file.allocated = file.size;
  file.cached = 1;
#if BMC64_IO_STATS
  const uint64_t elapsed = CTimer::GetClockTicks64() - begin;
  if (file.size != 0) {
    g_ioStats.slurp_bytes += file.size;
    g_ioStats.slurp_us += elapsed;
    if (elapsed > g_ioStats.slurp_max_us) g_ioStats.slurp_max_us = elapsed;
  }
#endif
  return true;
}

static bool flush_cached_file(CircleFile &file) {
  if (!file.cached || !file.written_to) return true;
  if (measured_seek(&file.file, 0) != FR_OK) {
    errno = EIO;
    return false;
  }
  if (file.size != 0 && !write_exact(&file.file, file.contents, file.size)) {
    return false;
  }
  if (measured_seek(&file.file, file.size) != FR_OK) {
    errno = EIO;
    return false;
  }
  const FRESULT truncate_result = f_truncate(&file.file);
  if (truncate_result != FR_OK) {
    set_fatfs_errno(truncate_result);
    return false;
  }
  return sync_file(file);
}

static bool spill_cache(CircleFile &file) {
  if (!file.cached) return true;
  if (!flush_cached_file(file)) return false;
  release_file_contents(file);
  file.cached = 0;
  if (measured_seek(&file.file, file.position) != FR_OK) {
    errno = EIO;
    return false;
  }
#if BMC64_IO_STATS
  ++g_ioStats.cache_spills;
  ++g_ioStats.direct_open_calls;
#endif
  return true;
}

static BYTE fatfs_open_flags(int flags, int mode) {
  BYTE result = mode == O_RDONLY
                    ? FA_READ
                    : mode == O_WRONLY ? FA_WRITE : FA_READ | FA_WRITE;

  if (flags & O_APPEND) {
    result |= FA_OPEN_APPEND;
  } else if ((flags & O_CREAT) && (flags & O_EXCL)) {
    result |= FA_CREATE_NEW;
  } else if (flags & O_TRUNC) {
    result |= FA_CREATE_ALWAYS;
  } else if (flags & O_CREAT) {
    result |= FA_OPEN_ALWAYS;
  }
  return result;
}

extern "C" int _open(char *file, int flags, int mode) {
  (void) mode;
  if (file == nullptr) {
    errno = EFAULT;
    return -1;
  }

  int const masked_flags = flags & O_ACCMODE;
  if (masked_flags != O_RDONLY && masked_flags != O_WRONLY &&
      masked_flags != O_RDWR) {
    errno = ENOSYS;
    return -1;
  }

  // Handle fast fail here
  for (int i=0;i<g_bootStatNum;i++) {
     if (g_bootStatWhat[i] == BOOTSTAT_WHAT_FAIL) {
        if (strend(file, g_bootStatFile[i])) {
          errno = EACCES;
          return -1;
        }
     }
  }
  int slot = FindFreeFileSlot();

  if (slot != -1) {
    CirclePath circlePath(file);
    if (!circlePath.ok) {
      errno = circlePath.error;
      return -1;
    }

    CircleFile &newFile = fileTab[slot];
    memset(&newFile.file, 0, sizeof newFile.file);
    newFile.fopen_called = 0;
    newFile.contents = nullptr;
    newFile.position = 0;
    newFile.size = 0;
    newFile.allocated = 0;
    newFile.mode = masked_flags;
    newFile.flags = flags;
    newFile.written_to = 0;
    newFile.cached = 0;
    newFile.fname[0] = '\0';

    const FRESULT result = f_open(
        &newFile.file, circlePath.path,
        fatfs_open_flags(flags, masked_flags));
    if (result != FR_OK) {
      set_fatfs_errno(result);
      return -1;
    }

    newFile.fopen_called = 1;
    if (!copy_string(newFile.fname, sizeof newFile.fname, circlePath.path)) {
      f_close(&newFile.file);
      errno = ENAMETOOLONG;
      return -1;
    }

    const FSIZE_t fatfs_size = f_size(&newFile.file);
    if (fatfs_size > static_cast<FSIZE_t>(UINT_MAX)) {
      f_close(&newFile.file);
      newFile.fopen_called = 0;
      errno = EFBIG;
      return -1;
    }
    newFile.size = static_cast<unsigned>(fatfs_size);
    if (flags & O_APPEND) {
      newFile.position = newFile.size;
    }

    const bool cache_read_write =
        masked_flags == O_RDWR &&
        newFile.size <= BMC64_FILE_CACHE_LIMIT;
    const bool cache_truncated_write =
        masked_flags == O_WRONLY && (flags & O_TRUNC) != 0;
    if ((cache_read_write || cache_truncated_write) &&
        !cache_file(newFile)) {
      f_close(&newFile.file);
      release_file_contents(newFile);
      newFile.fopen_called = 0;
      return -1;
    }
#if BMC64_IO_STATS
    if (!newFile.cached &&
        (newFile.size > BMC64_FILE_CACHE_LIMIT ||
         masked_flags != O_RDONLY)) {
      ++g_ioStats.direct_open_calls;
    }
#endif

    newFile.in_use = 1;
    note_file_open();
  } else {
    errno = ENFILE;
  }

  return slot;
}

extern "C" int _close(int fildes) {
  if (fildes < 0 || static_cast<unsigned int>(fildes) >= MAX_OPEN_FILES) {
    errno = EBADF;
    return -1;
  }

  CircleFile &file = fileTab[fildes];
  if (!file.in_use) {
    errno = EBADF;
    return -1;
  }

  bool ok = file.cached ? flush_cached_file(file)
                        : !file.written_to || sync_file(file);
  int saved_errno = ok ? 0 : errno;
  if (file.fopen_called) {
    const FRESULT result = f_close(&file.file);
    if (result != FR_OK) {
      if (ok) set_fatfs_errno(result);
      saved_errno = errno;
      ok = false;
    }
  }

  release_file_contents(file);
  file.allocated = 0;
  file.size = 0;
  file.position = 0;
  file.mode = 0;
  file.flags = 0;
  file.in_use = 0;
  note_file_close(fildes);
  file.written_to = 0;
  file.cached = 0;
  file.fopen_called = 0;
  file.fname[0] = '\0';

  if (!ok) {
    errno = saved_errno == 0 ? EIO : saved_errno;
    return -1;
  }

  return 0;
}

extern "C" int _read(int fildes, char *ptr, int len) {
  if (fildes < 0 || static_cast<unsigned int>(fildes) >= MAX_OPEN_FILES) {
    errno = EBADF;
    return -1;
  }
  if (len < 0) {
    errno = EINVAL;
    return -1;
  }
  if (len > 0 && ptr == nullptr) {
    errno = EFAULT;
    return -1;
  }

  CircleFile &file = fileTab[fildes];
  if (!file.in_use) {
    errno = EBADF;
    return -1;
  }
  if (file.mode == O_WRONLY) {
    errno = EBADF;
    return -1;
  }

  unsigned int num_read;
  if (!file.cached) {
     const FRESULT result = measured_read(
         &file.file, ptr, static_cast<UINT>(len), &num_read);
     if (result != FR_OK) {
       set_fatfs_errno(result);
       return -1;
     }

     file.position += num_read;
     return static_cast<int>(num_read);
  } else {
     // Read data from our internal buffer
     unsigned int max = len;
     unsigned int remain = file.position <= file.size ?
                           file.size - file.position : 0;

     if (max > remain) {
        max = remain;
     }

     if (max > 0) {
        memcpy(ptr, file.contents + file.position, max);
        file.position += max;
     }
     return static_cast<int>(max);
  }
}

extern "C" int _write(int fildes, char *ptr, int len) {
  if (fildes < 0 || static_cast<unsigned int>(fildes) >= MAX_OPEN_FILES) {
    errno = EBADF;
    return -1;
  }
  if (len < 0) {
    errno = EINVAL;
    return -1;
  }
  if (len > 0 && ptr == nullptr) {
    errno = EFAULT;
    return -1;
  }

  if (fildes == 1 || fildes == 2) {
    bmx::remote::CaptureDeveloperLog(ptr, static_cast<size_t>(len));
    if (g_serial) {
       return SerialWriteAll(ptr, len);
    } 
    return len;
  }

  CircleFile &file = fileTab[fildes];
  if (!file.in_use) {
    errno = EBADF;
    return -1;
  }
  if (file.mode == O_RDONLY) {
    errno = EBADF;
    return -1;
  }
  if (len == 0) return 0;

  if (file.flags & O_APPEND) {
    file.position = file.size;
    if (!file.cached && measured_seek(&file.file, file.position) != FR_OK) {
      errno = EIO;
      return -1;
    }
  }

  if ((unsigned)len > UINT_MAX - file.position) {
     errno = EFBIG;
     return -1;
  }
  unsigned required = file.position + (unsigned)len;

  if (file.cached && required > BMC64_FILE_CACHE_LIMIT &&
      !spill_cache(file)) {
    return -1;
  }

  if (file.cached) {
    if (ensure_file_capacity(file, required) != 0) return -1;
    memcpy(file.contents + file.position, ptr, len);
    file.position += static_cast<unsigned>(len);
  } else {
    UINT written = 0;
    const FRESULT result = measured_write(
        &file.file, ptr, static_cast<UINT>(len), &written);
    if (result != FR_OK) {
      set_fatfs_errno(result);
      return -1;
    }
    file.position += written;
    if (written == 0) {
      errno = EIO;
      return -1;
    }
    len = static_cast<int>(written);
  }
  if (file.position > file.size) {
     file.size = file.position;
  }
  file.written_to = 1;

  return len;
}

extern "C" DIR *BMX_NEW_IO_POSIX(opendir)(const char *name) {
  CirclePath circlePath(name); 
  if (!circlePath.ok) {
    errno = circlePath.error;
    return 0;
  }
  
  int const slotNum = FindFreeDirSlot();
  if (slotNum == -1) {
    errno = ENFILE;
    return 0;
  }

  CircleDir &slot = dirTab[slotNum];
  const FRESULT result = f_opendir(&slot.dir, circlePath.path);
  if (result != FR_OK) {
    set_fatfs_errno(result);
    return 0;
  }

  slot.in_use = 1;
  slot.last_entry = nullptr;
  slot.last_info_valid = 0;
  return reinterpret_cast<DIR *>(&slot.dir);
}

static struct dirent *do_readdir(CircleDir *dir, struct dirent *de) {

  assert(dir->in_use);

  FILINFO fno;
  struct dirent *result = nullptr;

  dir->last_entry = nullptr;
  dir->last_info_valid = 0;

  FRESULT res = f_findnext(&dir->dir, &fno);
  if (res == FR_OK && fno.fname[0] != 0) {
    snprintf(de->d_name, sizeof de->d_name, "%s", fno.fname);
    de->d_ino = (ino_t)(BMC64_DIRENT_FAT_ATTR_VALID | fno.fattrib);
    dir->last_entry = de;
    dir->last_size = static_cast<uint64_t>(fno.fsize);
    dir->last_fat_attributes = fno.fattrib;
    dir->last_info_valid = 1;
    result = de;
  }

  return result;
}

extern "C" struct dirent *BMX_NEW_IO_POSIX(readdir)(DIR *dir) {
  CircleDir *c_dir = FindCircleDirFromDIR(dir);
  if (c_dir == nullptr) {
    errno = EBADF;
    return nullptr;
  }

  return do_readdir(c_dir, &c_dir->mEntry);
}

extern "C" int bmx_readdir_get_info(void *directory, const void *entry,
                                      bmx_dirent_info_t *info) {
  if (directory == nullptr || entry == nullptr || info == nullptr) {
    return 0;
  }

  CircleDir *c_dir = FindCircleDirFromDIR(reinterpret_cast<DIR *>(directory));
  if (c_dir == nullptr || !c_dir->last_info_valid ||
      c_dir->last_entry != entry) {
    return 0;
  }

  info->size = c_dir->last_size;
  info->fat_attributes = c_dir->last_fat_attributes;
  return 1;
}

extern "C" int BMX_NEW_IO_POSIX(readdir_r)(DIR *__restrict dir,
                                              dirent *__restrict de,
                                              dirent **__restrict ode) {
  int result;
  CircleDir *c_dir = FindCircleDirFromDIR(dir);

  if (c_dir == nullptr) {
    *ode = nullptr;
    result = EBADF;
  } else {
    *ode = do_readdir(c_dir, de);
    result = 0;
  }

  return result;
}

extern "C" void BMX_NEW_IO_POSIX(rewinddir)(DIR *dir) {
  CircleDir *c_dir = FindCircleDirFromDIR(dir);
  if (c_dir != nullptr) {
    f_rewinddir(&c_dir->dir);
    c_dir->last_entry = nullptr;
    c_dir->last_info_valid = 0;
  }
}

extern "C" int BMX_NEW_IO_POSIX(closedir)(DIR *dir) {
  CircleDir *c_dir = FindCircleDirFromDIR(dir);
  if (c_dir == nullptr) {
    errno = EBADF;
    return -1;
  }

  c_dir->in_use = 0;
  c_dir->last_entry = nullptr;
  c_dir->last_info_valid = 0;

  if (f_closedir(&c_dir->dir) != FR_OK) {
    errno = EIO;
    return -1;
  }

  return 0;
}

extern "C" int _stat(const char *file, struct stat *st) {
  if (st == nullptr) {
    errno = EFAULT;
    return -1;
  }

  CirclePath circlePath(file);
  if (!circlePath.ok) {
    errno = circlePath.error;
    return -1;
  }

  memset(st, 0, sizeof(struct stat));

  // Fastfail or fastsucceed
  for (int i=0;i<g_bootStatNum;i++) {
     if (g_bootStatWhat[i] == BOOTSTAT_WHAT_STAT) {
        if (strend(circlePath.path, g_bootStatFile[i])) {
           st->st_mode = S_IFREG | S_IREAD | S_IWRITE;
           st->st_size = g_bootStatSize[i];
           return 0;
        }
     }
     else if (g_bootStatWhat[i] == BOOTSTAT_WHAT_FAIL) {
        if (strend(circlePath.path, g_bootStatFile[i])) {
          errno = EBADF;
          return -1;
        }
     }
  }

  FILINFO fno;
  if (f_stat(circlePath.path, &fno) == FR_OK) {
    if (fno.fattrib & AM_DIR) {
      st->st_mode |= S_IFDIR;
    } else {
      st->st_mode |= S_IFREG;
    }
    if (fno.fattrib & AM_RDO) {
      st->st_mode |= S_IREAD;
    } else {
      st->st_mode |= S_IREAD | S_IWRITE;
    }

    st->st_size = fno.fsize;
    return 0;
  }

  errno = EBADF;
  return -1;
}

extern "C" int access(const char *path, int mode) {
  if (path == nullptr) {
    errno = EFAULT;
    return -1;
  }

  CirclePath circlePath(path);
  if (!circlePath.ok) {
    errno = circlePath.error;
    return -1;
  }

  FILINFO fno;
  if (f_stat(circlePath.path, &fno) != FR_OK) {
    errno = ENOENT;
    return -1;
  }

  if (mode & X_OK) {
    errno = EACCES;
    return -1;
  }

  return 0;
}

extern "C" int _access(const char *path, int mode) {
  return access(path, mode);
}

extern "C" int _fstat(int fildes, struct stat *st) {
  if (st == nullptr) {
    errno = EFAULT;
    return -1;
  }
  if (fildes < 0 || static_cast<unsigned int>(fildes) >= MAX_OPEN_FILES) {
    errno = EBADF;
    return -1;
  }

  CircleFile &file = fileTab[fildes];
  if (!file.in_use) {
    errno = EBADF;
    return -1;
  }

  if (_stat(file.fname, st) != 0) return -1;
  st->st_size = file.size;
  return 0;
}

extern "C" int fsync(int fildes) {
  if (fildes < 0 || static_cast<unsigned int>(fildes) >= MAX_OPEN_FILES) {
    errno = EBADF;
    return -1;
  }

  CircleFile &file = fileTab[fildes];
  if (!file.in_use || file.mode == O_RDONLY) {
    errno = EBADF;
    return -1;
  }

  const bool ok = file.cached ? flush_cached_file(file) : sync_file(file);
  if (!ok) return -1;
  file.written_to = 0;
  return 0;
}

extern "C" int _lseek(int fildes, int ptr, int dir) {

  if (fildes < 0 || static_cast<unsigned int>(fildes) >= MAX_OPEN_FILES) {
    errno = EBADF;
    return -1;
  }

  CircleFile &file = fileTab[fildes];
  if (!file.in_use) {
    errno = EBADF;
    return -1;
  }

  long long next_position;
  if (dir == SEEK_SET) {
    next_position = ptr;
  } else if (dir == SEEK_CUR) {
    next_position = (long long)file.position + ptr;
  } else if (dir == SEEK_END) {
    next_position = (long long)file.size + ptr;
  } else {
    errno = EINVAL;
    return -1;
  }

  if (next_position < 0 || next_position > (long long)file.size) {
    errno = EINVAL;
    return -1;
  }

  if (file.mode == O_RDONLY && !file.cached &&
      file.size <= BMC64_FILE_CACHE_LIMIT) {
    if (!cache_file(file) && errno != ENOMEM) return -1;
    if (!file.cached) errno = 0;
  }

  if (!file.cached &&
      measured_seek(&file.file, static_cast<FSIZE_t>(next_position)) !=
          FR_OK) {
    errno = EIO;
    return -1;
  }
  file.position = static_cast<unsigned>(next_position);
  return file.position;
}

extern "C" int BMX_NEW_IO_POSIX(chdir)(const char *path) {
  if (path == nullptr) {
    errno = EFAULT;
    return -1;
  }

  size_t len = strlen(path);
  if (len == 0) {
    return 0;
  }

  if (len == 1 && path[0] == '.') {
    return 0;
  }

  // Up to parent
  if (len == 2 && path[0] == '.' && path[1] == '.') {
    move_current_dir_to_parent();
    return 0;
  }

  CirclePath circlePath(path);
  if (!circlePath.ok) {
    errno = circlePath.error;
    return -1;
  }

  if (!is_root_path(circlePath.path)) {
    FILINFO info;
    const FRESULT result = f_stat(circlePath.path, &info);
    if (result != FR_OK) {
      set_fatfs_errno(result);
      return -1;
    }
    if (!(info.fattrib & AM_DIR)) {
      errno = ENOTDIR;
      return -1;
    }
  }

  if (!copy_string(currentDir, sizeof currentDir, circlePath.path)) {
    errno = ENAMETOOLONG;
    return -1;
  }

  return 0;
}

extern "C" char *BMX_NEW_IO_POSIX(getcwd)(char *buf, size_t size) {
  if (buf == nullptr) {
    errno = EFAULT;
    return nullptr;
  }

  const size_t required = strlen(currentDir) + 1;
  if (size < required) {
    errno = ERANGE;
    return nullptr;
  }

  memcpy(buf, currentDir, required);
  strip_trailing_slash(buf);
  return buf;
}

extern "C" char *BMX_NEW_IO_POSIX(getwd)(char *buf) {
  return BMX_NEW_IO_POSIX(getcwd)(buf, BMC64_PATH_MAX);
}

extern "C" int BMX_NEW_IO_POSIX(mkdir)(const char *path, mode_t mode) {
  (void)mode;

  CirclePath circlePath(path);
  if (!circlePath.ok) {
    errno = circlePath.error;
    return -1;
  }

  const FRESULT result = f_mkdir(circlePath.path);
  if (result != FR_OK) {
    set_fatfs_errno(result);
    return -1;
  }
  return 0;
}

extern "C" int BMX_NEW_IO_POSIX(rmdir)(const char *path) {
  CirclePath circlePath(path);
  if (!circlePath.ok) {
    errno = circlePath.error;
    return -1;
  }

  FILINFO info;
  FRESULT result = f_stat(circlePath.path, &info);
  if (result != FR_OK) {
    set_fatfs_errno(result);
    return -1;
  }
  if (!(info.fattrib & AM_DIR)) {
    errno = ENOTDIR;
    return -1;
  }

  result = f_unlink(circlePath.path);
  if (result != FR_OK) {
    set_fatfs_errno(result);
    return -1;
  }
  return 0;
}

extern "C" int _link(char *existing, char *newname) {
  CirclePath existingPath(existing);
  CirclePath newPath(newname);
  if (!existingPath.ok) {
     errno = existingPath.error;
     return -1;
  }
  if (!newPath.ok) {
     errno = newPath.error;
     return -1;
  }

  int result = f_rename(existingPath.path, newPath.path);
  if (result != FR_OK) {
     if (result == FR_EXIST) errno = EEXIST;
     else errno = EBADF;
     return -1;
  }
  return 0;
}

extern "C" int _unlink(char *name) {
  CirclePath circlePath(name);
  if (!circlePath.ok) {
     errno = circlePath.error;
     return -1;
  }

  if (f_unlink(circlePath.path) != FR_OK) {
     errno = EBADF;
     return -1;
  }
  return 0;
}

extern "C" int _isatty(int fildes) {
  return fildes >= 0 && fildes <= 2 ? 1 : 0;
}
