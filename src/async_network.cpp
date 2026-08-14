//
// async_network.cpp
//

#include "bmc64_async_network.h"

extern "C" {
#include "bmc64_log.h"
#include "vicesocket.h"
}

#include <circle/spinlock.h>

#include <errno.h>
#include <string.h>
#include <sys/socket.h>

namespace {

constexpr unsigned kMaxConnections = 8;
constexpr unsigned kQueueSize = 16384;
constexpr unsigned kTxChunkSize = 256;
constexpr unsigned kRxChunkSize = 1600;
class SpinGuard {
public:
  explicit SpinGuard(CSpinLock &lock) : m_lock(lock) { m_lock.Acquire(); }
  ~SpinGuard() { m_lock.Release(); }

private:
  CSpinLock &m_lock;

  SpinGuard(const SpinGuard &);
  SpinGuard &operator=(const SpinGuard &);
};

class ByteQueue {
public:
  ByteQueue() : m_mutex(TASK_LEVEL), m_head(0), m_tail(0), m_count(0) {}

  unsigned Push(const uint8_t *data, unsigned len) {
    if (data == 0 || len == 0) {
      return 0;
    }

    m_mutex.Acquire();
    unsigned written = 0;
    while (written < len && m_count < kQueueSize) {
      m_data[m_tail] = data[written++];
      m_tail = (m_tail + 1) % kQueueSize;
      ++m_count;
    }
    m_mutex.Release();
    return written;
  }

  unsigned Pop(uint8_t *data, unsigned len) {
    if (data == 0 || len == 0) {
      return 0;
    }

    m_mutex.Acquire();
    unsigned read = 0;
    while (read < len && m_count > 0) {
      data[read++] = m_data[m_head];
      m_head = (m_head + 1) % kQueueSize;
      --m_count;
    }
    m_mutex.Release();
    return read;
  }

  unsigned Peek(uint8_t *data, unsigned len) {
    if (data == 0 || len == 0) {
      return 0;
    }

    m_mutex.Acquire();
    unsigned read = 0;
    unsigned pos = m_head;
    while (read < len && read < m_count) {
      data[read++] = m_data[pos];
      pos = (pos + 1) % kQueueSize;
    }
    m_mutex.Release();
    return read;
  }

  unsigned Drop(unsigned len) {
    if (len == 0) {
      return 0;
    }

    m_mutex.Acquire();
    unsigned dropped = 0;
    while (dropped < len && m_count > 0) {
      m_head = (m_head + 1) % kQueueSize;
      --m_count;
      ++dropped;
    }
    m_mutex.Release();
    return dropped;
  }

  unsigned Count() {
    m_mutex.Acquire();
    unsigned count = m_count;
    m_mutex.Release();
    return count;
  }

  unsigned Free() {
    m_mutex.Acquire();
    unsigned free = kQueueSize - m_count;
    m_mutex.Release();
    return free;
  }

  void Snapshot(unsigned *count, unsigned *free) {
    m_mutex.Acquire();
    if (count != 0) {
      *count = m_count;
    }
    if (free != 0) {
      *free = kQueueSize - m_count;
    }
    m_mutex.Release();
  }

  void Clear() {
    m_mutex.Acquire();
    m_head = 0;
    m_tail = 0;
    m_count = 0;
    m_mutex.Release();
  }

private:
  CSpinLock m_mutex;
  uint8_t m_data[kQueueSize];
  unsigned m_head;
  unsigned m_tail;
  unsigned m_count;
};

struct ConnectionSlot {
  ConnectionSlot()
      : mutex(TASK_LEVEL), inUse(0), releaseRequested(0),
        stopRequested(0), connectStarted(0),
        generation(0), state(BMC64_ASYNC_NET_CLOSED), error(0), socket(0),
        txDroppedBytes(0), rxDroppedBytes(0), rxFullSkips(0) {
    target[0] = '\0';
  }

