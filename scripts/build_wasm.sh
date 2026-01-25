#!/bin/bash
# Build parse_gds for WASM (Streaming mode)
# Run from root: ./scripts/build_wasm.sh

# Get the script's directory
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"
ROOT="$DIR/.."

echo "Building parse_gds.wasm..."
zig c++ -target wasm32-wasi -O3 -fno-exceptions -fno-rtti \
  -rdynamic -Wl,--no-entry -Wl,--strip-all \
  -lc -lc++ -DWASM "$ROOT/src/parse_gds.cpp" -o "$ROOT/web/parse_gds.wasm" -mexec-model=reactor

if [ $? -eq 0 ]; then
  echo "Success: parse_gds.wasm generated in web/."
  ls -lh "$ROOT/web/parse_gds.wasm"
else
  echo "Error: Compilation failed."
  exit 1
fi
