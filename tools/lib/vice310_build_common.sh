#!/bin/bash

# Shared incremental Pi4/Pi5 VICE 3.10 and BMX build implementation.
# The board wrappers set BMX_BUILD_BOARD and SRC_DIR before sourcing this file.

set -euo pipefail

if [ -z "${SRC_DIR:-}" ] || [ -z "${BMX_BUILD_BOARD:-}" ]; then
  echo "vice310_build_common.sh requires SRC_DIR and BMX_BUILD_BOARD" >&2
  return 2 2>/dev/null || exit 2
fi

# The public wrappers expose BMX-specific configuration variables.  Generic
# compiler/Make overrides would otherwise silently bypass the fingerprinted
# paths or change Circle output without entering the cache key, so normalize
# them before loading any generated Makefiles.
for _bmx_ambient_name in \
  CC CXX CPP AS LD AR RANLIB STRIP OBJCOPY OBJDUMP NM READELF SIZE ARFLAGS \
  CFLAGS CXXFLAGS CPPFLAGS AFLAGS ASFLAGS LDFLAGS LDLIBS LIBS \
  GNUMAKEFLAGS MAKE MAKEFLAGS MAKEFILES MAKELEVEL MFLAGS MAKEOVERRIDES \
  CROSS_COMPILE HOSTCC HOSTCXX CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH \
  OBJC_INCLUDE_PATH LIBRARY_PATH COMPILER_PATH GCC_EXEC_PREFIX \
  OPTIMIZE STANDARD C_STANDARD WARNINGS DEFINE INCLUDE EXTRAINCLUDE \
  EXTRALIBS EXTRA_LIBS \
  CLANG AARCH ARCH ARCHCPU RASPPI PREFIX PREFIX64 FLOAT_ABI GC_SECTIONS \
  GZIP_KERNEL KASAN_ENABLED KASAN_SHADOW_MAPPING_OFFSET \
  KASAN_SANITIZE_STACK KASAN_SANITIZE_GLOBALS STDLIB_SUPPORT CHECK_DEPS \
  HOST_BUILD CIRCLEHOME CIRCLE_STDLIB_HOME CIRCLE_STDLIB_INSTALL_DIR \
  VICE VICE_SOURCE VICE_ARCH VICE_SHARED VICE_INCLUDE_DIRS RESID_IMPL \
  TARGET TARGET_BASENAME BUILD_ROOT BUILD_BOARD BUILD_DIR \
  BMX_COMMON_BUILD_DIR BMC64_COMMON_LIB NEWLIBDIR BOARD MACHINE_CLASS; do
  unset "$_bmx_ambient_name"
done
unset _bmx_ambient_name

. "$SRC_DIR/tools/lib/build_paths.sh"
. "$SRC_DIR/tools/lib/circle_patches.sh"
. "$SRC_DIR/tools/lib/circle_source_archive.sh"
. "$SRC_DIR/tools/lib/mbedtls_source_archive.sh"
. "$SRC_DIR/tools/lib/circle_target_build.sh"

mkdir -p -- "$BMC64_BUILD_ROOT"
BMC64_BUILD_ROOT="$(cd "$BMC64_BUILD_ROOT" && pwd)"

TOOLS_BIN="$SRC_DIR/tools/autotools-stubs/bin"
CIRCLE_STDLIB_SOURCE_ARCHIVE="$SRC_DIR/third_party/source-cache/circle-stdlib-v20-a4fbed9b-full.tar.gz"
CIRCLE_STDLIB_SOURCE_SHA256="$SRC_DIR/third_party/source-cache/SHA256SUMS"
CIRCLE_STDLIB_PATCH_DIR="$SRC_DIR/third_party/circle-stdlib-patches"
TOOLCHAIN_ROOT="$SRC_DIR/.toolchains"
VICE_DIR="$SRC_DIR/third_party/vice-3.10"
VICE_SRC="$VICE_DIR/src"
BMX_BUILD_JOBS="${BMX_BUILD_JOBS:-$(nproc)}"
BMX_GENERATE_LISTING="${BMX_GENERATE_LISTING:-0}"

case "$BMX_BUILD_JOBS" in
  ''|*[!0-9]*|0) echo "BMX_BUILD_JOBS must be a positive integer" >&2; return 2 ;;
esac
case "$BMX_GENERATE_LISTING" in
  0|1) ;;
  *) echo "BMX_GENERATE_LISTING must be exactly 0 or 1" >&2; return 2 ;;
esac

