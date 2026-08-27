#!/usr/bin/env bash
set -euo pipefail

usage() {
  echo "Usage: scripts/build-uboot-images.sh {flash|ram}" >&2
}

if [ "$#" -ne 1 ]; then
  usage
  exit 2
fi

MODE="$1"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SOURCE_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PACKER="$SOURCE_ROOT/tools/qualcomm-mbn/elftombn.py"
FLASH_VALIDATOR="$SOURCE_ROOT/scripts/verify-appsbl.py"
RAM_VALIDATOR="$SOURCE_ROOT/scripts/verify-ram-uboot.py"
RAM_ITS="$SOURCE_ROOT/scripts/wxr5950ax12-ram.its"
ARTIFACT_DIR="$SOURCE_ROOT/build-artifacts"

case "$MODE" in
  flash)
    DEFCONFIG="buffalo_wxr5950ax12_defconfig"
    FINAL_NAME="APPSBL"
    ARTIFACT_LABEL="Flash APPSBL"
    ;;
  ram)
    DEFCONFIG="buffalo_wxr5950ax12_ram_defconfig"
    FINAL_NAME="ram-uboot.itb"
    ARTIFACT_LABEL="RAM U-Boot FIT"
    ;;
  *)
    usage
    exit 2
    ;;
esac

FINAL_IMAGE="$ARTIFACT_DIR/$FINAL_NAME"

required_commands=(make-all python3 nproc rsync sha256sum stat install)
if [ "$MODE" = "flash" ]; then
  required_commands+=(arm-openwrt-linux-strip)
else
  required_commands+=(gcc gzip fdtget)
fi

for required_command in "${required_commands[@]}"; do
  if ! command -v "$required_command" >/dev/null 2>&1; then
    echo "Required container command not found: $required_command" >&2
    exit 2
  fi
done

if [ "$MODE" = "flash" ]; then
  if [ ! -f "$PACKER" ]; then
    echo "MBN packer not found: $PACKER" >&2
    exit 2
  fi
  if [ ! -f "$FLASH_VALIDATOR" ]; then
    echo "APPSBL validator not found: $FLASH_VALIDATOR" >&2
    exit 2
  fi
else
  if [ ! -f "$RAM_VALIDATOR" ]; then
    echo "RAM U-Boot validator not found: $RAM_VALIDATOR" >&2
    exit 2
  fi
  if [ ! -f "$RAM_ITS" ]; then
    echo "RAM U-Boot ITS not found: $RAM_ITS" >&2
    exit 2
  fi
fi
if [ ! -f "$SOURCE_ROOT/configs/${DEFCONFIG}" ]; then
  echo "Defconfig not found: $SOURCE_ROOT/configs/${DEFCONFIG}" >&2
  exit 2
fi

JOBS="$(nproc)"
case "$JOBS" in
  ""|0|*[!0-9]*)
    echo "nproc did not return a positive integer" >&2
    exit 2
    ;;
esac

if [ -z "${QSDK_TOOLCHAIN:-}" ] || [ ! -d "$QSDK_TOOLCHAIN" ]; then
  echo "QSDK_TOOLCHAIN does not name the container toolchain directory" >&2
  exit 2
fi

export ARCH=arm
export CROSS_COMPILE=arm-openwrt-linux-
export STAGING_DIR="$QSDK_TOOLCHAIN"

MAKE_MODE_ARGS=()
MAKE_BUILD_ARGS=()
if [ "$MODE" = "flash" ]; then
  export TARGETCC="${CROSS_COMPILE}gcc"
else
  unset TARGETCC
  unset HOSTCC
  MAKE_MODE_ARGS=(HOSTCC=gcc)
  MAKE_BUILD_ARGS=(--eval=".SECONDARY: arch/arm/dts/ipq807x-wxr5950ax12.dtb")
fi

TEMP_DIR="$(mktemp -d "/tmp/wxr5950ax12-${MODE}.XXXXXX")"
PUBLISH_TMP=""

cleanup() {
  if [ -n "$PUBLISH_TMP" ]; then
    rm -f -- "$PUBLISH_TMP"
  fi
  rm -rf -- "$TEMP_DIR"
}
trap cleanup EXIT

verify_artifact() {
  local artifact="$1"

  if [ "$MODE" = "flash" ]; then
    python3 -B "$FLASH_VALIDATOR" "$artifact" "$STRIPPED_ELF"
  else
    python3 -B "$RAM_VALIDATOR" "$artifact" "$BUILD_DIR"
  fi
}

TEMP_SOURCE="$TEMP_DIR/source"
BUILD_DIR="$TEMP_DIR/build"
STRIPPED_ELF=""
if [ "$MODE" = "flash" ]; then
  STRIPPED_ELF="$TEMP_DIR/u-boot.strip"
  PACKAGED_IMAGE="$TEMP_DIR/APPSBL"
else
  PACKAGED_IMAGE="$TEMP_DIR/ram-uboot.itb"
