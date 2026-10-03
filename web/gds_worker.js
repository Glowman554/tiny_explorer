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

function progress(msg) {
    postMessage({ type: 'progress', message: msg });
}

async function fetchWithProgress(url, label = "GDS") {
    progress(`Fetching ${label}...`);
    const response = await fetch(url);
    if (!response.ok) {
        throw new Error(`HTTP ${response.status} fetching ${url}`);
    }

    const contentLength = response.headers.get('content-length');
    const total = contentLength ? parseInt(contentLength, 10) : 0;

    if (!response.body || !response.body.getReader) {
        return new Uint8Array(await response.arrayBuffer());
    }

    const reader = response.body.getReader();
    const chunks = [];
    let received = 0;
    let lastReport = performance.now();

    while (true) {
        const { done, value } = await reader.read();
        if (done) break;
        chunks.push(value);
        received += value.length;

        const now = performance.now();
        if (now - lastReport > 100) {
            lastReport = now;
            if (total > 0) {
                const percent = Math.round((received / total) * 100);
                progress(`Fetching ${label}: ${percent}% (${(received / 1024 / 1024).toFixed(1)} / ${(total / 1024 / 1024).toFixed(1)} MB)`);
            } else {
                progress(`Fetching ${label}: ${(received / 1024 / 1024).toFixed(1)} MB downloaded...`);
            }
        }
    }

    if (total > 0) {
        progress(`Fetching ${label}: 100% (${(received / 1024 / 1024).toFixed(1)} MB)`);
    } else {
        progress(`Fetched ${label}: ${(received / 1024 / 1024).toFixed(1)} MB`);
    }

    const result = new Uint8Array(received);
    let offset = 0;
    for (const chunk of chunks) {
        result.set(chunk, offset);
        offset += chunk.length;
    }
    return result;
}

let clkNetId = -1;
let simRunning = false;
let simSpeed = 0;
let autoClock = false;
let lastSimUpdate = 0;
let lastClkVal = 0;
let pendingVgaTick = false;
let pendingPeripheralClockTick = false;
const tt = { ids: new Map(), flash: new Uint8Array(16 * 1024 * 1024), psram: new Uint8Array(16 * 1024 * 1024), uart: '', prevSclk: 0, prevFlashCs: 1, prevRamCs: 1, spi: null, uartState: 'idle', uartCount: 0, uartBit: 0, uartByte: 0, inputFlash: 1 };

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
        const rawBytes = await fetchWithProgress(gdsUrl, isBrotli ? "Brotli GDS" : (isGzip ? "Gzipped GDS" : "GDS"));
        let data;

        if (isGzip) {
            progress("Decompressing GZip layout...");
            const stream = new Response(rawBytes).body.pipeThrough(new DecompressionStream('gzip'));
            data = new Uint8Array(await new Response(stream).arrayBuffer());
        } else if (isBrotli) {
            progress("Decompressing Brotli layout...");
            const brotli = new DecompressStream();
            const result = brotli.decompress(rawBytes, 200 * 1024 * 1024); // max 200MB
            data = result.buf;
        } else {
            data = rawBytes;
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
        progress("Initializing WASM engine...");
        const wasmResponse = await fetch('explorer.wasm');
        const wasmBuffer = await wasmResponse.arrayBuffer();
        const { instance: wasmInstance } = await WebAssembly.instantiate(wasmBuffer, {
            wasi_snapshot_preview1: wasi.wasiImport
        });
        instance = wasmInstance;
        
        wasi.initialize(instance);
        if (instance.exports.wasm_arena_init) instance.exports.wasm_arena_init(1); // Enable Arena Mode (1 = Arena, 0 = Heap)

        // 4. GDSTK Load phase
        progress("Parsing layout with GDSTK...");
        const loadStart = performance.now();
        const pathPtr = allocString(virtPath, instance);
        const pdkPtr = allocString(pdk || "", instance);

        instance.exports.wasm_load_file(pathPtr, pdkPtr);
        instance.exports.wasm_free(pathPtr);
        instance.exports.wasm_free(pdkPtr);
        const loadTime = performance.now() - loadStart;

        // 5. Processing phase
        progress("Extracting circuit topology & hierarchy...");
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
        clkNetId = -1;
        lastClkVal = 0;
        pendingVgaTick = false;
        pendingPeripheralClockTick = false;
        wireNames.forEach(w => {
            if (/^clk$/i.test(w.name)) clkNetId = w.id;
            const m = w.name.match(/^(ui_in|uo_out|uio_in|uio_out)(?:\[(\d+)\])?$/i);
            if (m) tt.ids.set(`${m[1].toLowerCase()}${m[2] === undefined ? '' : `[${m[2]}]`}`, w.id);
        });
        tt.flash.fill(0xff); tt.psram.fill(0); tt.uart = ''; tt.spi = null; tt.uartState = 'idle';
        tt.prevSclk = 0; tt.prevFlashCs = 1; tt.prevRamCs = 1; tt.inputFlash = 1;

        postMessage({ type: 'done', stats, file: gdsUrl, pdk, wireNames, hasSocTt: tt.ids.has('uio_out[0]') && tt.ids.has('uo_out[0]') }, transferables);

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
        if (instance.exports.wasm_circuit_is_settled) {
            payload.isSettled = !!instance.exports.wasm_circuit_is_settled();
        }
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
            if (simRunning) {
                simRunning = false;
                sendSimUpdate(true); // Final sync when stopping
            }
        }
    } else if (e.data.type === 'flash_hex') {
        const bytes = String(e.data.text || '').trim().split(/\s+/).filter(Boolean).map(token => parseInt(token.replace(/^0x/i, ''), 16));
        tt.flash.fill(0xff);
        for (let i = 0; i < Math.min(bytes.length, tt.flash.length); i++) tt.flash[i] = bytes[i];
        postMessage({ type: 'flash_loaded', count: Math.min(bytes.length, tt.flash.length) });
    } else if (e.data.type === 'ack_request') {
        postMessage({ type: 'ack_response' });
    }
};

