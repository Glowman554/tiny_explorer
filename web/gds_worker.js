/**
 * Unified GDS Worker
 * Supports both streaming visualization (viewer.html) and benchmarking (bench.html).
 * Integrates brotli-dec-wasm for .gds.br support.
 */

// MARK: - Imports & Globals
import brotliInit, { DecompressStream, BrotliStreamResultCode } from '../vendor/brotli_dec_wasm.js';
import { WASI, File, PreopenDirectory, Fd, ConsoleStdout } from '../vendor/browser_wasi_shim.js';

let instance;
let logBuffer = "";

function log(msg) {
    postMessage({ type: 'log', message: msg });
}

let clkNetId = -1;
let simRunning = false;
let simSpeed = 0;
let autoClock = false;
let lastSimUpdate = 0;
let lastClkVal = 0;

const files = new Map();

// MARK: - WASM Memory Utilities
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
    const count = instance.exports.wasm_labeledCount();
    const labeled = [];
    for (let i = 0; i < count; i++) {
        labeled.push({
            id: instance.exports.wasm_circuit_get_labeled_id(i),
            name: getString(instance.exports.wasm_circuit_get_labeled_name(i))
        });
    }
    return labeled;
}

// MARK: - GDS Processing
async function runGdsTask(gdsUrl, pdk, options = {}) {
    logBuffer = "";
    const { returnGeometry = true } = options;
    
    const targetUrl = options.filename || gdsUrl;
    const urlPath = targetUrl.toLowerCase().split(/[?#]/)[0];
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
        const memMB = instance.exports.memory.buffer.byteLength / 1024 / 1024;

        log(`\n--- Stats ---\n`);
        log(`Load time: ${loadTime.toFixed(2)}ms\n`);
        log(`Process time: ${procTime.toFixed(2)}ms\n`);
        log(`Total wall time: ${totalTime.toFixed(2)}ms\n`);
        log(`Memory usage: ${memMB.toFixed(2)}MB\n`);

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
            errors: (logBuffer.match(/Error:/g) || []).length,
            vgaWidth: instance.exports.wasm_vga_width ? instance.exports.wasm_vga_width() : 0,
            vgaHeight: instance.exports.wasm_vga_height ? instance.exports.wasm_vga_height() : 0
        };

        const transferables = [];
        if (returnGeometry) {
            console.log(instance.exports);
            const rectDataPtr = instance.exports.wasm_flatRects_ptr();
            const rectDataSize = instance.exports.wasm_flatRects_size();
            const layerOffsetsPtr = instance.exports.wasm_flatLayerOffsets_ptr();
            const layerOffsetsSize = instance.exports.wasm_flatLayerOffsets_size();

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
        wireNames.forEach(w => {
            if (/^clk$/i.test(w.name)) clkNetId = w.id;
        });

        postMessage({ type: 'done', stats, file: gdsUrl, pdk, wireNames }, transferables);

    } catch (err) {
        postMessage({ type: 'error', file: gdsUrl, message: err.message });
    }
}

// MARK: - Worker Message Loop
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
                const getPtr = instance.exports[`wasm_${arrName}_ptr`];
                const getSize = instance.exports[`wasm_${arrName}_size`];
                if (getPtr && getSize) {
                    const ptr = getPtr();
                    if (ptr) {
                        const count = getSize();
                        const data = new Uint8Array(instance.exports.memory.buffer, ptr, count).slice();
                        payload[arrName] = data;
                        transferables.push(data.buffer);
                    }
                }
            });
        }
        postMessage(payload, transferables);
    } else if (e.data.type === 'sim_config') {
        const { speed, autoClock: autoClk } = e.data;
        simSpeed = speed;
        autoClock = autoClk;
        
        const shouldRun = simSpeed > 0 || autoClock;
        if (shouldRun && !simRunning) {
            simRunning = true;
            lastSimLoopTime = performance.now();
            waveAccumulator = 0;
            scheduleNext(simLoop);
        } else if (!shouldRun) {
            simRunning = false;
        }
    } else if (e.data.type === 'ack_request') {
        postMessage({ type: 'ack_response' });
    }
};