fi
mkdir -p "$TEMP_SOURCE" "$BUILD_DIR"

rsync -a \
  --exclude '.git' \
  --exclude '/build-*' \
  "$SOURCE_ROOT/" \
  "$TEMP_SOURCE/"

make-all -C "$TEMP_SOURCE" \
  mrproper

echo "Building $MODE U-Boot with $JOBS container-visible CPUs"
make-all -C "$TEMP_SOURCE" \
  O="$BUILD_DIR" \
  "${MAKE_MODE_ARGS[@]}" \
  "$DEFCONFIG"
make-all -C "$TEMP_SOURCE" \
  O="$BUILD_DIR" \
  "${MAKE_MODE_ARGS[@]}" \
  -j "$JOBS" \
  "${MAKE_BUILD_ARGS[@]}"

if [ ! -s "$BUILD_DIR/u-boot" ]; then
  echo "Build did not produce a non-empty u-boot ELF" >&2
  exit 1
fi

if [ "$MODE" = "flash" ]; then
  arm-openwrt-linux-strip "$BUILD_DIR/u-boot" -o "$STRIPPED_ELF"
  python3 -B "$PACKER" -f "$STRIPPED_ELF" -o "$PACKAGED_IMAGE" -v 3
else
  RAM_RAW="$BUILD_DIR/u-boot.bin"
  RAM_DTB="$BUILD_DIR/arch/arm/dts/ipq807x-wxr5950ax12.dtb"
  MKIMAGE="$BUILD_DIR/tools/mkimage"
  DUMPIMAGE="$BUILD_DIR/tools/dumpimage"
  FIT_INPUT="$TEMP_DIR/fit-input"

  for required_file in \
    "$BUILD_DIR/u-boot.map" \
    "$RAM_RAW" \
    "$RAM_DTB" \
    "$MKIMAGE" \
    "$DUMPIMAGE"; do
    if [ ! -s "$required_file" ]; then
      echo "RAM build output missing or empty: $required_file" >&2
      exit 1
    fi
  done

  "$MKIMAGE" -V
  "$DUMPIMAGE" -V

  mkdir -p "$FIT_INPUT"
  gzip -9 -n -c "$RAM_RAW" > "$FIT_INPUT/u-boot.bin.gz"
  install -m 0644 "$RAM_DTB" "$FIT_INPUT/ipq807x-wxr5950ax12.dtb"
  install -m 0644 "$RAM_ITS" "$FIT_INPUT/wxr5950ax12-ram.its"

  (
    cd "$FIT_INPUT"
    "$MKIMAGE" -f wxr5950ax12-ram.its "$PACKAGED_IMAGE"
  )
fi

chmod 0644 "$PACKAGED_IMAGE"
verify_artifact "$PACKAGED_IMAGE"

STAGED_SIZE="$(stat -c '%s' "$PACKAGED_IMAGE")"
STAGED_SHA256="$(sha256sum "$PACKAGED_IMAGE" | awk '{print $1}')"
STAGED_MODE="$(stat -c '%a' "$PACKAGED_IMAGE")"

mkdir -p "$ARTIFACT_DIR"
PUBLISH_TMP="$(mktemp "$ARTIFACT_DIR/.${FINAL_NAME}.XXXXXX")"
install -m 0644 "$PACKAGED_IMAGE" "$PUBLISH_TMP"

if [ "$(stat -c '%s' "$PUBLISH_TMP")" != "$STAGED_SIZE" ]; then
  echo "Published staging size differs from the verified image" >&2
  exit 1
fi
if [ "$(sha256sum "$PUBLISH_TMP" | awk '{print $1}')" != "$STAGED_SHA256" ]; then
  echo "Published staging hash differs from the verified image" >&2
  exit 1
fi
if [ "$(stat -c '%a' "$PUBLISH_TMP")" != "$STAGED_MODE" ]; then
  echo "Published staging permission differs from the verified image" >&2
  exit 1
fi

mv -f -- "$PUBLISH_TMP" "$FINAL_IMAGE"
PUBLISH_TMP=""

verify_artifact "$FINAL_IMAGE"

FINAL_SIZE="$(stat -c '%s' "$FINAL_IMAGE")"
FINAL_SHA256="$(sha256sum "$FINAL_IMAGE" | awk '{print $1}')"
FINAL_MODE="$(stat -c '%a' "$FINAL_IMAGE")"
if [ "$FINAL_SIZE" != "$STAGED_SIZE" ] || \
   [ "$FINAL_SHA256" != "$STAGED_SHA256" ] || \
   [ "$FINAL_MODE" != "$STAGED_MODE" ]; then
  echo "Final image changed during publication" >&2
  exit 1
fi

echo "$ARTIFACT_LABEL: $FINAL_IMAGE"
echo "size: $FINAL_SIZE bytes"
echo "sha256: $FINAL_SHA256"
echo "mode: $FINAL_MODE"
