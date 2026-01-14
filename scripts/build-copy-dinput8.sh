#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT_DIR/clients/dinput8_proxy/dinput8_proxy.cpp"
OUT="$ROOT_DIR/clients/dinput8_proxy/dinput8.dll"

if ! command -v x86_64-w64-mingw32-g++ >/dev/null 2>&1; then
  echo "Missing x86_64-w64-mingw32-g++ (mingw-w64)." >&2
  exit 1
fi

TARGET_DIR="${1:-${AC_DIR:-}}"
if [[ -z "$TARGET_DIR" ]]; then
  echo "Usage: $0 <path-to-acs-folder>" >&2
  echo "Example: $0 \"/Volumes/hd_externo/crossover/Assetto Corsa/drive_c/Program Files (x86)/Steam/steamapps/common/assettocorsa\"" >&2
  exit 1
fi

if [[ ! -f "$SRC" ]]; then
  echo "Source not found: $SRC" >&2
  exit 1
fi

if [[ ! -d "$TARGET_DIR" ]]; then
  echo "Target dir not found: $TARGET_DIR" >&2
  exit 1
fi

x86_64-w64-mingw32-g++ -shared -O2 -static -static-libstdc++ -static-libgcc \
  -o "$OUT" "$SRC" -lws2_32

cp -f "$OUT" "$TARGET_DIR/dinput8.dll"

echo "Built: $OUT"
echo "Copied to: $TARGET_DIR/dinput8.dll"