// MARK: - Simulation Subsystem
let waveAccumulator = 0;
let lastSimLoopTime = 0;
let wireDataNeedsClear = false;

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
    let edgeDetected = false;
    if (clkVal === 1 && lastClkVal === 0) {
        edgeDetected = true;
    }
    lastClkVal = clkVal;
    return edgeDetected;
}

function pin(name, fallback = 1) { const id = tt.ids.get(name); return id === undefined ? fallback : getNetValue(id); }
function setPin(name, value) { const id = tt.ids.get(name); if (id !== undefined) instance.exports.wasm_circuit_set_input(id, value); }
function tickTinyTapeoutPeripherals(clockRising = false) {
    if (!tt.ids.size) return;
    const cs = pin('uio_out[0]'), ramA = pin('uio_out[6]'), ramB = pin('uio_out[7]');
    const sclk = pin('uio_out[3]'), mosi = pin('uio_out[1]');
    const selected = cs === 0 ? 'flash' : ramA === 0 ? 'psram_a' : ramB === 0 ? 'psram_b' : null;
    const wasSelected = tt.prevFlashCs === 0 ? 'flash' : tt.prevRamCs === 0 ? (tt.spi?.device || 'psram_a') : null;
    if (selected !== wasSelected) {
        if (tt.spi && (tt.spi.command === 2 || tt.spi.command === 3 || tt.spi.command === 0x0b)) {
            const op = tt.spi.command === 2 ? 'write' : 'read';
            const label = tt.spi.device === 'flash' ? 'FLASH' : `PSRAM ${tt.spi.device.slice(-1).toUpperCase()}`;
            const preview = tt.spi.preview?.length ? ` data=${tt.spi.preview.map(byte => byte.toString(16).padStart(2, '0')).join(' ')}` : '';
            postMessage({ type: 'peripheral_log', message: `[${label}] ${op} ${tt.spi.byteCount || 0} byte(s) at 0x${(tt.spi.startAddress || 0).toString(16).padStart(6, '0')}${preview}\n` });
        }
        if (!selected && tt.spi) setPin('uio_in[2]', 1);
        tt.spi = selected ? { device: selected, bits: 0, shift: 0, command: 0, address: 0, startAddress: 0, byteCount: 0, outBit: 0, readByte: 0, preview: [], dataStarted: false } : null;
    }
    if (tt.spi && sclk && !tt.prevSclk) {
        const p = tt.spi; p.shift = ((p.shift << 1) | mosi) & 255; p.bits++;
        if (p.bits === 8) {
            if (!p.command) p.command = p.shift;
            else if (!p.dataStarted) { p.address = ((p.address << 8) | p.shift) >>> 0; if (p.bitsTotal === undefined) p.bitsTotal = 0; p.bitsTotal += 8; if (p.bitsTotal === 24) { p.dataStarted = true; p.startAddress = p.address; } }
            else if (p.command === 2) { const mem = p.device === 'flash' ? tt.flash : tt.psram; const offset = p.device === 'psram_b' ? 8 * 1024 * 1024 : 0; const index = p.device === 'flash' ? p.address % mem.length : offset + (p.address % (8 * 1024 * 1024)); mem[index] = p.shift; if (p.preview.length < 8) p.preview.push(p.shift); p.address++; p.byteCount++; }
            p.bits = 0; p.shift = 0;
        }
    }
    // The SPI chips update MISO on falling SCLK, so it is stable for the next rising edge.
    if (tt.spi && !sclk && tt.prevSclk) {
        const p = tt.spi; let bit = 1;
        if (p.dataStarted && (p.command === 3 || p.command === 0x0b)) {
            const mem = p.device === 'flash' ? tt.flash : tt.psram;
            const offset = p.device === 'psram_b' ? 8 * 1024 * 1024 : 0;
            const index = p.device === 'flash' ? p.address % mem.length : offset + (p.address % (8 * 1024 * 1024));
            const out = mem[index];
            bit = (out >> (7 - p.outBit)) & 1;
            p.readByte = (p.readByte << 1) | bit;
            if (++p.outBit === 8) { p.outBit = 0; if (p.preview.length < 8) p.preview.push(p.readByte); p.readByte = 0; p.address++; p.byteCount++; }
        }
        setPin('uio_in[2]', bit);
    }
    tt.prevSclk = sclk; tt.prevFlashCs = cs; tt.prevRamCs = ramA && ramB ? 1 : 0;
    if (!clockRising) return;
    const tx = pin('uo_out[0]');
    if (tt.uartState === 'idle') { if (tx === 0) { tt.uartState = 'start'; tt.uartCount = 116; tt.uartBit = 0; tt.uartByte = 0; } }
    else if (--tt.uartCount <= 0) {
        if (tt.uartState === 'start') { if (tx === 0) { tt.uartState = 'data'; tt.uartCount = 233; } else tt.uartState = 'idle'; }
        else if (tt.uartState === 'data') { tt.uartByte |= tx << tt.uartBit++; tt.uartCount = 233; if (tt.uartBit === 8) tt.uartState = 'stop'; }
        else { if (tx) { tt.uart += String.fromCharCode(tt.uartByte); postMessage({ type: 'uart', text: tt.uart }); } tt.uartState = 'idle'; }
    }
}

