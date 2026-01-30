# #!/bin/bash
# # Build explorer for WASM (Streaming mode)
# # Run from root: ./scripts/build_wasm.sh

# # Get the script's directory
# DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"
# ROOT="$DIR/.."

# echo "Building explorer.wasm..."
# zig c++ -target wasm32-wasi -O3 -fno-exceptions -fno-rtti \
#   -rdynamic -Wl,--no-entry -Wl,--strip-all \
#   -lc -lc++ -DWASM "$ROOT/src/main.cpp" -o "$ROOT/web/explorer.wasm" -mexec-model=reactor

# if [ $? -eq 0 ]; then
#   echo "Success: explorer.wasm generated in web/."
#   ls -lh "$ROOT/web/explorer.wasm"
# else
#   echo "Error: Compilation failed."
#   exit 1
# fi
