#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
STAGE_ARGS=()
BUILD_PROFILE="${BMC64_BUILD_PROFILE:-release}"
BUILD_ONLY=0
STAGE_DIR_SET=0
BUILD_MACHINES=()
BUILD_JOBS="${BMX_BUILD_JOBS:-$(nproc)}"
GENERATE_LISTING="${BMX_GENERATE_LISTING:-0}"
CLEAN_BUILD=0

usage() {
  cat <<EOF
Usage: $(basename "$0") [OPTIONS]

Builds Pi5/Pi500 VICE 3.10 kernels and stages a boot tree.

Options:
  --profile      staging boot config profile (default: release)
  --debug-uart   alias for --profile debug
  --build-only   build kernels without creating an SD-card stage
  --stage-dir    override the output staging directory
  --machine      build one machine (repeatable; requires --build-only)
  --jobs         maximum parallel compiler jobs (default: host CPU count)
  --listing      generate full kernel disassembly listings
  --no-listing   skip disassembly listings (default)
  --clean        discard the selected configuration cache before building
  --cmdline-option
                 set or override one staged cmdline.txt option; may be passed multiple times
EOF
}

while (($# > 0)); do
  case "$1" in
    --profile)
      if [ -z "${2:-}" ]; then
        echo "--profile requires release or debug" >&2
        exit 1
      fi
      case "$2" in
        release|debug) ;;
        *)
          echo "--profile requires release or debug" >&2
          exit 1
          ;;
      esac
      BUILD_PROFILE="$2"
      STAGE_ARGS+=("--profile" "$2")
      shift 2
      ;;
    --debug-uart)
      BUILD_PROFILE=debug
      STAGE_ARGS+=("--profile" "debug")
      shift
      ;;
    --build-only)
      BUILD_ONLY=1
      shift
      ;;
    --machine)
      if [ -z "${2:-}" ]; then
        echo "--machine requires a machine name" >&2
        exit 1
      fi
      BUILD_MACHINES+=("$2")
      shift 2
      ;;
    --jobs)
      if [ -z "${2:-}" ]; then
        echo "--jobs requires a positive integer" >&2
        exit 1
      fi
      BUILD_JOBS="$2"
      shift 2
      ;;
    --listing)
      GENERATE_LISTING=1
      shift
      ;;
    --no-listing)
      GENERATE_LISTING=0
      shift
      ;;
    --clean)
      CLEAN_BUILD=1
      shift
      ;;
    --stage-dir)
      if [ -z "${2:-}" ]; then
        echo "--stage-dir requires a directory" >&2
        exit 1
      fi
      STAGE_ARGS+=("--stage-dir" "$2")
      STAGE_DIR_SET=1
      shift 2
      ;;
    --cmdline-option)
      if [ -z "${2:-}" ] || [[ "$2" != *=* ]] || [[ "$2" == =* ]]; then
        echo "--cmdline-option requires KEY=VALUE" >&2
        exit 1
      fi
      STAGE_ARGS+=("--cmdline-option" "$2")
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "unexpected argument: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

if [ "$BUILD_ONLY" -eq 1 ] && [ "$STAGE_DIR_SET" -eq 1 ]; then
  echo "--build-only cannot be combined with --stage-dir" >&2
  exit 1
fi
if [ "${#BUILD_MACHINES[@]}" -gt 0 ] && [ "$BUILD_ONLY" -ne 1 ]; then
  echo "--machine requires --build-only so staging cannot mix partial kernel sets" >&2
  exit 1
fi
case "$BUILD_JOBS" in
  ''|*[!0-9]*|0) echo "--jobs requires a positive integer" >&2; exit 1 ;;
esac

export BMC64_BUILD_PROFILE="$BUILD_PROFILE"
export BMX_BUILD_JOBS="$BUILD_JOBS"
export BMX_GENERATE_LISTING="$GENERATE_LISTING"
export BMX_BUILD_CLEAN="$CLEAN_BUILD"

cat <<'EOF'
Building Pi5/Pi500 VICE 3.10 kernels with pinned Mbed TLS support.

Currently wired VICE 3.10 machines: C64 (x64/x64sc), SCPU64, C128, VIC20, Plus/4, PET.
EOF

. "$SRC_DIR/tools/pi5/vice310_build_common.sh"

BMX_PI5_MACHINE_LIST="$(
  python3 "$SRC_DIR/tools/sd_layout.py" kernel-machines --board pi5
)"
[[ -n "$BMX_PI5_MACHINE_LIST" ]] || {
  echo "sd-layout.toml defines no required Pi 5 machine kernels" >&2
  exit 1
}
mapfile -t BMX_PI5_MACHINES <<<"$BMX_PI5_MACHINE_LIST"
if [ "${#BUILD_MACHINES[@]}" -gt 0 ]; then
  for machine in "${BUILD_MACHINES[@]}"; do
    grep -Fx "$machine" <<<"$BMX_PI5_MACHINE_LIST" >/dev/null || {
      echo "unsupported Pi 5 machine: $machine" >&2
      exit 1
    }
  done
  BMX_PI5_MACHINES=("${BUILD_MACHINES[@]}")
fi
build_vice310_machines "${BMX_PI5_MACHINES[@]}"
if [ "$BUILD_ONLY" -eq 0 ]; then
  "$SRC_DIR/tools/pi5/stage_pi5_sd.sh" \
    --kernel-dir "$BMX_VARIANT_ROOT/images" "${STAGE_ARGS[@]}"
fi