case "$BMX_BUILD_BOARD" in
  pi4)
    NEWLIB_SUBDIR="install/arm-none-circle"
    TOOLCHAIN_NAME="arm-gnu-toolchain-14.2.rel1-x86_64-arm-none-eabi"
    TOOLCHAIN_URL_BASE="https://developer.arm.com/-/media/Files/downloads/gnu/14.2.rel1/binrel"
    TOOL_PREFIX="arm-none-eabi-"
    VICE_HOST="arm-none-eabi"
    CIRCLE_CONFIG_ARGS=(
      --raspberrypi=4 --kernel-max-size 48
      --option ARM_ALLOW_MULTI_CORE --option USE_USB_SOF_INTR
      --opt-tls --prefix arm-none-eabi-
    )
    VICE_ARCH_FLAGS="-march=armv8-a -mtune=cortex-a72 -marm -mfpu=neon-fp-armv8 -mfloat-abi=hard"
    VICE_C_WARNING_FLAGS=" -Wno-incompatible-pointer-types"
    TARGET_BASENAME="kernel7l"
    TARGET_AARCH=32
    ;;
  pi5)
    NEWLIB_SUBDIR="install/aarch64-none-circle"
    TOOLCHAIN_NAME="arm-gnu-toolchain-15.2.rel1-x86_64-aarch64-none-elf"
    TOOLCHAIN_URL_BASE="https://developer.arm.com/-/media/Files/downloads/gnu/15.2.rel1/binrel"
    TOOL_PREFIX="aarch64-none-elf-"
    VICE_HOST="aarch64-none-elf"
    CIRCLE_CONFIG_ARGS=(
      --raspberrypi=5 --aarch64 --kernel-max-size 48
      --option ARM_ALLOW_MULTI_CORE --option USE_USB_SOF_INTR
      --opt-tls --prefix aarch64-none-elf-
    )
    VICE_ARCH_FLAGS="-mcpu=cortex-a76 -mlittle-endian"
    VICE_C_WARNING_FLAGS=""
    TARGET_BASENAME="kernel_2712"
    TARGET_AARCH=64
    ;;
  *)
    echo "unsupported BMX_BUILD_BOARD: $BMX_BUILD_BOARD" >&2
    return 2
    ;;
esac

BMC64_BUILD_PROFILE="${BMC64_BUILD_PROFILE:-release}"
case "$BMC64_BUILD_PROFILE" in
  release|debug) ;;
  *) echo "BMC64_BUILD_PROFILE must be release or debug" >&2; return 2 ;;
esac
BMX_BUILD_CLEAN="${BMX_BUILD_CLEAN:-0}"
case "$BMX_BUILD_CLEAN" in
  0|1) ;;
  *) echo "BMX_BUILD_CLEAN must be exactly 0 or 1" >&2; return 2 ;;
esac
BMX_UPDATE_UPDATER_ABI="${BMX_UPDATE_UPDATER_ABI:-2}"
if [ "$BMX_BUILD_BOARD" = pi5 ]; then
  BMX_PI5_SID_WORKER="${BMX_PI5_SID_WORKER:-1}"
else
  BMX_PI5_SID_WORKER="${BMX_PI5_SID_WORKER:-0}"
fi
BMX_PI5_SID_DIAGNOSTICS="${BMX_PI5_SID_DIAGNOSTICS:-0}"
case "$BMX_PI5_SID_WORKER" in 0|1) ;; *) echo "BMX_PI5_SID_WORKER must be exactly 0 or 1" >&2; return 2 ;; esac
case "$BMX_PI5_SID_DIAGNOSTICS" in 0|1) ;; *) echo "BMX_PI5_SID_DIAGNOSTICS must be exactly 0 or 1" >&2; return 2 ;; esac
if [ "$BMX_BUILD_BOARD" = pi4 ] && \
   [ "$BMX_PI5_SID_WORKER$BMX_PI5_SID_DIAGNOSTICS" != 00 ]; then
  echo "Pi5 SID worker and diagnostics are not supported in Pi4 builds" >&2
  return 2
fi

# Keep every configuration input visible to child make processes even when a
# caller sourced the board adapter and assigned (rather than exported) it.
export BMC64_BUILD_PROFILE BMC64_MENU_LOG_LEVEL BMC64_RS232_LOG_LEVEL \
  BMC64_ACIA_LOG_LEVEL BMC64_TCP_LOG_LEVEL BMC64_NET_LOG_LEVEL \
  BMC64_WLAN_TRACE BMC64_WLAN_LOW_IMPACT_TRACE \
  BMC64_WLAN_LOW_IMPACT_TRACE_LOG BMX_PI5_SID_WORKER \
  BMX_PI5_SID_DIAGNOSTICS BMX_UPDATE_UPDATER_ABI BMX_UPDATE_TEST_CHANNEL \
  BMX_UPDATE_TEST_REPOSITORY_OWNER BMX_UPDATE_TEST_REPOSITORY_NAME \
  BMX_UPDATE_HARDWARE_TEST_MODE BMX_UPDATE_SIMPLE_PRODUCTION \
  BMX_UPDATE_OWNER_DRAFT_TEST

TOOLCHAIN_DIR="$TOOLCHAIN_ROOT/$TOOLCHAIN_NAME"
TOOLCHAIN_TARBALL="$TOOLCHAIN_NAME.tar.xz"
TOOLCHAIN_URL="$TOOLCHAIN_URL_BASE/$TOOLCHAIN_TARBALL"

bmx_hash_stream() {
  sha256sum | awk '{print $1}'
}

bmx_build_value() {
  local name="$1"
  printf '%s=%s\n' "$name" "${!name-}"
}

