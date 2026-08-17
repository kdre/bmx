#!/bin/bash

# Configuration-isolated Circle build support for the shared Pi target helper.
# This file is content-hashed into the Circle cache key, so VICE-only build
# helper changes do not invalidate the expensive Circle/Newlib tree.

bmx_circle_input_hash() {
  local name circle_archive_hash mbedtls_archive_hash
  circle_archive_hash="$(awk -v archive_name="$(basename "$CIRCLE_STDLIB_SOURCE_ARCHIVE")" \
    '$2 ~ archive_name "$" { print $1; exit }' "$CIRCLE_STDLIB_SOURCE_SHA256")"
  mbedtls_archive_hash="$(awk -v archive="$BMX_MBEDTLS_ARCHIVE_REL" \
    '$2 == archive { print $1; exit }' "$SRC_DIR/$BMX_MBEDTLS_CHECKSUM_REL")"
  [ -n "$circle_archive_hash" ] || {
    echo "missing Circle source archive checksum" >&2
    return 1
  }
  [ -n "$mbedtls_archive_hash" ] || {
    echo "missing Mbed TLS source archive checksum" >&2
    return 1
  }

  {
    printf 'format=1\nboard=%s\nprofile=%s\ntoolchain=%s\n' \
      "$BMX_BUILD_BOARD" "$BMC64_BUILD_PROFILE" "$BMX_TOOLCHAIN_FINGERPRINT"
    printf 'configure=%s\ncircle-archive=%s\nmbedtls-archive=%s\npatchset=%s\n' \
      "${CIRCLE_CONFIG_ARGS[*]}" "$circle_archive_hash" "$mbedtls_archive_hash" \
      "$(circle_patchset_hash "$CIRCLE_STDLIB_PATCH_DIR")"
    for name in BMC64_BUILD_PROFILE BMC64_TCP_LOG_LEVEL BMC64_WLAN_TRACE \
      BMC64_WLAN_LOW_IMPACT_TRACE BMC64_WLAN_LOW_IMPACT_TRACE_LOG; do
      bmx_build_value "$name"
    done
    sha256sum \
      "$SRC_DIR/tools/lib/circle_patches.sh" \
      "$SRC_DIR/tools/lib/circle_source_archive.sh" \
      "$SRC_DIR/tools/lib/mbedtls_source_archive.sh" \
      "${BASH_SOURCE[0]}"
  } | bmx_hash_stream
}

ensure_circle_stdlib() {
  local patchset_hash

  patchset_hash="$(circle_patchset_hash "$CIRCLE_STDLIB_PATCH_DIR")"
  if ! circle_stdlib_patchset_matches "$CIRCLE_STDLIB_HOME" "$patchset_hash"; then
    echo "Circle patch set changed; refreshing $CIRCLE_STDLIB_HOME"
    rm -rf -- "$CIRCLE_STDLIB_HOME"
  fi

  circle_stdlib_extract_from_archive \
    "$CIRCLE_STDLIB_SOURCE_ARCHIVE" \
    "$CIRCLE_STDLIB_SOURCE_SHA256" \
    "$CIRCLE_STDLIB_HOME"
  apply_circle_stdlib_patches "$CIRCLE_STDLIB_HOME" "$CIRCLE_STDLIB_PATCH_DIR"
  circle_stdlib_install_pinned_mbedtls "$SRC_DIR" "$CIRCLE_STDLIB_HOME"
  record_circle_stdlib_patchset "$CIRCLE_STDLIB_HOME" "$patchset_hash"
}

configure_circle_profile_flags() {
  local config2="$CIRCLE_STDLIB_HOME/libs/circle/Config2.mk"
  local temporary
  temporary="$(mktemp "$CIRCLE_STDLIB_HOME/libs/circle/.Config2.mk.XXXXXX")"

  if [ "$BMC64_BUILD_PROFILE" = debug ]; then
    printf 'DEFINE += -DBMC64_DEBUG_PROFILE\n' >>"$temporary"
    if [ -n "${BMC64_TCP_LOG_LEVEL:-}" ]; then
      printf 'DEFINE += -DBMC64_TCP_LOG_LEVEL=%s\n' "$BMC64_TCP_LOG_LEVEL" >>"$temporary"
    fi
  fi
  [ -z "${BMC64_WLAN_TRACE:-}" ] || printf 'DEFINE += -DBMC64_WLAN_TRACE\n' >>"$temporary"
  [ -z "${BMC64_WLAN_LOW_IMPACT_TRACE:-}" ] || \
    printf 'DEFINE += -DBMC64_WLAN_LOW_IMPACT_TRACE\n' >>"$temporary"
  [ -z "${BMC64_WLAN_LOW_IMPACT_TRACE_LOG:-}" ] || \
    printf 'DEFINE += -DBMC64_WLAN_LOW_IMPACT_TRACE_LOG\n' >>"$temporary"

  if [ ! -s "$temporary" ]; then
    rm -f "$temporary" "$config2"
  elif [ -f "$config2" ] && cmp -s "$temporary" "$config2"; then
    rm -f "$temporary"
  else
    mv "$temporary" "$config2"
  fi
}

