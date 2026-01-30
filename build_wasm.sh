#!/bin/bash
set -e

# Build explorer for WASM using zig build
# Run from root: ./build_wasm.sh

echo "Building explorer.wasm..."
zig build -Dtarget=wasm32-wasi --release=fast

# Ensure web directory exists
mkdir -p web

# Copy the generated binary to the web folder
# Note: zig build might name it 'explorer' or 'explorer.wasm'
if [ -f "zig-out/bin/explorer.wasm" ]; then
    cp zig-out/bin/explorer.wasm web/explorer.wasm
else
    echo "Error: Could not find build output in zig-out/bin/"
    exit 1
fi

echo "Success: explorer.wasm generated in web/."
ls -lh web/explorer.wasm