bmx_variant_hash() {
  local name
  {
    printf 'format=4\nboard=%s\nprofile=%s\ntoolchain=%s\n' \
      "$BMX_BUILD_BOARD" "$BMC64_BUILD_PROFILE" "$BMX_TOOLCHAIN_FINGERPRINT"
    printf 'source-root=%s\ncircle-input=%s\n' "$SRC_DIR" "$BMX_CIRCLE_INPUT_HASH"
    for name in \
      BMC64_MENU_LOG_LEVEL BMC64_RS232_LOG_LEVEL BMC64_ACIA_LOG_LEVEL \
      BMC64_TCP_LOG_LEVEL BMC64_NET_LOG_LEVEL BMC64_WLAN_TRACE \
      BMC64_WLAN_LOW_IMPACT_TRACE BMC64_WLAN_LOW_IMPACT_TRACE_LOG \
      BMX_PI5_SID_WORKER BMX_PI5_SID_DIAGNOSTICS \
      BMX_UPDATE_UPDATER_ABI BMX_UPDATE_TEST_CHANNEL \
      BMX_UPDATE_TEST_REPOSITORY_OWNER BMX_UPDATE_TEST_REPOSITORY_NAME \
      BMX_UPDATE_HARDWARE_TEST_MODE BMX_UPDATE_SIMPLE_PRODUCTION \
      BMX_UPDATE_OWNER_DRAFT_TEST; do
      bmx_build_value "$name"
    done
    sha256sum \
      "$SRC_DIR/Makefile" \
      "$SRC_DIR/mk/machines/Makefile-310.common" \
      "$SRC_DIR"/mk/machines/Makefile-*-310 \
      "$SRC_DIR/mk/vice310-targets.mk" \
      "$SRC_DIR/third_party/common/Makefile" \
      "$SRC_DIR/third_party/common/Config.mk" \
      "${BASH_SOURCE[0]}"
  } | bmx_hash_stream
}

bmx_initialize_build_layout() {
  [ -n "${BMX_TOOLCHAIN_FINGERPRINT:-}" ] || {
    echo "toolchain must be selected before initializing the build layout" >&2
    return 1
  }

  BMX_CIRCLE_INPUT_HASH="$(bmx_circle_input_hash)"
  BMX_CIRCLE_BUILD_VARIANT="$BMC64_BUILD_PROFILE-${BMX_CIRCLE_INPUT_HASH:0:16}"
  CIRCLE_STDLIB_HOME="$BMC64_BUILD_ROOT/$BMX_BUILD_BOARD/circle-variants/$BMX_CIRCLE_BUILD_VARIANT/circle-stdlib"
  NEWLIBDIR="$CIRCLE_STDLIB_HOME/$NEWLIB_SUBDIR"

  BMX_VARIANT_HASH="$(bmx_variant_hash)"
  BMX_BUILD_VARIANT="${BMC64_BUILD_VARIANT:-$BMC64_BUILD_PROFILE-${BMX_VARIANT_HASH:0:16}}"
  if [[ ! "$BMX_BUILD_VARIANT" =~ ^[A-Za-z0-9._-]+$ ]] || \
     [ "$BMX_BUILD_VARIANT" = . ] || [ "$BMX_BUILD_VARIANT" = .. ]; then
    echo "BMC64_BUILD_VARIANT contains an unsafe path segment" >&2
    return 2
  fi

  BMX_VARIANT_ROOT="$BMC64_BUILD_ROOT/$BMX_BUILD_BOARD/variants/$BMX_BUILD_VARIANT"
  VICE_BUILD_DIR="$BMX_VARIANT_ROOT/vice-3.10"
  VICE_BUILD_SRC="$VICE_BUILD_DIR/src"
  COMMON_BUILD_DIR="$BMX_VARIANT_ROOT/common"
  COMMON_LIB="$COMMON_BUILD_DIR/lib/libbmc64common.a"
  BMX_SHARED_BUILD_DIR="$BMX_VARIANT_ROOT/bmx/common"
  BMX_MACHINE_ROOT="$BMX_VARIANT_ROOT/bmx/machines"
  BMX_CONFIG_STAMP="$BMX_VARIANT_ROOT/config.sha256"
}

bmx_write_variant_stamp() {
  mkdir -p "$BMX_VARIANT_ROOT"
  if [ ! -f "$BMX_CONFIG_STAMP" ] || \
     [ "$(cat "$BMX_CONFIG_STAMP")" != "$BMX_VARIANT_HASH" ]; then
    printf '%s\n' "$BMX_VARIANT_HASH" >"$BMX_CONFIG_STAMP"
  fi
}

bmx_remove_variant_root() {
  local variants_parent="$BMC64_BUILD_ROOT/$BMX_BUILD_BOARD/variants"
  if [ "$(dirname -- "$BMX_VARIANT_ROOT")" != "$variants_parent" ] || \
     [ "$BMX_VARIANT_ROOT" = "$variants_parent" ]; then
    echo "refusing unsafe variant removal: $BMX_VARIANT_ROOT" >&2
    return 1
  fi
  rm -rf -- "$BMX_VARIANT_ROOT"
}

ensure_toolchain() {
  local tool toolchain_complete=1
  export PATH="$TOOLS_BIN:$PATH"

  for tool in gcc g++ ar ranlib ld objcopy objdump nm c++filt strip; do
    command -v "${TOOL_PREFIX}$tool" >/dev/null 2>&1 || toolchain_complete=0
  done
  if [ "$toolchain_complete" -eq 0 ]; then
    mkdir -p "$TOOLCHAIN_ROOT"
    if [ ! -d "$TOOLCHAIN_DIR" ]; then
      curl -L --fail "$TOOLCHAIN_URL" -o "$TOOLCHAIN_ROOT/$TOOLCHAIN_TARBALL"
      tar -C "$TOOLCHAIN_ROOT" -xf "$TOOLCHAIN_ROOT/$TOOLCHAIN_TARBALL"
    fi
    export PATH="$TOOLCHAIN_DIR/bin:$PATH"
  fi

  for tool in gcc g++ ar ranlib ld objcopy objdump nm c++filt strip; do
    command -v "${TOOL_PREFIX}$tool" >/dev/null 2>&1 || {
      echo "incomplete cross toolchain: missing ${TOOL_PREFIX}$tool" >&2
      return 1
    }
  done

  BMX_TOOLCHAIN_FINGERPRINT="$({
    printf 'format=1\nprefix=%s\n' "$TOOL_PREFIX"
    for tool in gcc g++ ar ranlib ld objcopy objdump nm c++filt strip; do
      printf 'tool=%s\n' "$(realpath "$(command -v "${TOOL_PREFIX}$tool")")"
      sha256sum "$(realpath "$(command -v "${TOOL_PREFIX}$tool")")"
    done
  } | bmx_hash_stream)"
}