circle_config_hash() {
  {
    printf 'format=3\ncircle-input=%s\n' "$BMX_CIRCLE_INPUT_HASH"
  } | bmx_hash_stream
}

rename_wpa_supplicant_sha1_symbols() {
  local lib="$CIRCLE_STDLIB_HOME/libs/circle/addon/wlan/hostap/wpa_supplicant/libwpa_supplicant.a"
  if "${TOOL_PREFIX}nm" --defined-only "$lib" 2>/dev/null | \
     grep -E '[[:space:]]SHA1(Transform|Init|Update|Final)$' >/dev/null; then
    "${TOOL_PREFIX}objcopy" \
      --redefine-sym SHA1Transform=wpa_SHA1Transform \
      --redefine-sym SHA1Init=wpa_SHA1Init \
      --redefine-sym SHA1Update=wpa_SHA1Update \
      --redefine-sym SHA1Final=wpa_SHA1Final \
      "$lib"
  fi
}

remove_newlib_conflicting_members() {
  local lib="$NEWLIBDIR/lib/libcirclenewlib.a"
  local members=()
  local member
  for member in io.o errno.o getpid.o; do
    if "${TOOL_PREFIX}ar" t "$lib" | grep -Fx "$member" >/dev/null; then
      members+=("$member")
    fi
  done
  [ "${#members[@]}" -eq 0 ] || "${TOOL_PREFIX}ar" d "$lib" "${members[@]}"
}

# Keep the historical build/<board>/circle-stdlib path useful for patch work
# and auxiliary developer tools without making target builds depend on a
# mutable "active" tree.  A pre-existing real directory is deliberately left
# untouched: it may contain an older developer checkout.  Fresh build roots
# receive an atomically updated convenience symlink to the last completed
# configuration-isolated Circle tree.
bmx_update_circle_active_alias() {
  local board_root alias target temporary_dir
  board_root="$BMC64_BUILD_ROOT/$BMX_BUILD_BOARD"
  alias="$board_root/circle-stdlib"
  target="circle-variants/$BMX_CIRCLE_BUILD_VARIANT/circle-stdlib"

  if { [ -e "$alias" ] || [ -L "$alias" ]; } && [ ! -L "$alias" ]; then
    return
  fi

  temporary_dir="$(mktemp -d "$board_root/.circle-alias.XXXXXX")"
  ln -s "$target" "$temporary_dir/circle-stdlib"
  mv -Tf "$temporary_dir/circle-stdlib" "$alias"
  rmdir "$temporary_dir"
}

circle_required_artifacts() {
  local circle="$CIRCLE_STDLIB_HOME/libs/circle"
  printf '%s\n' \
    "$CIRCLE_STDLIB_HOME/Config.mk" \
    "$NEWLIBDIR/lib/libm.a" \
    "$NEWLIBDIR/lib/libc.a" \
    "$NEWLIBDIR/lib/libcirclenewlib.a" \
    "$NEWLIBDIR/lib/libnosys.a" \
    "$circle/addon/SDCard/libsdcard.a" \
    "$circle/addon/fatfs/libfatfs.a" \
    "$circle/addon/linux/liblinuxemu.a" \
    "$circle/addon/qemu/libqemusupport.a" \
    "$circle/addon/wlan/libwlan.a" \
    "$circle/addon/wlan/hostap/wpa_supplicant/libwpa_supplicant.a" \
    "$circle/lib/usb/libusb.a" \
    "$circle/lib/input/libinput.a" \
    "$circle/lib/fs/libfs.a" \
    "$circle/lib/net/libnet.a" \
    "$circle/lib/sched/libsched.a" \
    "$circle/lib/sound/libsound.a" \
    "$circle/lib/libcircle.a" \
    "$CIRCLE_STDLIB_HOME/src/circle-mbedtls/libcircle-mbedtls.a" \
    "$CIRCLE_STDLIB_HOME/libs/mbedtls/library/libmbedtls.a" \
    "$CIRCLE_STDLIB_HOME/libs/mbedtls/library/libmbedx509.a" \
    "$CIRCLE_STDLIB_HOME/libs/mbedtls/library/libmbedcrypto.a"

  if [ "${BMX_PI4_LEGACY_DISPLAY:-0}" = 1 ]; then
    printf '%s\n' \
      "$circle/addon/vc4/vchiq/libvchiq.a" \
      "$circle/addon/vc4/interface/bcm_host/libbcm_host.a" \
      "$circle/addon/vc4/interface/khronos/libkhrn_client.a" \
      "$circle/addon/vc4/interface/vcos/libvcos.a" \
      "$circle/addon/vc4/interface/vmcs_host/libvmcs_host.a"
  fi
  if [ "$BMX_BUILD_BOARD" = pi4 ] && [ "${BMX_PI4_AARCH64:-0}" = 1 ]; then
    printf '%s\n' "$circle/boot/armstub8-rpi4.bin"
  fi
}