  CSpinLock mutex;
  int inUse;
  int releaseRequested;
  int stopRequested;
  int connectStarted;
  unsigned generation;
  int state;
  int error;
  vice_network_socket_t *socket;
  char target[256];
  ByteQueue rx;
  ByteQueue tx;
  unsigned long txDroppedBytes;
  unsigned long rxDroppedBytes;
  unsigned long rxFullSkips;
};

ConnectionSlot g_slots[kMaxConnections];
unsigned g_nextGeneration = 1;
CSpinLock g_generationLock(TASK_LEVEL);

unsigned NextGeneration() {
  SpinGuard guard(g_generationLock);
  unsigned generation = g_nextGeneration++;
  if (g_nextGeneration == 0) {
    g_nextGeneration = 1;
  }
  return generation;
}

struct AsyncSlotSnapshot {
  int valid;
  int inUse;
  int releaseRequested;
  int stopRequested;
  int connectStarted;
  unsigned generation;
  int state;
  int error;
  vice_network_socket_t *socket;
  unsigned rxPending;
  unsigned rxFree;
  unsigned txPending;
  unsigned txFree;
  unsigned long txDroppedBytes;
  unsigned long rxDroppedBytes;
  unsigned long rxFullSkips;
};

#if BMC64_RS232_LOG_LEVEL >= BMC64_LOG_EVENT
const char *AsyncStateName(int state) {
  switch (state) {
    case BMC64_ASYNC_NET_CONNECTING:
      return "connecting";
    case BMC64_ASYNC_NET_CONNECTED:
      return "connected";
    case BMC64_ASYNC_NET_CLOSED:
      return "closed";
    case BMC64_ASYNC_NET_ERROR:
      return "error";
    default:
      return "unknown";
  }
}

const char *AsyncErrnoName(int err) {
  switch (err) {
    case ETIMEDOUT:
      return "ETIMEDOUT";
    case ECONNREFUSED:
      return "ECONNREFUSED";
    case EHOSTUNREACH:
      return "EHOSTUNREACH";
    case ENETUNREACH:
      return "ENETUNREACH";
    case ECONNRESET:
      return "ECONNRESET";
    case ENOPROTOOPT:
      return "ENOPROTOOPT";
    case EIO:
      return "EIO";
    case EINVAL:
      return "EINVAL";
    default:
      return "unknown";
  }
}
#endif

void SnapshotSlot(ConnectionSlot &slot, AsyncSlotSnapshot *snapshot,
                  int includeQueues) {
  memset(snapshot, 0, sizeof(*snapshot));
  {
    SpinGuard guard(slot.mutex);
    snapshot->valid = 1;
    snapshot->inUse = slot.inUse;
    snapshot->releaseRequested = slot.releaseRequested;
    snapshot->stopRequested = slot.stopRequested;
    snapshot->connectStarted = slot.connectStarted;
    snapshot->generation = slot.generation;
    snapshot->state = slot.state;
    snapshot->error = slot.error;
    snapshot->socket = slot.socket;
    snapshot->txDroppedBytes = slot.txDroppedBytes;
    snapshot->rxDroppedBytes = slot.rxDroppedBytes;
    snapshot->rxFullSkips = slot.rxFullSkips;
  }

  if (includeQueues) {
    slot.rx.Snapshot(&snapshot->rxPending, &snapshot->rxFree);
    slot.tx.Snapshot(&snapshot->txPending, &snapshot->txFree);
  }
}

int SnapshotHandle(const bmc64_async_net_handle_t *handle,
                   AsyncSlotSnapshot *snapshot, int includeQueues);

void AddTxDropped(ConnectionSlot &slot, unsigned long bytes,
                  unsigned long *total) {
  SpinGuard guard(slot.mutex);
  slot.txDroppedBytes += bytes;
  if (total != 0) {
    *total = slot.txDroppedBytes;
  }
}

void AddRxDropped(ConnectionSlot &slot, unsigned long bytes,
                  unsigned long *total) {
  SpinGuard guard(slot.mutex);
  slot.rxDroppedBytes += bytes;
  if (total != 0) {
    *total = slot.rxDroppedBytes;
  }
}

void AddRxFullSkip(ConnectionSlot &slot, unsigned long *total) {
  SpinGuard guard(slot.mutex);
  ++slot.rxFullSkips;
  if (total != 0) {
    *total = slot.rxFullSkips;
  }
}

#ifdef BMC64_DEBUG_PROFILE
unsigned g_asyncTxPartialCount = 0;
unsigned g_asyncTxWouldBlockCount = 0;
unsigned g_asyncTxErrorCount = 0;
unsigned g_asyncTxDropCount = 0;
unsigned g_asyncRxDropCount = 0;
unsigned g_asyncRxFullSkipCount = 0;

int AsyncDebugShouldLog(unsigned count) {
  return count <= 8 || (count % 256) == 0;
}
#endif

class AsyncNetWorker {
public:
  void Process(void) {
    for (unsigned i = 0; i < kMaxConnections; ++i) {
      ProcessSlot(g_slots[i]);
    }
  }

private:
  void ProcessSlot(ConnectionSlot &slot) {
    AsyncSlotSnapshot snapshot;

    SnapshotSlot(slot, &snapshot, 0);
    if (!snapshot.inUse) {
      return;
    }

    if (snapshot.releaseRequested && snapshot.socket == 0 &&
        snapshot.state != BMC64_ASYNC_NET_CONNECTING) {
      FreeSlot(slot);
      return;
    }

    if (snapshot.state == BMC64_ASYNC_NET_CONNECTING &&
        !snapshot.connectStarted) {
      Connect(slot);
      return;
    }

    if (snapshot.state == BMC64_ASYNC_NET_CONNECTED) {
      PumpConnected(slot);
      return;
    }

    if (snapshot.releaseRequested &&
        (snapshot.state == BMC64_ASYNC_NET_CLOSED ||
         snapshot.state == BMC64_ASYNC_NET_ERROR)) {
      FreeSlot(slot);
    }
  }

