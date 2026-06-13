#!/usr/bin/env bash
set -euo pipefail

VARIANT="${1:-release}"
case "$VARIANT" in
  release|normal) VARIANT="release" ;;
  debug) VARIANT="debug" ;;
  *) echo "usage: $0 [release|debug]" >&2; exit 2 ;;
esac

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="$ROOT_DIR/out"
ZIP_NAME="Yukari.zip"
STAGE_NAME="Yukari"
BUILD_VARIANT="Release"
STRIP_TASK="stripReleaseDebugSymbols"

if [ "$VARIANT" = "debug" ]; then
  ZIP_NAME="Yukari-debug.zip"
  STAGE_NAME="Yukari-debug"
  BUILD_VARIANT="Debug"
  STRIP_TASK="stripDebugDebugSymbols"
fi

ZIP_PATH="$OUT_DIR/$ZIP_NAME"
STAGE="$OUT_DIR/$STAGE_NAME"

find_native_library() {
  local variant_lower="$1"
  local variant_cmake="$2"
  local strip_task="$3"
  local candidates=(
    "$ROOT_DIR/module/build/intermediates/stripped_native_libs/$variant_lower/$strip_task/out/lib/arm64-v8a/libyukari.so"
    "$ROOT_DIR/module/build/intermediates/stripped_native_libs/$variant_lower/$strip_task/out/lib/arm64-v8a/yukari.so"
    "$ROOT_DIR/module/build/intermediates/merged_native_libs/$variant_lower/merge${variant_cmake}NativeLibs/out/lib/arm64-v8a/libyukari.so"
    "$ROOT_DIR/module/build/intermediates/cxx/$variant_cmake"/*/obj/arm64-v8a/libyukari.so
    "$ROOT_DIR/module/build/intermediates/cxx/$variant_cmake"/*/obj/arm64-v8a/yukari.so
  )

  local candidate
  for candidate in "${candidates[@]}"; do
    if [ -f "$candidate" ]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done
  return 1
}

rm -rf "$STAGE" "$ZIP_PATH"
mkdir -p "$STAGE/zygisk" "$OUT_DIR"
cp "$ROOT_DIR/module/module.prop" "$STAGE/module.prop"
cp "$ROOT_DIR/module/config.json" "$STAGE/config.json"
cp "$ROOT_DIR/module/post-fs-data.sh" "$STAGE/post-fs-data.sh"
cp "$ROOT_DIR/module/service.sh" "$STAGE/service.sh"
chmod 0755 "$STAGE/post-fs-data.sh" "$STAGE/service.sh"

if [ "$VARIANT" = "debug" ]; then
  sed -i 's/^version=.*/version=0.1.0-debug/' "$STAGE/module.prop"
  sed -i 's/^description=.*/description=Debug build of Yukari with module-directory runtime logging enabled./' "$STAGE/module.prop"
fi

NATIVE_LIBRARY="$(find_native_library "$VARIANT" "$BUILD_VARIANT" "$STRIP_TASK" || true)"
if [ -z "$NATIVE_LIBRARY" ]; then
  echo "error: arm64-v8a libyukari.so was not produced for $VARIANT" >&2
  find "$ROOT_DIR/module/build" -type f \( -name 'libyukari.so' -o -name 'yukari.so' \) -print >&2 || true
  exit 1
fi

cp "$NATIVE_LIBRARY" "$STAGE/zygisk/arm64-v8a.so"

(
  cd "$STAGE"
  zip -9 -r "$ZIP_PATH" .
)

test -s "$ZIP_PATH"
unzip -l "$ZIP_PATH" | grep -q 'module.prop'
unzip -l "$ZIP_PATH" | grep -q 'zygisk/arm64-v8a.so'
echo "Packaged $ZIP_PATH from $NATIVE_LIBRARY"