circle_source_changed_since() {
  local marker="$1"
  local newer
  newer="$(find "$CIRCLE_STDLIB_HOME" \
    \( -path "$CIRCLE_STDLIB_HOME/build" -o \
       -path "$CIRCLE_STDLIB_HOME/install" -o \
       -path "$CIRCLE_STDLIB_HOME/.bmc64-circle-patches" \) -prune -o \
    -type f \
    \( -name '*.c' -o -name '*.cc' -o -name '*.cpp' -o \
       -name '*.h' -o -name '*.hpp' -o -name '*.S' -o -name '*.s' -o \
       -name '*.inc' -o -name '*.ld' -o -name '*.mk' -o \
       -name 'Makefile' -o -name 'Makefile.*' -o -name 'configure' -o \
       -name '*.sh' \) \
    -newer "$marker" -print -quit)"
  [ -n "$newer" ]
}

circle_build_is_complete() {
  local marker="$CIRCLE_STDLIB_HOME/.bmx-build-complete.sha256"
  local wanted="$1"
  local artifact

  [ -f "$marker" ] && [ "$(cat "$marker")" = "$wanted" ] || return 1
  while IFS= read -r artifact; do
    [ -s "$artifact" ] || return 1
  done < <(circle_required_artifacts)
  ! circle_source_changed_since "$marker"
}

circle_downstream_dependency_files() {
  local artifact

  if [ -d "$CIRCLE_STDLIB_HOME/install" ]; then
    find "$CIRCLE_STDLIB_HOME/install" -type f -print0
  fi
  find "$CIRCLE_STDLIB_HOME" \
    \( -path "$CIRCLE_STDLIB_HOME/build" -o \
       -path "$CIRCLE_STDLIB_HOME/install" -o \
       -path "$CIRCLE_STDLIB_HOME/.bmc64-circle-patches" \) -prune -o \
    -type f \
    \( -name '*.a' -o -name '*.h' -o -name '*.hpp' -o \
       -name '*.inc' -o -name 'Config.mk' -o -name 'Config2.mk' \) \
    -print0
  while IFS= read -r artifact; do
    [ ! -f "$artifact" ] || printf '%s\0' "$artifact"
  done < <(circle_required_artifacts)
}

circle_snapshot_downstream_metadata() {
  local snapshot_dir="$1"
  local path reference index=0

  BMX_CIRCLE_SNAPSHOT_PATHS=()
  BMX_CIRCLE_SNAPSHOT_HASHES=()
  BMX_CIRCLE_SNAPSHOT_REFERENCES=()
  while IFS= read -r -d '' path; do
    reference="$snapshot_dir/$index"
    : >"$reference"
    touch -r "$path" "$reference"
    BMX_CIRCLE_SNAPSHOT_PATHS+=("$path")
    BMX_CIRCLE_SNAPSHOT_HASHES+=("$(sha256sum "$path" | awk '{print $1}')")
    BMX_CIRCLE_SNAPSHOT_REFERENCES+=("$reference")
    index=$((index + 1))
  done < <(circle_downstream_dependency_files | sort -zu)
}

circle_restore_identical_downstream_metadata() {
  local index path

  for index in "${!BMX_CIRCLE_SNAPSHOT_PATHS[@]}"; do
    path="${BMX_CIRCLE_SNAPSHOT_PATHS[$index]}"
    if [ -f "$path" ] && \
       [ "$(sha256sum "$path" | awk '{print $1}')" = \
         "${BMX_CIRCLE_SNAPSHOT_HASHES[$index]}" ]; then
      touch -r "${BMX_CIRCLE_SNAPSHOT_REFERENCES[$index]}" "$path"
    fi
  done
}