  void Connect(ConnectionSlot &slot) {
    char target[sizeof(slot.target)];
    vice_network_socket_address_t *address = 0;
    vice_network_socket_t *socket = 0;
    int closeAbandoned = 0;

    {
      SpinGuard guard(slot.mutex);
      slot.connectStarted = 1;
      strncpy(target, slot.target, sizeof(target) - 1);
      target[sizeof(target) - 1] = '\0';
    }

    address = vice_network_address_generate(target, 0);
    if (address == 0) {
      int err = vice_network_get_errorcode();
      BMC64_RS232_EVENT("async resolve failed target %s err %d %s",
                        target, err, AsyncErrnoName(err));
      SpinGuard guard(slot.mutex);
      slot.error = err;
      slot.state = BMC64_ASYNC_NET_ERROR;
      return;
    }

    socket = vice_network_client(address);
    int connectError = socket == 0 ? vice_network_get_errorcode() : 0;
    vice_network_address_close(address);
    if (socket == 0) {
      BMC64_RS232_EVENT("async connect failed target %s err %d %s",
                        target, connectError, AsyncErrnoName(connectError));
    }

    {
      SpinGuard guard(slot.mutex);
      if (slot.stopRequested || socket == 0) {
        if (socket != 0) {
          closeAbandoned = 1;
        } else {
          slot.error = connectError;
          slot.state = BMC64_ASYNC_NET_ERROR;
        }
        slot.socket = 0;
      }

      if (!closeAbandoned && socket != 0) {
        slot.socket = socket;
        slot.state = BMC64_ASYNC_NET_CONNECTED;
        slot.error = 0;
      }
    }

    if (closeAbandoned) {
      int closeResult = vice_network_socket_close(socket);
      BMC64_RS232_EVENT("async close abandoned target %s result %d",
                        target, closeResult);
      (void)closeResult;

      SpinGuard guard(slot.mutex);
      slot.state = BMC64_ASYNC_NET_CLOSED;
      slot.error = 0;
      return;
    }
    if (socket == 0) {
      return;
    }
    BMC64_RS232_EVENT("async connected target %s", target);
  }