// MARK: - Simulation Subsystem
let waveAccumulator = 0;
let lastSimLoopTime = 0;

function scheduleNext(cb) {
    if (typeof requestAnimationFrame !== 'undefined') {
        requestAnimationFrame(cb);
    } else {
        setTimeout(cb, 0);
    }
}

function getNetValue(netId) {
    if (netId === -1 || !instance?.exports?.wasm_wireData_ptr) return 0;
    const wireData = new Uint8Array(instance.exports.memory.buffer, instance.exports.wasm_wireData_ptr(), instance.exports.wasm_wireData_size());
    return wireData[netId] & 1;
}

function checkClockEdge() {
    const clkVal = getNetValue(clkNetId);
    if (clkVal === 1 && lastClkVal === 0) {
        if (instance.exports.wasm_vga_tick) instance.exports.wasm_vga_tick(1);
    }
    lastClkVal = clkVal;
}

function sendSimUpdate() {
    const payload = { type: 'simUpdate' };
    const transferables = [];
    
    if (instance.exports.wasm_wireData_ptr) {
        const data = new Uint8Array(instance.exports.memory.buffer, instance.exports.wasm_wireData_ptr(), instance.exports.wasm_wireData_size()).slice();
        payload.wireData = data;
        transferables.push(data.buffer);
    }
    
    if (instance.exports.wasm_vga_buffer_ptr) {
        const vgaPtr = instance.exports.wasm_vga_buffer_ptr();
        if (vgaPtr) {
            const data = new Uint8Array(instance.exports.memory.buffer, vgaPtr, instance.exports.wasm_vga_buffer_size()).slice();
            payload.vga_buffer = data;
            transferables.push(data.buffer);
        }
        payload.rayX = instance.exports.wasm_vga_ray_x ? instance.exports.wasm_vga_ray_x() : 0;
        payload.rayY = instance.exports.wasm_vga_ray_y ? instance.exports.wasm_vga_ray_y() : 0;
    }
    
    postMessage(payload, transferables);
    lastSimUpdate = performance.now();
}

function simLoop() {
    if (!simRunning) return;
    
    if (!instance || !instance.exports.wasm_circuit_is_settled) {
        scheduleNext(simLoop);
        return;
    }

    const now = performance.now();
    const isMax = simSpeed === 100;
    const timeLimit = isMax ? (now + 14) : (now + 2); // Spend up to 14ms (max) or 2ms (throttled)
    
    let iterCount = 0;
    
    if (isMax) {
        iterCount = 100000;
    } else if (simSpeed > 0) {
        // Clamp delta to prevent massive jumps if tab was sleeping
        const delta = Math.min(now - lastSimLoopTime, 100);
        
        // Logarithmic scale so speed=1 => 1 wps, speed=99 => 10000 wps
        const targetWavesPerSec = Math.pow(10, ((simSpeed - 1) / 98) * 4);
        waveAccumulator += (delta / 1000.0) * targetWavesPerSec;
        
        iterCount = Math.floor(waveAccumulator);
        if (iterCount > 0) {
            waveAccumulator -= iterCount;
        }
    } else if (autoClock) {
        // Speed slider is 0 (Paused) but autoClock is manually on
        // Leave logic iterCount at 0 so only clock edges run
        iterCount = 0;
    }
    
    lastSimLoopTime = now;

    let didWork = false;

    while (iterCount > 0 && performance.now() < timeLimit) {
        let isSettled = instance.exports.wasm_circuit_is_settled();
        
        if (!isSettled) {
            instance.exports.wasm_circuit_run_wave();
            checkClockEdge();
            didWork = true;
            iterCount--;
        } else if (autoClock && clkNetId !== -1) {
            const val = getNetValue(clkNetId);
            instance.exports.wasm_circuit_set_input(clkNetId, val ? 0 : 1);
            checkClockEdge();
            didWork = true;
            iterCount--;
        } else {
            break; // Settled and no edge to trigger
        }
    }
    
    if (didWork || (now - lastSimUpdate) > 100) {
        // limit UI updates to ~60fps (16ms)
        if ((now - lastSimUpdate) >= 16) {
            sendSimUpdate();
        }
    }
    
    scheduleNext(simLoop);
}
