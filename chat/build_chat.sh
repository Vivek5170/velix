#!/usr/bin/env bash
set -euo pipefail

# Find the project root
ROOT=$(cd "$(dirname "$0")" && pwd)
BUILD_DIR="$ROOT/../build"

echo "[*] Building Velix Gateway Handler (C++)..."

mkdir -p "$BUILD_DIR"

# -----------------------------------------------------------------------------
# build handler
# -----------------------------------------------------------------------------
# Requires the Velix SDK, the framing layer, and the core communication utils.
if g++ -std=c++17 -pthread -O2 \
    -I"$ROOT/.." \
    -I"$ROOT/../vendor" \
    "$ROOT/handler.cpp" \
        "$ROOT/../runtime/sdk/cpp/velix_process.cpp" \
        "$ROOT/../communication/send.cpp" \
        "$ROOT/../communication/recv.cpp" \
        -o "$BUILD_DIR/chat_handler"; then
    chmod +x "$BUILD_DIR/chat_handler"
    echo "[OK] chat_handler built: build/chat_handler"
    echo "[INFO] Running it will listen on port 6060 for Gateway connections."
else
    echo "[FAIL] chat_handler build failed!"
    exit 1
fi

echo "[INFO] Python gateways (terminal.py) do not require compilation."
echo "[DONE]"