  vice_network_socket_t *SocketForConnected(ConnectionSlot &slot,
                                            int *stopRequested) {
    SpinGuard guard(slot.mutex);
    if (stopRequested != 0) {
      *stopRequested = slot.stopRequested;
    }
    if (slot.state != BMC64_ASYNC_NET_CONNECTED || slot.socket == 0) {
      return 0;
    }
    return slot.socket;
  }

  void PumpConnected(ConnectionSlot &slot) {
    uint8_t tx[kTxChunkSize];
    uint8_t rx[kRxChunkSize];
    vice_network_socket_t *socket;
    int stopRequested = 0;

    socket = SocketForConnected(slot, &stopRequested);
    if (stopRequested) {
      CloseSocket(slot, BMC64_ASYNC_NET_CLOSED, 0, "stop");
      return;
    }
    if (socket == 0) {
      return;
    }

    unsigned txLen = slot.tx.Peek(tx, sizeof(tx));
    if (txLen > 0) {
      ssize_t sent = vice_network_send(socket, tx, txLen, MSG_DONTWAIT);
      int err = vice_network_get_errorcode();
      if (sent > 0) {
        slot.tx.Drop(static_cast<unsigned>(sent));
#ifdef BMC64_DEBUG_PROFILE
        if (sent < static_cast<ssize_t>(txLen)) {
          ++g_asyncTxPartialCount;
          if (AsyncDebugShouldLog(g_asyncTxPartialCount)) {
            BMC64_RS232_EVENT("async tx partial #%u sent %ld/%u queued %u",
                              g_asyncTxPartialCount, (long)sent, txLen,
                              slot.tx.Count());
          }
        }
#endif
      } else if (sent < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
#ifdef BMC64_DEBUG_PROFILE
          ++g_asyncTxWouldBlockCount;
          if (AsyncDebugShouldLog(g_asyncTxWouldBlockCount)) {
            BMC64_RS232_EVENT("async tx would-block #%u queued %u err %d",
                              g_asyncTxWouldBlockCount, slot.tx.Count(), err);
          }
#endif
        } else {
#ifdef BMC64_DEBUG_PROFILE
          ++g_asyncTxErrorCount;
          BMC64_RS232_EVENT("async tx error #%u err %d",
                            g_asyncTxErrorCount, err);
#else
          BMC64_RS232_EVENT("async tx error err %d", err);
#endif
          CloseSocket(slot, BMC64_ASYNC_NET_ERROR, err, "tx-error");
          return;
        }
      }
    }

