# Tiny Tapeout Explorer

Tiny Tapeout Explorer is an interactive web-based visualization tool for exploring integrated circuit designs from the Tiny Tapeout community. It features:

- **WASM-Powered Engine:** C++ Manhattan-optimized layout parser and circuit extractor running natively in the browser.
- **Advanced Visualization:** Rich 3D GDSII/OASIS exploration using WebGL 2.0 with wire-level highlights.
- **Switch-Level Simulation:** Interactive logic simulation with native VGA output and real-time signal monitoring.

This project serves as a major upgrade to the [original TT09 WebGL GDS Viewer](https://znah.net/tt09/).

**Author:** [Alexander Mordvintsev](https://znah.net/)

## Project Structure

- `src/`
  - Core C++ extraction, processing, and simulation engine (`main.cpp`, `extractor.h`, `cell_processing.h`, `cells.h`, `fetsim.h`, `vga.h`, `geom.h`).
  - WebAssembly memory management and allocator bindings (`wasm_allocator.cpp`).
  - Stub and utility headers (`qhull_stub/`).
- `web/`
  - WebAssembly build outputs (`explorer.wasm`) and Web Worker bridge (`gds_worker.js`).
  - WebGL 2.0 rendering engine and camera management (`circuit_viewer.js`).
  - Reactive user interface, tree navigator, and simulation controls (`viewer_ui.js`, `app.js`, `store.js`).
  - Styling and UI assets (`style.css`).
- `vendor/`
  - `gdstk/`: C++ library for GDSII and OASIS file ingestion (compiled with custom memory management flags).
  - `miniz/`: Lightweight C library for data compression and archive handling.
- `explorer.py`
  - Python wrapper and CLI utility for running extraction and inspection routines directly from the command line.
- `index.html` & `viewer.html`
  - Entry points for the web-based interactive layout exploration UI and circuit viewer.
- `Makefile` & `build.zig`
  - Native build configuration (`Makefile`) using Clang/GCC, and cross-platform/WebAssembly build configuration (`build.zig`, `build_wasm.sh`) using the Zig toolchain.

## Build Instructions

### Prerequisites
- **For Native Build:** `clang` or `gcc` with C++20 support, `make`.
- **For WebAssembly Build:** `zig` (version 0.12 or newer recommended) and `bash`.
- **Git Submodules:** Ensure all dependencies inside `vendor/` are initialized before building:
  ```bash
  git submodule update --init --recursive
  ```

### 1. Building for WebAssembly (Web Viewer)
To compile the C++ engine into WebAssembly (`web/explorer.wasm`) for use in the browser, run the provided shell script:
```bash
./build_wasm.sh
```
Alternatively, invoke the Zig build system directly:
```bash
zig build -Dtarget=wasm32-wasi --release=fast
cp zig-out/bin/explorer.wasm web/explorer.wasm
```

### 2. Building the Native CLI Binary
To build the native C++ executable (`explorer`) for command-line extraction and analysis on your host system:
```bash
make
```
To clean build artifacts:
```bash
make clean
```

### 3. Running the Web Application Locally
Once `explorer.wasm` is built, serve the repository root using any static local HTTP server (required to allow Web Worker and WASM file loading):
```bash
python3 -m http.server 8000
```
Then open `http://localhost:8000/viewer.html` or `http://localhost:8000/index.html` in your web browser.

## AI Use Disclosure

This project made extensive use of agentic coding assistants throughout its development cycle. Much of the web interface (`web/` components, reactive UI logic, and HTML/CSS styling) was "vibe-coded".

Conversely, the core C++ layout parsing engine, hierarchical extraction algorithms, switch-level simulator data structures in `src/` were either directly hand-written or crafted under rigorous manual supervision and architectural verification.
