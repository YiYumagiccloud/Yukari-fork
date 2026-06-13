#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="$ROOT_DIR/out"
ZIP_PATH="$OUT_DIR/Yukari.zip"
STAGE="$OUT_DIR/Yukari"

rm -rf "$STAGE"
mkdir -p "$STAGE/zygisk" "$OUT_DIR"
cp "$ROOT_DIR/module/module.prop" "$STAGE/module.prop"
cp "$ROOT_DIR/module/config.json" "$STAGE/config.json"
cp "$ROOT_DIR/module/post-fs-data.sh" "$STAGE/post-fs-data.sh"
cp "$ROOT_DIR/module/service.sh" "$STAGE/service.sh"

if [ -f "$ROOT_DIR/module/build/intermediates/stripped_native_libs/release/stripReleaseDebugSymbols/out/lib/arm64-v8a/libyukari.so" ]; then
  cp "$ROOT_DIR/module/build/intermediates/stripped_native_libs/release/stripReleaseDebugSymbols/out/lib/arm64-v8a/libyukari.so" "$STAGE/zygisk/arm64-v8a.so"
else
  echo "warning: arm64-v8a libyukari.so not found; zip will not include native library" >&2
fi

(cd "$STAGE" && zip -r "$ZIP_PATH" .)
echo "$ZIP_PATH"