    if (slot.rx.Count() <= kQueueSize - kRxChunkSize) {
      int ready = vice_network_select_poll_one(socket);
      if (ready < 0) {
        BMC64_RS232_EVENT("async poll error err %d", vice_network_get_errorcode());
        CloseSocket(slot, BMC64_ASYNC_NET_ERROR, vice_network_get_errorcode(),
                    "poll-error");
        return;
      }
      if (ready > 0) {
        ssize_t received =
            vice_network_receive(socket, rx, sizeof(rx), MSG_DONTWAIT);
        if (received < 0) {
          if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
          }
          BMC64_RS232_EVENT("async rx error err %d", vice_network_get_errorcode());
          CloseSocket(slot, BMC64_ASYNC_NET_ERROR,
                      vice_network_get_errorcode(), "rx-error");
          return;
        }
        if (received == 0) {
          BMC64_RS232_EVENT("async remote closed");
          CloseSocket(slot, BMC64_ASYNC_NET_CLOSED, 0, "remote-closed");
          return;
        }
        unsigned pushed = slot.rx.Push(rx, static_cast<unsigned>(received));
        if (pushed < static_cast<unsigned>(received)) {
          unsigned long rxDropped = 0;
          AddRxDropped(slot, static_cast<unsigned long>(received) - pushed,
                       &rxDropped);
#ifdef BMC64_DEBUG_PROFILE
          ++g_asyncRxDropCount;
          if (AsyncDebugShouldLog(g_asyncRxDropCount)) {
            BMC64_RS232_EVENT("async rx drop #%u pushed %u/%ld dropped %lu queued %u",
                              g_asyncRxDropCount, pushed, (long)received,
                              rxDropped, slot.rx.Count());
          }
#endif
        }
      }
    } else {
      unsigned long rxFullSkips = 0;
      AddRxFullSkip(slot, &rxFullSkips);
#ifdef BMC64_DEBUG_PROFILE
      ++g_asyncRxFullSkipCount;
      if (AsyncDebugShouldLog(g_asyncRxFullSkipCount)) {
        BMC64_RS232_EVENT("async rx full-skip #%u queued %u free %u",
                          g_asyncRxFullSkipCount, slot.rx.Count(),
                          slot.rx.Free());
      }
#endif
      (void)rxFullSkips;
    }
  }

  void CloseSocket(ConnectionSlot &slot, int state, int error,
                   const char *reason) {
    vice_network_socket_t *socket = 0;
    char target[sizeof(slot.target)];
    unsigned generation;

    {
      SpinGuard guard(slot.mutex);
      socket = slot.socket;
      slot.socket = 0;
      generation = slot.generation;
      strncpy(target, slot.target, sizeof(target) - 1);
      target[sizeof(target) - 1] = '\0';
    }

    if (socket != 0) {
      int closeResult = vice_network_socket_close(socket);
      BMC64_RS232_EVENT("async close gen %u reason %s state %s err %d result %d target %s",
                        generation, reason ? reason : "unspecified",
                        AsyncStateName(state), error, closeResult, target);
      (void)closeResult;
    } else {
      BMC64_RS232_EVENT("async close gen %u reason %s state %s err %d no-socket target %s",
                        generation, reason ? reason : "unspecified",
                        AsyncStateName(state), error, target);
    }
    (void)generation;

    {
      SpinGuard guard(slot.mutex);
      slot.error = error;
      slot.state = state;
    }
  }

  void FreeSlot(ConnectionSlot &slot) {
    vice_network_socket_t *socket = 0;
    char target[sizeof(slot.target)];
    unsigned generation;

    {
      SpinGuard guard(slot.mutex);
      socket = slot.socket;
      slot.socket = 0;
      generation = slot.generation;
      strncpy(target, slot.target, sizeof(target) - 1);
      target[sizeof(target) - 1] = '\0';
      slot.rx.Clear();
      slot.tx.Clear();
      slot.target[0] = '\0';
      slot.releaseRequested = 0;
      slot.stopRequested = 0;
      slot.connectStarted = 0;
      slot.state = BMC64_ASYNC_NET_CLOSED;
      slot.error = 0;
      slot.inUse = 0;
    }

    if (socket != 0) {
      int closeResult = vice_network_socket_close(socket);
      BMC64_RS232_EVENT("async free closed lingering socket gen %u result %d target %s",
                        generation, closeResult, target);
      (void)closeResult;
    } else {
      BMC64_RS232_EVENT("async free gen %u target %s", generation, target);
    }
    (void)generation;
  }
};

AsyncNetWorker g_worker;
bool g_workerInitialized = false;
CSpinLock g_workerLock(TASK_LEVEL);

bool WorkerAvailable() {
  SpinGuard guard(g_workerLock);
  return g_workerInitialized;
}

ConnectionSlot *HandleSlotIfValid(const bmc64_async_net_handle_t *handle);

} // namespace

struct bmc64_async_net_handle_s {
  ConnectionSlot *slot;
  unsigned generation;
};

