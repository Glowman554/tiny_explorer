/**
 * Unified GDS Worker
 * Supports both streaming visualization (viewer.html) and benchmarking (bench.html).
 * Integrates brotli-dec-wasm for .gds.br support.
 */

import brotliInit, { DecompressStream, BrotliStreamResultCode } from '../vendor/brotli_dec_wasm.js';
import { WASI, File, PreopenDirectory, Fd, ConsoleStdout } from '../vendor/browser_wasi_shim.js';

let instance;
let logBuffer = "";

function log(msg) {
    postMessage({ type: 'log', message: msg });
}

const files = new Map();

function allocString(str, instance) {
    const bytes = new TextEncoder().encode(str);
    const ptr = instance.exports.wasm_malloc(bytes.length + 1);
    const buf = new Uint8Array(instance.exports.memory.buffer, ptr, bytes.length + 1);
    buf.set(bytes);
    buf[bytes.length] = 0;
    return ptr;
}

function getString(ptr) {
    if (!ptr) return null;
    const buf = new Uint8Array(instance.exports.memory.buffer);
    let end = ptr;
    while (buf[end]) end++;
    return new TextDecoder().decode(buf.slice(ptr, end));
}

function getWireNames() {
    const count = instance.exports.wasm_circuit_get_labeled_count();
    const labeled = [];
    for (let i = 0; i < count; i++) {
        labeled.push({
            id: instance.exports.wasm_circuit_get_labeled_id(i),
            name: getString(instance.exports.wasm_circuit_get_labeled_name(i))
        });
    }
    return labeled;
}

async function runGdsTask(gdsUrl, pdk, options = {}) {
    logBuffer = "";
    const { returnGeometry = true } = options;
    
    const urlPath = gdsUrl.toLowerCase().split(/[?#]/)[0];
    const isBrotli = urlPath.endsWith('.br');
    const isGzip = urlPath.endsWith('.gz');
    
    try {
        if (isBrotli) await brotliInit();

        // 1. Fetch and decompress data
        const startTime = performance.now();
        const response = await fetch(gdsUrl);
        let data;

        if (isGzip) {
            const stream = response.body.pipeThrough(new DecompressionStream('gzip'));
            data = new Uint8Array(await new Response(stream).arrayBuffer());
        } else if (isBrotli) {
            const compressed = new Uint8Array(await response.arrayBuffer());
            const brotli = new DecompressStream();
            const result = brotli.decompress(compressed, 200 * 1024 * 1024); // max 200MB
            data = result.buf;
        } else {
            data = new Uint8Array(await response.arrayBuffer());
        }

        const totalBytes = data.length;
        const virtPath = urlPath.substring(urlPath.lastIndexOf('/') + 1).replace('.gz', '').replace('.br', '');

        // 2. Setup WASI
        const stdoutHandler = bytes => {
            const text = new TextDecoder().decode(bytes);
            logBuffer += text;
            log(text);
        };

        const fds = [
            new Fd(),
            new ConsoleStdout(stdoutHandler),
            new ConsoleStdout(stdoutHandler),
            new PreopenDirectory(".", new Map([[virtPath, new File(data)]]))
        ];

        const wasi = new WASI(["explorer"], [], fds);

        // 3. Initialize WASM
        const wasmResponse = await fetch('explorer.wasm');
        const wasmBuffer = await wasmResponse.arrayBuffer();
        const { instance: wasmInstance } = await WebAssembly.instantiate(wasmBuffer, {
            wasi_snapshot_preview1: wasi.wasiImport
        });
        instance = wasmInstance;
        
        wasi.initialize(instance);
        instance.exports.wasm_init();
        if (instance.exports.wasm_arena_init) instance.exports.wasm_arena_init(1); // Enable Arena Mode (1 = Arena, 0 = Heap)

        // 4. GDSTK Load phase
        const loadStart = performance.now();
        const pathPtr = allocString(virtPath, instance);
        const pdkPtr = allocString(pdk || "", instance);

        instance.exports.wasm_load_file(pathPtr, pdkPtr);
        instance.exports.wasm_free(pathPtr);
        instance.exports.wasm_free(pdkPtr);
        const loadTime = performance.now() - loadStart;

        // 5. Processing phase
        const procStart = performance.now();

        instance.exports.wasm_process();
        const procTime = performance.now() - procStart;
        
        const totalTime = performance.now() - startTime;
        const arenaUsage = instance.exports.wasm_arena_get_usage ? Number(instance.exports.wasm_arena_get_usage()) : 0;
        const memMB = (arenaUsage > 0 ? arenaUsage : instance.exports.memory.buffer.byteLength) / 1024 / 1024;

        const stats = {
            totalBytes,
            totalTime,
            loadTime,
            procTime,
            wallTime: totalTime,
            memMB: memMB.toFixed(2),
            peakMemMB: memMB.toFixed(2),
            flatRects: parseInt(logBuffer.match(/Total flat rects: (\d+)/)?.[1] || 0),
            flatFETs: 0, // Not currently explicitly logged
            netlistWires: parseInt(logBuffer.match(/Wires: (\d+)/)?.[1] || 0),
            netlistFETs: parseInt(logBuffer.match(/FETs: (\d+)/)?.[1] || 0),
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
                // rectDataSize is in bytes, Int32Array expects element count
                const rectData = new Int32Array(instance.exports.memory.buffer, rectDataPtr, rectDataSize / 4).slice();
                stats.rectData = rectData;
                transferables.push(rectData.buffer);
            }
            if (layerOffsetsPtr && layerOffsetsSize) {
                // layerOffsetsSize is in bytes, Uint32Array expects element count
                const layerOffsets = new Uint32Array(instance.exports.memory.buffer, layerOffsetsPtr, layerOffsetsSize / 4).slice();
                stats.layerOffsets = layerOffsets;
                transferables.push(layerOffsets.buffer);
            }
        }

        const wireNames = getWireNames();
        postMessage({ type: 'done', stats, file: gdsUrl, pdk, wireNames }, transferables);

    } catch (err) {
        postMessage({ type: 'error', file: gdsUrl, message: err.message });
    }
}

onmessage = function(e) {
    if (e.data.type === 'start') {
        runGdsTask(e.data.gdsUrl, e.data.pdk, e.data.options || {});
    } else if (e.data.type === 'call') {
        const { name, args, returnArrays } = e.data;
        const fn = instance.exports[name];
        if (!fn) {
            postMessage({ type: 'error', message: `WASM function not found: ${name}` });
            return;
        }
        const result = fn(...(args || []));
        
        const payload = { type: 'callResult', name, result };
        const transferables = [];
        
        if (returnArrays) {
            returnArrays.forEach(arrName => {
                if (arrName === 'wireData') {
                    const ptr = instance.exports.wasm_circuit_get_wire_data_ptr();
                    const count = instance.exports.wasm_circuit_get_wire_count();
                    if (ptr) {
                        const data = new Uint8Array(instance.exports.memory.buffer, ptr, count).slice();
                        payload.wireData = data;
                        transferables.push(data.buffer);
                    }
                }
                if (arrName === 'fetOn') {
                    const ptr = instance.exports.wasm_circuit_get_fet_on_ptr();
                    const count = instance.exports.wasm_circuit_get_fet_count();
                    if (ptr) {
                        const data = new Uint8Array(instance.exports.memory.buffer, ptr, count).slice();
                        payload.fetOn = data;
                        transferables.push(data.buffer);
                    }
                }
            });
        }
        postMessage(payload, transferables);
    }
};