set_vice310_make_args() {
  local vice_cppflags vice_cflags vice_cxxflags vice_ldflags
  local debug_define=""
  local release_define=""
  local diag_defines=""
  local sid_defines=""
  local sid_worker="${BMX_PI5_SID_WORKER:-1}"
  local sid_diagnostics="${BMX_PI5_SID_DIAGNOSTICS:-0}"

  if [ "$BMX_BUILD_BOARD" = pi4 ]; then
    sid_worker=0
    sid_diagnostics=0
  else
    case "$sid_worker" in 0|1) ;; *) echo "BMX_PI5_SID_WORKER must be exactly 0 or 1" >&2; return 2 ;; esac
    case "$sid_diagnostics" in 0|1) ;; *) echo "BMX_PI5_SID_DIAGNOSTICS must be exactly 0 or 1" >&2; return 2 ;; esac
    sid_defines=" -DBMX_SID_WORKER=$sid_worker -DBMX_SID_DIAGNOSTICS=$sid_diagnostics"
  fi

  if [ "${BMC64_BUILD_PROFILE:-release}" = debug ]; then
    debug_define=" -DBMC64_DEBUG_PROFILE"
  else
    release_define=" -DNDEBUG"
  fi
  [ -z "${BMC64_RS232_LOG_LEVEL:-}" ] || diag_defines="$diag_defines -DBMC64_RS232_LOG_LEVEL=$BMC64_RS232_LOG_LEVEL"
  [ -z "${BMC64_ACIA_LOG_LEVEL:-}" ] || diag_defines="$diag_defines -DBMC64_ACIA_LOG_LEVEL=$BMC64_ACIA_LOG_LEVEL"
  [ -z "${BMC64_TCP_LOG_LEVEL:-}" ] || diag_defines="$diag_defines -DBMC64_TCP_LOG_LEVEL=$BMC64_TCP_LOG_LEVEL"
  [ -z "${BMC64_NET_LOG_LEVEL:-}" ] || diag_defines="$diag_defines -DBMC64_NET_LOG_LEVEL=$BMC64_NET_LOG_LEVEL"

  vice_cppflags="-DRASPI_COMPILE$debug_define$release_define$diag_defines$sid_defines -I$VICE_SRC -I$VICE_SRC/arch/shared -I$VICE_SRC/arch/raspi"
  vice_cflags="-O3 -std=gnu11 -ffreestanding -nostdlib -fno-exceptions$VICE_C_WARNING_FLAGS $VICE_ARCH_FLAGS$debug_define$release_define$diag_defines$sid_defines -I$VICE_SRC -I$SRC_DIR/src -I$SRC_DIR -I$SRC_DIR/third_party/common -I$NEWLIBDIR/include -I$CIRCLE_STDLIB_HOME/include -I$CIRCLE_STDLIB_HOME/libs/circle/addon -I$CIRCLE_STDLIB_HOME/libs/circle/addon/fatfs"
  vice_cxxflags="-O3 -ffreestanding -nostdlib -fno-exceptions -fcheck-new $VICE_ARCH_FLAGS -std=c++11 -fno-rtti -nostdinc++$debug_define$release_define$diag_defines$sid_defines -I$VICE_SRC -I$SRC_DIR/src -I$SRC_DIR -I$SRC_DIR/third_party/common -I$NEWLIBDIR/include -I$CIRCLE_STDLIB_HOME/include -I$CIRCLE_STDLIB_HOME/libs/circle/addon -I$CIRCLE_STDLIB_HOME/libs/circle/addon/fatfs"
  vice_ldflags="-L$NEWLIBDIR/lib"

  vice_configure_args=(
    "--host=$VICE_HOST" --with-fastsid --disable-realdevice --disable-ipv6
    --disable-catweasel --disable-hardsid --disable-parsid
    --without-portaudio --without-lame
    --disable-midi --disable-hidmgr --without-oss
    --without-alsa --without-pulse --without-zlib --without-png
    --without-libcurl --disable-sdl1ui --disable-sdl2ui --enable-raspiui
  )
  vice_make_args=(
    "CC=${TOOL_PREFIX}gcc" "CXX=${TOOL_PREFIX}g++"
    "CPP=${TOOL_PREFIX}gcc -E" "CXXCPP=${TOOL_PREFIX}g++ -E"
    "AR=${TOOL_PREFIX}ar" "RANLIB=${TOOL_PREFIX}ranlib"
    "STRIP=${TOOL_PREFIX}strip" "XA=true" "DOS2UNIX=true"
    "ac_cv_lib_lex=none needed" "ac_cv_search_yywrap=none required"
    "ac_cv_header_arpa_inet_h=yes" "ac_cv_header_netdb_h=yes"
    "ac_cv_header_netinet_in_h=yes" "ac_cv_header_netinet_tcp_h=yes"
    "ac_cv_header_sys_select_h=yes" "ac_cv_header_sys_socket_h=yes"
    "ac_cv_header_sys_time_h=yes" "ac_cv_header_sys_types_h=yes"
    "ac_cv_header_unistd_h=yes" "ac_cv_func_accept=yes"
    "ac_cv_func_bind=yes" "ac_cv_func_connect=yes"
    "ac_cv_func_getaddrinfo=yes" "ac_cv_func_gethostbyname=yes"
    "ac_cv_func_htonl=yes" "ac_cv_func_htons=yes"
    "ac_cv_func_listen=yes" "ac_cv_func_recv=yes" "ac_cv_func_send=yes"
    "ac_cv_func_socket=yes" "CPPFLAGS=$vice_cppflags" "CFLAGS=$vice_cflags"
    "CXXFLAGS=$vice_cxxflags" "LDFLAGS=$vice_ldflags"
  )
}

