#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")" && pwd)
BUILD_DIR="$ROOT/../build"

echo "[*] Building chat handler and terminal clients..."

mkdir -p "$BUILD_DIR"

# build terminal client (requires communication helpers)
g++ -std=c++17 -pthread -O2 \
    -I"$ROOT/.." \
    -I"$ROOT/../vendor" \
    "$ROOT/terminal.cpp" \
        "$ROOT/../communication/send.cpp" \
        "$ROOT/../communication/recv.cpp" \
        -o "$BUILD_DIR/chat_terminal"

# build handler (SDK + communication layer)
if g++ -std=c++17 -pthread -O2 \
    -I"$ROOT/.." \
    -I"$ROOT/../vendor" \
    "$ROOT/handler.cpp" \
        "$ROOT/../runtime/sdk/cpp/velix_process.cpp" \
        "$ROOT/../communication/send.cpp" \
        "$ROOT/../communication/recv.cpp" \
        -o "$BUILD_DIR/chat_handler"; then
    chmod +x "$BUILD_DIR/chat_handler"
    echo "[OK] chat handler built: build/chat_handler"
else
    echo "[WARN] chat handler build failed; terminal client is still available."
fi

chmod +x "$BUILD_DIR/chat_terminal"

echo "[DONE] chat terminal built: build/chat_terminal"
