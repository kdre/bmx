#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
. "$SRC_DIR/tools/lib/build_paths.sh"
. "$SRC_DIR/tools/lib/install_boot_partition.sh"

usage() {
  cat <<EOF
Usage: $(basename "$0") BOARD MOUNTPOINT [--stage-dir DIR]

BOARD must be one of: pi4, pi5.

Copies the prepared BMX boot partition contents for BOARD into an already
mounted FAT boot partition.

The target must be an exact FAT/vfat mountpoint and must not be on the current
system disk.

Examples:
  $(basename "$0") pi4 /path/to/mounted/boot
  $(basename "$0") pi5 /path/to/mounted/boot --stage-dir /tmp/bmx-boot
EOF
}

if (($# == 0)); then
  usage
  exit 1
fi

BOARD=
MOUNTPOINT=
STAGE_DIR=
while (($# > 0)); do
  case "$1" in
    --stage-dir)
      if (($# < 2)); then
        echo "--stage-dir requires a directory" >&2
        exit 1
      fi
      STAGE_DIR="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      if [ -z "$BOARD" ]; then
        BOARD="$1"
      elif [ -z "${MOUNTPOINT:-}" ]; then
        MOUNTPOINT="$1"
      else
        echo "unexpected argument: $1" >&2
        exit 1
      fi
      shift
      ;;
  esac
done

case "$BOARD" in
  pi4)
    BOARD_NAME="Pi 4 / Pi 400"
    KERNEL_NAME=""
    ;;
  pi5)
    BOARD_NAME="Pi 5 / Pi 500"
    KERNEL_NAME="kernel_2712.img.c64"
    ;;
  "")
    echo "missing board" >&2
    usage
    exit 1
    ;;
  *)
    echo "unsupported board: $BOARD" >&2
    usage
    exit 1
    ;;
esac

if [ -z "${MOUNTPOINT:-}" ]; then
  echo "missing mountpoint" >&2
  usage
  exit 1
fi

if [ -z "$STAGE_DIR" ]; then
  STAGE_DIR="$(bmc64_stage_dir "$BOARD")"
fi

if [ "$BOARD" = pi4 ]; then
  SELECTOR="$STAGE_DIR/bmx-active-kernel.txt"
  [ -f "$SELECTOR" ] || {
    echo "Pi4 stage is missing bmx-active-kernel.txt" >&2
    exit 1
  }
  KERNEL_NAME="$(sed -n 's/^kernel=\(kernel[^[:space:]]*\.img\.c64\)$/\1/p' \
    "$SELECTOR")"
  case "$KERNEL_NAME" in
    kernel8.img.c64|kernel7l.img.c64)
      ;;
    *)
      echo "Pi4 stage selects an unsupported or ambiguous C64 kernel" >&2
      exit 1
      ;;
  esac
fi

bmc64_install_boot_partition "$BOARD_NAME" "$STAGE_DIR" "$MOUNTPOINT" \
  "$KERNEL_NAME"
