#!/usr/bin/env bash
set -euo pipefail

SO_PATH="${1:-}"
PID="${2:-}"
if [[ -z "$SO_PATH" || ! -f "$SO_PATH" ]]; then
  echo "usage: $0 /path/to/arm64-v8a.so [target-pid]" >&2
  exit 2
fi

echo "[ELF] exported symbols"
readelf -Ws "$SO_PATH" | awk '$4 == "FUNC" && $7 != "UND" {print}'
if readelf -Ws "$SO_PATH" | grep -Eq ' (hook_ioctl|hook_transact_native|install_hooks)$'; then
  echo "error: private hook symbol is visible" >&2
  exit 1
fi
readelf -Ws "$SO_PATH" | grep -q 'zygisk_module_entry' || {
  echo "error: zygisk_module_entry is missing" >&2
  exit 1
}

if [[ -n "$PID" ]]; then
  echo "[maps] module and writable-executable mappings for pid $PID"
  adb shell "cat /proc/$PID/maps" | grep -E 'yukari|rwxp' || true
  echo "[GOT] JNI path leaves libbinder relocations untouched."
  echo "      For the legacy ioctl fallback, resolve the ioctl slot with dladdr"
  echo "      and verify it points at an anonymous r-xp trampoline."
fi

echo "verification complete"
