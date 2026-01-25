/**
 * Unified GDS Worker
 * Supports both streaming visualization (viewer.html) and benchmarking (bench.html).
 * Integrates brotli-dec-wasm for .gds.br support.
 */

import brotliInit, { DecompressStream, BrotliStreamResultCode } from '../vendor/brotli_dec_wasm.js';

let instance;
let logBuffer = "";

function log(msg) {
    postMessage({ type: 'log', message: msg });
}

const wasiShim = {
    fd_write: (fd, iovs, iovsLen, nwritten) => {
        const view = new DataView(instance.exports.memory.buffer);
        let written = 0;
        for (let i = 0; i < iovsLen; i++) {
            const ptr = view.getUint32(iovs + i * 8, true);
            const len = view.getUint32(iovs + i * 8 + 4, true);
            const bytes = new Uint8Array(instance.exports.memory.buffer, ptr, len);
            const text = new TextDecoder().decode(bytes);
            logBuffer += text;
            log(text); // Send to main thread for real-time display
            written += len;
        }
        view.setUint32(nwritten, written, true);
        return 0;
    },
    fd_prestat_get: () => 8,
    fd_prestat_dir_name: () => 8,
    args_sizes_get: (argc, bufSize) => {
        const view = new DataView(instance.exports.memory.buffer);
        view.setUint32(argc, 0, true);
        view.setUint32(bufSize, 0, true);
        return 0;
    },
    args_get: () => 0,
    clock_time_get: () => 0,
    proc_exit: (code) => console.log(`Process exited with code ${code}`),
    environ_sizes_get: (n, s) => { 
        const view = new DataView(instance.exports.memory.buffer);
        view.setUint32(n, 0, true); 
        view.setUint32(s, 0, true); 
        return 0; 
    },
    environ_get: () => 0,
    fd_close: () => 0,
    fd_seek: () => 0,
    fd_fdstat_get: () => 0,
};

async function runGdsTask(gdsUrl, options = {}) {
    logBuffer = "";
    const { returnGeometry = true } = options;
    const urlPath = gdsUrl.toLowerCase().split(/[?#]/)[0];
    const isBrotli = urlPath.endsWith('.br');
    const isGzip = urlPath.endsWith('.gz');

    try {
        // Initialize Brotli if needed
        if (isBrotli) {
            await brotliInit();
        }

        const wasmResponse = await fetch('parse_gds.wasm');
        const wasmBuffer = await wasmResponse.arrayBuffer();
        const { instance: wasmInstance } = await WebAssembly.instantiate(wasmBuffer, {
            wasi_snapshot_preview1: wasiShim
        });
        
        instance = wasmInstance;
        instance.exports.wasm_init();

        const response = await fetch(gdsUrl);
        let stream = response.body;

        if (isGzip) {
            if (typeof DecompressionStream === 'undefined') {
                throw new Error("GZIP decompression (DecompressionStream) is not supported in this browser.");
            }
            stream = stream.pipeThrough(new DecompressionStream('gzip'));
        }

        const reader = stream.getReader();

        let totalBytes = 0;
        const startTime = performance.now();
        let chunkPtr = 0;
        let chunkCap = 0;

        const brotliStream = isBrotli ? new DecompressStream() : null;

        while (true) {
            const { done, value } = await reader.read();
            if (done) break;

            let dataToParse = value;
            if (isBrotli) {
                // Decompress chunk
                let input = value;
                while (true) {
                    const result = brotliStream.decompress(input, 1024 * 1024); // 1MB buffer
                    const uncompressed = result.buf;
                    
                    if (uncompressed.length > 0) {
                        pushToParser(uncompressed);
                    }
                    
                    input = input.subarray(result.input_offset);
                    if (result.code === BrotliStreamResultCode.ResultSuccess) break;
                    if (result.code === BrotliStreamResultCode.NeedsMoreInput && input.length === 0) break;
                }
            } else {
                pushToParser(value);
            }
        }

        function pushToParser(data) {
            const len = data.length;
            totalBytes += len;

            if (len > chunkCap) {
                if (chunkPtr) instance.exports.wasm_free(chunkPtr);
                chunkCap = Math.max(len, 256 * 1024);
                chunkPtr = instance.exports.wasm_malloc(chunkCap);
            }

            const wasmBuf = new Uint8Array(instance.exports.memory.buffer, chunkPtr, len);
            wasmBuf.set(data);
            instance.exports.wasm_push_chunk(chunkPtr, len);
        }

        if (chunkPtr) instance.exports.wasm_free(chunkPtr);
        instance.exports.wasm_finalize();
        
        const totalTime = performance.now() - startTime;
        const memMB = (instance.exports.memory.buffer.byteLength / 1024 / 1024).toFixed(2);

        const stats = {
            totalBytes,
            totalTime,
            wallTime: totalTime,
            memMB,
            peakMemMB: memMB,
            flatRects: parseInt(logBuffer.match(/FlatRects=(\d+)/)?.[1] || 0),
            flatFETs: parseInt(logBuffer.match(/FlatFETs=(\d+)/)?.[1] || 0),
            netlistWires: parseInt(logBuffer.match(/NetlistWires=(\d+)/)?.[1] || 0),
            netlistFETs: parseInt(logBuffer.match(/NetlistFETs=(\d+)/)?.[1] || 0),
            warnings: (logBuffer.match(/Warning:/g) || []).length,
            errors: (logBuffer.match(/Error:/g) || []).length
        };

        const transferables = [];
        if (returnGeometry) {
            const rectDataPtr = instance.exports.wasm_get_rect_data_ptr();
            const rectDataSize = instance.exports.wasm_get_rect_data_size();
            const layerOffsetsPtr = instance.exports.wasm_get_layer_offsets_ptr();
            const layerOffsetsSize = instance.exports.wasm_get_layer_offsets_size();

            if (rectDataPtr && rectDataSize) {
                const rectData = new Int32Array(instance.exports.memory.buffer, rectDataPtr, rectDataSize).slice();
                stats.rectData = rectData;
                transferables.push(rectData.buffer);
            }
            if (layerOffsetsPtr && layerOffsetsSize) {
                const layerOffsets = new Uint32Array(instance.exports.memory.buffer, layerOffsetsPtr, layerOffsetsSize).slice();
                stats.layerOffsets = layerOffsets;
                transferables.push(layerOffsets.buffer);
            }
        }

        postMessage({ type: 'done', stats, file: gdsUrl }, transferables);

    } catch (err) {
        postMessage({ type: 'error', file: gdsUrl, message: err.message });
    }
}

onmessage = function(e) {
    if (e.data.type === 'start') {
        runGdsTask(e.data.gdsUrl, e.data.options || {});
    }
};