run_vice310_make() {
  # The pinned VICE tree already contains its generated Autotools files.  Git
  # timestamps can nevertheless make aclocal.m4 newer than config.h.in.  The
  # generated rule then touches config.h.in even though AUTOHEADER is a stub,
  # which remakes config.h after common objects have already consumed it.  Do
  # not run those maintainer-only dependency edges for distribution builds.
  make "$@" "${vice_make_args[@]}" 'am__configure_deps='
}

prepare_vice_source_for_out_of_tree() {
  local lock_file="$SRC_DIR/build/.vice310-source-migration.lock"
  local marker="$SRC_DIR/build/.vice310-source-migration-v2"
  local preserve_dir artifact relative
  local -a preserved_files=(src/debug.h src/version.h src/vice-version.sh)
  local -a legacy_artifacts=()

  if [ -f "$marker" ] && [ ! -f "$VICE_DIR/config.status" ] && \
     [ ! -e "$VICE_SRC/rs232drv/tcpser/bmx_tcpser_adapter.o" ]; then
    return
  fi
  command -v flock >/dev/null 2>&1 || {
    echo "flock is required for one-time VICE in-tree build migration" >&2
    return 1
  }
  mkdir -p "$(dirname "$lock_file")"
  exec 8>"$lock_file"
  flock 8
  if [ -f "$VICE_DIR/config.status" ]; then
    echo "Migrating legacy VICE in-tree artifacts to isolated build directories"
    preserve_dir="$(mktemp -d /tmp/bmx-vice310-preserve.XXXXXX)"
    for relative in "${preserved_files[@]}"; do
      if [ -f "$VICE_DIR/$relative" ]; then
        mkdir -p "$preserve_dir/$(dirname "$relative")"
        cp -p "$VICE_DIR/$relative" "$preserve_dir/$relative"
      fi
    done
    if ! make -C "$VICE_DIR" distclean >/dev/null; then
      for relative in "${preserved_files[@]}"; do
        [ ! -f "$preserve_dir/$relative" ] || \
          cp -p "$preserve_dir/$relative" "$VICE_DIR/$relative"
      done
      rm -rf -- "$preserve_dir"
      return 1
    fi
    for relative in "${preserved_files[@]}"; do
      [ ! -f "$preserve_dir/$relative" ] || \
        cp -p "$preserve_dir/$relative" "$VICE_DIR/$relative"
    done
    rm -rf -- "$preserve_dir"
  fi

  mapfile -d '' legacy_artifacts < <(
    find "$VICE_SRC" -type f \( -name '*.o' -o -name '*.a' \) -print0
  )
  for artifact in "${legacy_artifacts[@]}"; do
    case "$artifact" in
      "$VICE_SRC"/*.o|"$VICE_SRC"/*.a) ;;
      *) echo "refusing unsafe legacy VICE artifact removal: $artifact" >&2; return 1 ;;
    esac
    relative="${artifact#"$SRC_DIR/"}"
    if git -C "$SRC_DIR" ls-files --error-unmatch -- "$relative" >/dev/null 2>&1; then
      echo "refusing to remove tracked VICE artifact: $relative" >&2
      return 1
    fi
  done
  [ "${#legacy_artifacts[@]}" -eq 0 ] || rm -f -- "${legacy_artifacts[@]}"

  flock -u 8
  exec 8>&-
  [ ! -f "$VICE_DIR/config.status" ] || {
    echo "VICE source tree remains configured after distclean" >&2
    return 1
  }
  printf '2\n' >"$marker"
}

vice_config_hash() {
  {
    printf 'format=2\nvariant=%s\n' "$BMX_VARIANT_HASH"
    printf 'configure-args=%s\n' "${vice_configure_args[*]}"
    printf 'make-args=%s\n' "${vice_make_args[*]}"
    find "$VICE_DIR" -type f \( -name 'Makefile.in' -o -name 'config.h.in' \) \
      -print0 | sort -z | xargs -0 sha256sum
    sha256sum "$VICE_DIR/configure" "$SRC_DIR/mk/vice310-targets.mk" \
      "$TOOLS_BIN"/*
  } | bmx_hash_stream
}

preserve_vice310_generated_monitor_parser() {
  local source_dir="$VICE_SRC/monitor"
  local build_dir="$VICE_BUILD_SRC/monitor"
  local name prerequisite
  mkdir -p "$build_dir"
  for name in mon_parse.c mon_parse.h mon_lex.c; do
    if [ ! -f "$build_dir/$name" ] || ! cmp -s "$source_dir/$name" "$build_dir/$name"; then
      cp -p "$source_dir/$name" "$build_dir/$name"
    fi
    prerequisite=mon_parse.y
    [ "$name" != mon_lex.c ] || prerequisite=mon_lex.l
    touch -r "$source_dir/$prerequisite" "$build_dir/$name"
  done
}

generate_vice310_infocontrib() {
  local output="$VICE_BUILD_SRC/infocontrib.h"
  local generator="$VICE_SRC/buildtools/geninfocontrib_h.sh"
  local source="$VICE_DIR/doc/vice.texi"
  local filter="$VICE_SRC/buildtools/infocontrib.sed"
  local temporary work_dir

  if [ -f "$output" ] && [ "$output" -nt "$generator" ] && \
     [ "$output" -nt "$source" ] && [ "$output" -nt "$filter" ] && \
     [ "$output" -nt "$VICE_SRC/vicedate.h" ]; then
    return
  fi
  temporary="$(mktemp "$VICE_BUILD_SRC/.infocontrib.h.XXXXXX")"
  work_dir="$(mktemp -d "$VICE_BUILD_SRC/.infocontrib-work.XXXXXX")"
  if ! (
    cd "$work_dir"
    "$generator" infocontrib.h <"$source" \
      "$(awk '/VICEDATE_YEAR / {print $3; exit}' "$VICE_SRC/vicedate.h")"
  ) | LC_ALL=C sed -f "$filter" | \
      iconv -f ISO-8859-15 -t UTF-8 >"$temporary"; then
    rm -rf -- "$work_dir"
    rm -f "$temporary"
    return 1
  fi
  rm -rf -- "$work_dir"
  mv "$temporary" "$output"
}

configure_vice310() {
  local marker="$VICE_BUILD_DIR/.bmx-config.sha256"
  local wanted
  wanted="$(vice_config_hash)"
  if [ ! -f "$VICE_BUILD_DIR/config.status" ] || \
     [ ! -f "$marker" ] || [ "$(cat "$marker")" != "$wanted" ]; then
    case "$VICE_BUILD_DIR" in
      "$BMX_VARIANT_ROOT/vice-3.10") ;;
      *) echo "refusing unsafe VICE build removal: $VICE_BUILD_DIR" >&2; return 1 ;;
    esac
    rm -rf -- "$VICE_BUILD_DIR"
    mkdir -p "$VICE_BUILD_DIR"
    echo "Configuring isolated VICE tree $VICE_BUILD_DIR"
    (
      cd "$VICE_BUILD_DIR"
      "$VICE_DIR/configure" "${vice_configure_args[@]}" "${vice_make_args[@]}"
    )
    printf '%s\n' "$wanted" >"$marker"
  fi
  mkdir -p "$VICE_BUILD_SRC/rs232drv/tcpser"
  # Automake writes stamp-h1 a few milliseconds after config.h.  GNU Make then
  # considers config.h remade on every invocation even though its recipe is a
  # no-op, which in turn rebuilds every config.h-dependent VICE object.  Equal
  # timestamps retain Automake's dependency contract without that false edge.
  if [ -f "$VICE_BUILD_SRC/config.h" ] && [ -f "$VICE_BUILD_SRC/stamp-h1" ]; then
    touch -r "$VICE_BUILD_SRC/stamp-h1" "$VICE_BUILD_SRC/config.h"
  fi
  preserve_vice310_generated_monitor_parser
  generate_vice310_infocontrib
}

build_common() {
  make -C "$SRC_DIR/third_party/common" -j"$BMX_BUILD_JOBS" \
    BOARD="$BMX_BUILD_BOARD" \
    CIRCLE_STDLIB_HOME="$CIRCLE_STDLIB_HOME" \
    VICE_SOURCE_DIR="$VICE_SRC" VICE_BUILD_SRC="$VICE_BUILD_SRC" \
    BUILD_DIR="$COMMON_BUILD_DIR/obj" LIBRARY="$COMMON_LIB" \
    CONFIG_STAMP="$BMX_CONFIG_STAMP" \
    BMC64_BUILD_PROFILE="${BMC64_BUILD_PROFILE:-release}" \
    BMC64_MENU_LOG_LEVEL="${BMC64_MENU_LOG_LEVEL:-}"
}

vice310_machine_config() {
  local machine="$1"
  case "$machine" in
    c64)
      VICE310_TARGET=x64; VICE310_MAKEFILE=mk/machines/Makefile-C64-310
      VICE310_CLASS=RASPI_C64; VICE310_ARCH_DIR=c64; VICE310_ARCH_LIB=libarch_c64.a
      VICE310_IMAGE_SUFFIX=c64; VICE310_COPY_DEFAULT=1 ;;
    c64sc)
      VICE310_TARGET=x64sc; VICE310_MAKEFILE=mk/machines/Makefile-C64SC-310
      VICE310_CLASS=RASPI_C64SC; VICE310_ARCH_DIR=c64; VICE310_ARCH_LIB=libarch_c64.a
      VICE310_IMAGE_SUFFIX=c64sc; VICE310_COPY_DEFAULT=0 ;;
    scpu64)
      VICE310_TARGET=xscpu64; VICE310_MAKEFILE=mk/machines/Makefile-SCPU64-310
      VICE310_CLASS=RASPI_SCPU64; VICE310_ARCH_DIR=c64; VICE310_ARCH_LIB=libarch_c64.a
      VICE310_IMAGE_SUFFIX=scpu64; VICE310_COPY_DEFAULT=0 ;;
    c128)
      VICE310_TARGET=x128; VICE310_MAKEFILE=mk/machines/Makefile-C128-310
      VICE310_CLASS=RASPI_C128; VICE310_ARCH_DIR=c128; VICE310_ARCH_LIB=libarch_c128.a
      VICE310_IMAGE_SUFFIX=c128; VICE310_COPY_DEFAULT=0 ;;
    vic20)
      VICE310_TARGET=xvic; VICE310_MAKEFILE=mk/machines/Makefile-VIC20-310
      VICE310_CLASS=RASPI_VIC20; VICE310_ARCH_DIR=vic20; VICE310_ARCH_LIB=libarch_vic20.a
      VICE310_IMAGE_SUFFIX=vic20; VICE310_COPY_DEFAULT=0 ;;
    plus4)
      VICE310_TARGET=xplus4; VICE310_MAKEFILE=mk/machines/Makefile-PLUS4-310
      VICE310_CLASS=RASPI_PLUS4; VICE310_ARCH_DIR=plus4; VICE310_ARCH_LIB=libarch_plus4.a
      VICE310_IMAGE_SUFFIX=plus4; VICE310_COPY_DEFAULT=0 ;;
    pet)
      VICE310_TARGET=xpet; VICE310_MAKEFILE=mk/machines/Makefile-PET-310
      VICE310_CLASS=RASPI_PET; VICE310_ARCH_DIR=pet; VICE310_ARCH_LIB=libarch_pet.a
      VICE310_IMAGE_SUFFIX=pet; VICE310_COPY_DEFAULT=0 ;;
    *) echo "unsupported VICE 3.10 machine: $machine" >&2; return 1 ;;
  esac
}

build_vice310_archives() {
  local machine="$1"
  vice310_machine_config "$machine"

  run_vice310_make -C "$VICE_BUILD_SRC" -f Makefile \
    -f "$SRC_DIR/mk/vice310-targets.mk" \
    -j"$BMX_BUILD_JOBS" "bmx-$VICE310_TARGET"
}

vice310_machine_archive_paths() {
  local machine="$1"
  vice310_machine_config "$machine"
  {
    awk -v vice="$VICE_BUILD_SRC" '
      /^\t\$\(VICE\)\// {
        gsub(/\\/, "")
        sub(/^\$\(VICE\)/, vice, $1)
        print $1
      }
    ' "$SRC_DIR/$VICE310_MAKEFILE" | grep '\.a$'
    printf '%s\n' "$VICE_BUILD_SRC/resid/libresid.a" "$VICE_BUILD_SRC/hvsc/libhvsc.a"
  } | sort -u
}

build_vice310_link_archives() {
  local machine archive directory target
  local -a targets
  declare -A targets_by_directory=()

  for machine in "$@"; do
    while IFS= read -r archive; do
      directory="$(dirname "$archive")"
      target="$(basename "$archive")"
      targets_by_directory["$directory"]="${targets_by_directory[$directory]-}$target "
    done < <(vice310_machine_archive_paths "$machine")
  done

  while IFS= read -r directory; do
    read -r -a targets <<<"${targets_by_directory[$directory]}"
    run_vice310_make -C "$directory" -j"$BMX_BUILD_JOBS" "${targets[@]}"
  done < <(printf '%s\n' "${!targets_by_directory[@]}" | sort)
}

bmx_copy_if_changed() {
  local source="$1"
  local destination="$2"
  local temporary
  if [ -f "$destination" ] && cmp -s "$source" "$destination"; then
    return
  fi
  mkdir -p "$(dirname "$destination")"
  temporary="$(mktemp "$(dirname "$destination")/.$(basename "$destination").XXXXXX")"
  if cp -p "$source" "$temporary" && mv -f "$temporary" "$destination"; then
    return
  fi
  rm -f "$temporary"
  return 1
}

bmx_update_kernel_listing() {
  local machine_build_dir="$1"
  local elf="$machine_build_dir/$TARGET_BASENAME.elf"
  local listing="$machine_build_dir/$TARGET_BASENAME.lst"
  local temporary

  if [ "$BMX_GENERATE_LISTING" = 0 ]; then
    [ ! -s "$listing" ] || : >"$listing"
    return
  fi
  if [ -s "$listing" ] && [ "$listing" -nt "$elf" ]; then
    return
  fi

  temporary="$(mktemp "$machine_build_dir/.$TARGET_BASENAME.lst.XXXXXX")"
  "${TOOL_PREFIX}objdump" -d "$elf" | "${TOOL_PREFIX}c++filt" >"$temporary"
  mv "$temporary" "$listing"
}

build_vice310_kernel() {
  local machine="$1"
  local image_dir machine_build_dir source_image output_image
  vice310_machine_config "$machine"
  machine_build_dir="$BMX_MACHINE_ROOT/$VICE310_CLASS"
  source_image="$machine_build_dir/$TARGET_BASENAME.img"

  (
    cd "$SRC_DIR"
    make -f "$VICE310_MAKEFILE" -j"$BMX_BUILD_JOBS" \
      BOARD="$BMX_BUILD_BOARD" AARCH="$TARGET_AARCH" \
      SRC_DIR=src BUILD_ROOT="$BMC64_BUILD_ROOT" \
      BUILD_DIR="$machine_build_dir" BMX_COMMON_BUILD_DIR="$BMX_SHARED_BUILD_DIR" \
      BMX_CONFIG_STAMP="$BMX_CONFIG_STAMP" BMX_GENERATE_LISTING="$BMX_GENERATE_LISTING" \
      TARGET_BASENAME="$TARGET_BASENAME" TARGET="$machine_build_dir/$TARGET_BASENAME" \
      CIRCLE_STDLIB_HOME="$CIRCLE_STDLIB_HOME" \
      CIRCLEHOME="$CIRCLE_STDLIB_HOME/libs/circle" NEWLIBDIR="$NEWLIBDIR" \
      VICE="$VICE_BUILD_SRC" VICE_SOURCE="$VICE_SRC" \
      VICE_ARCH="$VICE_SRC/arch/raspi" VICE_SHARED="$VICE_SRC/arch/shared" \
      RESID_IMPL="$VICE_BUILD_SRC/resid/libresid.a" BMC64_COMMON_LIB="$COMMON_LIB" \
      SD_LAYOUT_TOML=sd-layout.toml \
      UPDATE_PATH_POLICY_GENERATOR=tools/generate_update_path_policy.py \
      UPDATE_PATH_POLICY_MODULE=tools/update_path_policy.py \
      UPDATE_PATH_POLICY_HEADER=src/update/generated/update_path_policy_v1.h \
      BMC64_BUILD_PROFILE="${BMC64_BUILD_PROFILE:-release}" \
      BMX_PI5_SID_WORKER="${BMX_PI5_SID_WORKER:-$([ "$BMX_BUILD_BOARD" = pi5 ] && echo 1 || echo 0)}" \
      BMX_PI5_SID_DIAGNOSTICS="${BMX_PI5_SID_DIAGNOSTICS:-0}"
  )
  bmx_update_kernel_listing "$machine_build_dir"

  image_dir="$BMX_VARIANT_ROOT/images"
  mkdir -p "$image_dir"
  output_image="$image_dir/$TARGET_BASENAME.img.$VICE310_IMAGE_SUFFIX"
  bmx_copy_if_changed "$source_image" "$output_image"
  if [ "$VICE310_COPY_DEFAULT" -eq 1 ]; then
    bmx_copy_if_changed "$source_image" "$image_dir/$TARGET_BASENAME.img"
  fi
}

publish_vice310_images() {
  local machine source_dir destination_dir publish_lock_fd
  source_dir="$BMX_VARIANT_ROOT/images"
  destination_dir="$(bmc64_vice310_image_dir "$BMX_BUILD_BOARD")"
  mkdir -p "$destination_dir"

  exec {publish_lock_fd}>"$BMC64_BUILD_ROOT/$BMX_BUILD_BOARD/.vice310-images.lock"
  flock "$publish_lock_fd"
  for machine in "$@"; do
    vice310_machine_config "$machine"
    bmx_copy_if_changed \
      "$source_dir/$TARGET_BASENAME.img.$VICE310_IMAGE_SUFFIX" \
      "$destination_dir/$TARGET_BASENAME.img.$VICE310_IMAGE_SUFFIX"
    if [ "$VICE310_COPY_DEFAULT" -eq 1 ]; then
      bmx_copy_if_changed "$source_dir/$TARGET_BASENAME.img" \
        "$destination_dir/$TARGET_BASENAME.img"
    fi
  done
  flock -u "$publish_lock_fd"
  exec {publish_lock_fd}>&-
}

bmx_select_toolchain_and_layout() {
  local toolchain_lock_fd status=0

  command -v flock >/dev/null 2>&1 || {
    echo "flock is required for isolated incremental builds" >&2
    return 1
  }
  mkdir -p "$BMC64_BUILD_ROOT/$BMX_BUILD_BOARD"
  exec {toolchain_lock_fd}>"$BMC64_BUILD_ROOT/$BMX_BUILD_BOARD/.toolchain.lock"
  flock "$toolchain_lock_fd"
  ensure_toolchain && bmx_initialize_build_layout || status=$?
  flock -u "$toolchain_lock_fd"
  exec {toolchain_lock_fd}>&-
  return "$status"
}

bmx_prepare_circle_cache() {
  local circle_lock_fd status=0

  mkdir -p "$BMC64_BUILD_ROOT/$BMX_BUILD_BOARD/circle-variants/.locks"
  exec {circle_lock_fd}>"$BMC64_BUILD_ROOT/$BMX_BUILD_BOARD/circle-variants/.locks/$BMX_CIRCLE_BUILD_VARIANT.lock"
  flock "$circle_lock_fd"
  ensure_circle_stdlib && build_circle_stdlib || status=$?
  flock -u "$circle_lock_fd"
  exec {circle_lock_fd}>&-
  return "$status"
}

build_vice310_machines() {
  local machine variant_lock_fd
  [ "$#" -gt 0 ] || {
    echo "build_vice310_machines requires at least one machine" >&2
    return 2
  }

  bmx_select_toolchain_and_layout

  mkdir -p "$BMC64_BUILD_ROOT/$BMX_BUILD_BOARD/variants/.locks"
  exec {variant_lock_fd}>"$BMC64_BUILD_ROOT/$BMX_BUILD_BOARD/variants/.locks/$BMX_BUILD_VARIANT.lock"
  flock "$variant_lock_fd"

  if [ "${BMX_BUILD_CLEAN:-0}" = 1 ]; then
    bmx_remove_variant_root
  fi
  bmx_write_variant_stamp
  bmx_prepare_circle_cache

  set_vice310_make_args
  prepare_vice_source_for_out_of_tree
  configure_vice310
  build_common

  for machine in "$@"; do
    build_vice310_archives "$machine"
  done
  build_vice310_link_archives "$@"
  for machine in "$@"; do
    build_vice310_kernel "$machine"
  done
  publish_vice310_images "$@"

  flock -u "$variant_lock_fd"
  exec {variant_lock_fd}>&-
}