function sendSimUpdate(forceWireData = false) {
    const payload = { type: 'simUpdate' };
    const transferables = [];
    
    if (forceWireData && instance.exports.wasm_wireData_ptr) {
        const data = new Uint8Array(instance.exports.memory.buffer, instance.exports.wasm_wireData_ptr(), instance.exports.wasm_wireData_size()).slice();
        payload.wireData = data;
        if (instance.exports.wasm_circuit_is_settled) {
            payload.isSettled = !!instance.exports.wasm_circuit_is_settled();
        }
        transferables.push(data.buffer);
    }
    
    if (instance.exports.wasm_vga_buffer_ptr) {
        const vga = {
            stride: instance.exports.wasm_vga_stride(),
            width: instance.exports.wasm_vga_width(),
            height: instance.exports.wasm_vga_height(),
            rayX: instance.exports.wasm_vga_ray_x(),
            rayY: instance.exports.wasm_vga_ray_y(),
        };
        const vgaPtr = instance.exports.wasm_vga_buffer_ptr();
        if (vgaPtr) {
            const data = new Uint8Array(instance.exports.memory.buffer, vgaPtr, instance.exports.wasm_vga_buffer_size()).slice();
            vga.buffer = data;
            transferables.push(data.buffer);
        }
        payload.vga = vga;
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
            if (instance.exports.wasm_circuit_is_settled()) {
                tickTinyTapeoutPeripherals(pendingPeripheralClockTick);
                pendingPeripheralClockTick = false;
                if (pendingVgaTick) {
                    if (instance.exports.wasm_vga_tick) instance.exports.wasm_vga_tick(1);
                    pendingVgaTick = false;
                }
            }
            didWork = true;
            iterCount--;
        } else if (autoClock && clkNetId !== -1) {
            const val = getNetValue(clkNetId);
            instance.exports.wasm_circuit_set_input(clkNetId, val ? 0 : 1);
            if (checkClockEdge()) {
                pendingVgaTick = true;
                pendingPeripheralClockTick = true;
            }
            
            // If it settled instantly (no logic depth or no feedback)
            if (instance.exports.wasm_circuit_is_settled() && pendingVgaTick) {
                if (instance.exports.wasm_vga_tick) instance.exports.wasm_vga_tick(1);
                pendingVgaTick = false;
            }
            didWork = true;
            iterCount--;
        } else {
            break; // Settled and no edge to trigger
        }
    }
    
    const forceResync = (now - lastSimUpdate) > 100;
    const isSettled = instance.exports.wasm_circuit_is_settled();
    
    if (didWork) {
        wireDataNeedsClear = true;
    }
    
    let sendWireData = didWork;
    if (!didWork && forceResync && isSettled && wireDataNeedsClear) {
        sendWireData = true;
        wireDataNeedsClear = false;
    }
    
    if (didWork || forceResync) {
        // limit UI updates to ~60fps (16ms)
        // BUT: If the circuit is settled and we just did work, this is a 'final' state, send it immediately
        if ((now - lastSimUpdate) >= 16 || (isSettled && didWork)) {
            sendSimUpdate(sendWireData);
        }
    }
    
    scheduleNext(simLoop);
}