build_circle_stdlib_artifacts() {
  local -a deterministic_archive_args=(
    "AR=${TOOL_PREFIX}ar -D"
    "RANLIB=${TOOL_PREFIX}ranlib -D"
    "AR_FOR_TARGET=${TOOL_PREFIX}ar -D"
    "RANLIB_FOR_TARGET=${TOOL_PREFIX}ranlib -D"
  )

  make -C "$CIRCLE_STDLIB_HOME" -j"$BMX_BUILD_JOBS" \
    "${deterministic_archive_args[@]}" || return
  make -C "$CIRCLE_STDLIB_HOME/libs/circle/addon/fatfs" \
    -j"$BMX_BUILD_JOBS" "${deterministic_archive_args[@]}" || return
  make -C "$CIRCLE_STDLIB_HOME/libs/circle/addon/linux" \
    -j"$BMX_BUILD_JOBS" "${deterministic_archive_args[@]}" || return
  make -C "$CIRCLE_STDLIB_HOME/libs/circle/addon/wlan" \
    -j"$BMX_BUILD_JOBS" "${deterministic_archive_args[@]}" || return
  make -C "$CIRCLE_STDLIB_HOME/libs/circle/addon/wlan/hostap/wpa_supplicant" \
    -f Makefile.circle -j"$BMX_BUILD_JOBS" \
    "${deterministic_archive_args[@]}" libwpa_supplicant.a || return
  rename_wpa_supplicant_sha1_symbols || return

  if [ "${BMX_PI4_LEGACY_DISPLAY:-0}" = 1 ]; then
    make -C "$CIRCLE_STDLIB_HOME/libs/circle/addon/vc4/vchiq" \
      -j"$BMX_BUILD_JOBS" "${deterministic_archive_args[@]}" || return
    make -C "$CIRCLE_STDLIB_HOME/libs/circle/addon/vc4/interface/bcm_host" \
      -j"$BMX_BUILD_JOBS" "${deterministic_archive_args[@]}" || return
    make -C "$CIRCLE_STDLIB_HOME/libs/circle/addon/vc4/interface/khronos" \
      -j"$BMX_BUILD_JOBS" "${deterministic_archive_args[@]}" || return
    make -C "$CIRCLE_STDLIB_HOME/libs/circle/addon/vc4/interface/vcos" \
      -j"$BMX_BUILD_JOBS" "${deterministic_archive_args[@]}" || return
    make -C "$CIRCLE_STDLIB_HOME/libs/circle/addon/vc4/interface/vmcs_host" \
      -j"$BMX_BUILD_JOBS" "${deterministic_archive_args[@]}" || return
  else
    make -C "$CIRCLE_STDLIB_HOME/libs/circle/lib/sound" \
      -j"$BMX_BUILD_JOBS" "${deterministic_archive_args[@]}" all || return
  fi
  if [ "$BMX_BUILD_BOARD" = pi4 ] && [ "${BMX_PI4_AARCH64:-0}" = 1 ]; then
    make -C "$CIRCLE_STDLIB_HOME/libs/circle/boot" armstub64 || return
  fi
  remove_newlib_conflicting_members || return
}

build_circle_stdlib() {
  local marker="$CIRCLE_STDLIB_HOME/.bmx-build-config.sha256"
  local complete_marker="$CIRCLE_STDLIB_HOME/.bmx-build-complete.sha256"
  local complete_temporary
  local snapshot_dir
  local wanted
  local profile_options=()

  wanted="$(circle_config_hash)"
  if [ ! -f "$CIRCLE_STDLIB_HOME/Config.mk" ] || \
     [ ! -f "$marker" ] || [ "$(cat "$marker")" != "$wanted" ]; then
    echo "Configuring Circle for $BMX_BUILD_BOARD/$BMC64_BUILD_PROFILE"
    (
      cd "$CIRCLE_STDLIB_HOME"
      make mrproper >/dev/null 2>&1 || true
      if [ "$BMC64_BUILD_PROFILE" = release ]; then
        profile_options+=(--option NDEBUG)
      fi
      ./configure "${CIRCLE_CONFIG_ARGS[@]}" "${profile_options[@]}"
    )
    configure_circle_profile_flags
    printf '%s\n' "$wanted" >"$marker"
  else
    configure_circle_profile_flags
  fi

  if circle_build_is_complete "$wanted"; then
    bmx_update_circle_active_alias
    return
  fi
  # A failed rebuild must never leave a success marker which can hide partial
  # archives or an interrupted Newlib install.
  rm -f "$complete_marker"
  snapshot_dir="$(mktemp -d /tmp/bmx-circle-metadata.XXXXXX)"
  circle_snapshot_downstream_metadata "$snapshot_dir"
  if ! build_circle_stdlib_artifacts; then
    rm -rf -- "$snapshot_dir"
    return 1
  fi
  circle_restore_identical_downstream_metadata
  rm -rf -- "$snapshot_dir"
  complete_temporary="$(mktemp "$CIRCLE_STDLIB_HOME/.bmx-build-complete.XXXXXX")"
  printf '%s\n' "$wanted" >"$complete_temporary"
  mv "$complete_temporary" "$complete_marker"
  bmx_update_circle_active_alias
}