namespace {

int SnapshotHandle(const bmc64_async_net_handle_t *handle,
                   AsyncSlotSnapshot *snapshot, int includeQueues) {
  if (snapshot != 0) {
    memset(snapshot, 0, sizeof(*snapshot));
  }
  if (handle == 0 || handle->slot == 0 || snapshot == 0) {
    return 0;
  }

  ConnectionSlot *slot = handle->slot;
  {
    SpinGuard guard(slot->mutex);
    if (!slot->inUse || slot->generation != handle->generation) {
      return 0;
    }
    snapshot->valid = 1;
    snapshot->inUse = slot->inUse;
    snapshot->releaseRequested = slot->releaseRequested;
    snapshot->stopRequested = slot->stopRequested;
    snapshot->connectStarted = slot->connectStarted;
    snapshot->generation = slot->generation;
    snapshot->state = slot->state;
    snapshot->error = slot->error;
    snapshot->socket = slot->socket;
    snapshot->txDroppedBytes = slot->txDroppedBytes;
    snapshot->rxDroppedBytes = slot->rxDroppedBytes;
    snapshot->rxFullSkips = slot->rxFullSkips;
  }

  if (includeQueues) {
    slot->rx.Snapshot(&snapshot->rxPending, &snapshot->rxFree);
    slot->tx.Snapshot(&snapshot->txPending, &snapshot->txFree);
  }
  return 1;
}

ConnectionSlot *HandleSlotIfValid(const bmc64_async_net_handle_t *handle) {
  AsyncSlotSnapshot snapshot;
  if (!SnapshotHandle(handle, &snapshot, 0)) {
    return 0;
  }
  return handle->slot;
}

} // namespace

extern "C" bmc64_async_net_handle_t *
bmc64_async_net_start(const char *target) {
  if (target == 0 || target[0] == '\0') {
    return 0;
  }

  if (!WorkerAvailable()) {
    return 0;
  }

  for (unsigned i = 0; i < kMaxConnections; ++i) {
    ConnectionSlot &slot = g_slots[i];
    SpinGuard guard(slot.mutex);
    if (!slot.inUse) {
      bmc64_async_net_handle_t *handle = new bmc64_async_net_handle_t;
      if (handle == 0) {
        return 0;
      }

      slot.rx.Clear();
      slot.tx.Clear();
      strncpy(slot.target, target, sizeof(slot.target) - 1);
      slot.target[sizeof(slot.target) - 1] = '\0';
      slot.generation = NextGeneration();
      slot.error = 0;
      slot.socket = 0;
      slot.txDroppedBytes = 0;
      slot.rxDroppedBytes = 0;
      slot.rxFullSkips = 0;
      slot.releaseRequested = 0;
      slot.stopRequested = 0;
      slot.connectStarted = 0;
      slot.state = BMC64_ASYNC_NET_CONNECTING;
      slot.inUse = 1;

      handle->slot = &slot;
      handle->generation = slot.generation;
      return handle;
    }
  }

  return 0;
}

extern "C" void bmc64_async_net_stop(bmc64_async_net_handle_t *handle) {
  if (handle == 0) {
    return;
  }

  if (handle->slot != 0) {
    ConnectionSlot *slot = handle->slot;
    SpinGuard guard(slot->mutex);
    if (slot->inUse && slot->generation == handle->generation) {
      slot->stopRequested = 1;
      slot->releaseRequested = 1;
      BMC64_RS232_EVENT("async stop request gen %u state %s socket %d target %s",
                        slot->generation, AsyncStateName(slot->state),
                        slot->socket ? 1 : 0, slot->target);
    }
  }

  delete handle;
}

extern "C" int bmc64_async_net_initialize(void) {
  SpinGuard guard(g_workerLock);
  g_workerInitialized = true;
  return 1;
}

extern "C" void bmc64_async_net_process(void) {
  if (WorkerAvailable()) {
    g_worker.Process();
  }
}

extern "C" int bmc64_async_net_status(bmc64_async_net_handle_t *handle) {
  AsyncSlotSnapshot snapshot;
  if (!SnapshotHandle(handle, &snapshot, 0)) {
    return BMC64_ASYNC_NET_CLOSED;
  }
  return snapshot.state;
}

extern "C" int bmc64_async_net_error(bmc64_async_net_handle_t *handle) {
  AsyncSlotSnapshot snapshot;
  if (!SnapshotHandle(handle, &snapshot, 0)) {
    return 0;
  }
  return snapshot.error;
}

extern "C" int bmc64_async_net_send(bmc64_async_net_handle_t *handle,
                                     const uint8_t *data, size_t len) {
  ConnectionSlot *slot;

  if (data == 0 || len == 0) {
    return 0;
  }
  slot = HandleSlotIfValid(handle);
  if (slot == 0) {
    return 0;
  }
  if (len > 0xffffffffu) {
    len = 0xffffffffu;
  }
  unsigned requested = static_cast<unsigned>(len);
  unsigned pushed = slot->tx.Push(data, requested);
  if (pushed < requested) {
    unsigned long txDropped = 0;
    AddTxDropped(*slot, requested - pushed, &txDropped);
#ifdef BMC64_DEBUG_PROFILE
    ++g_asyncTxDropCount;
    if (AsyncDebugShouldLog(g_asyncTxDropCount)) {
      BMC64_RS232_EVENT("async tx queue-full #%u pushed %u/%u dropped %lu queued %u",
                        g_asyncTxDropCount, pushed, requested,
                        txDropped, slot->tx.Count());
    }
#endif
  }
  return static_cast<int>(pushed);
}

extern "C" int bmc64_async_net_receive(bmc64_async_net_handle_t *handle,
                                        uint8_t *data, size_t len) {
  ConnectionSlot *slot;

  if (data == 0 || len == 0) {
    return 0;
  }
  slot = HandleSlotIfValid(handle);
  if (slot == 0) {
    return 0;
  }
  if (len > 0xffffffffu) {
    len = 0xffffffffu;
  }
  return static_cast<int>(slot->rx.Pop(data, static_cast<unsigned>(len)));
}

extern "C" size_t bmc64_async_net_rx_pending(bmc64_async_net_handle_t *handle) {
  AsyncSlotSnapshot snapshot;
  if (!SnapshotHandle(handle, &snapshot, 1)) {
    return 0;
  }
  return snapshot.rxPending;
}

extern "C" size_t bmc64_async_net_rx_free(bmc64_async_net_handle_t *handle) {
  AsyncSlotSnapshot snapshot;
  if (!SnapshotHandle(handle, &snapshot, 1)) {
    return 0;
  }
  return snapshot.rxFree;
}

extern "C" size_t bmc64_async_net_tx_pending(bmc64_async_net_handle_t *handle) {
  AsyncSlotSnapshot snapshot;
  if (!SnapshotHandle(handle, &snapshot, 1)) {
    return 0;
  }
  return snapshot.txPending;
}

extern "C" size_t bmc64_async_net_tx_free(bmc64_async_net_handle_t *handle) {
  AsyncSlotSnapshot snapshot;
  if (!SnapshotHandle(handle, &snapshot, 1)) {
    return 0;
  }
  return snapshot.txFree;
}

extern "C" unsigned long
bmc64_async_net_tx_dropped(bmc64_async_net_handle_t *handle) {
  AsyncSlotSnapshot snapshot;
  if (!SnapshotHandle(handle, &snapshot, 0)) {
    return 0;
  }
  return snapshot.txDroppedBytes;
}

extern "C" unsigned long
bmc64_async_net_rx_dropped(bmc64_async_net_handle_t *handle) {
  AsyncSlotSnapshot snapshot;
  if (!SnapshotHandle(handle, &snapshot, 0)) {
    return 0;
  }
  return snapshot.rxDroppedBytes;
}
