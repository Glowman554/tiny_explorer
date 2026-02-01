const VS_SOURCE = `#version 300 es
    in vec3 a_pos;       // Box vertex position (0..1)
    in ivec4 a_rect;     // Rect: x1, y1, x2, y2
    in int a_net;        // Net ID
    
    uniform mat4 u_viewMat;
    uniform float u_layerZ;
    uniform float u_thickness;
    uniform float u_showPowerNets;
    uniform float u_isExemptLayer;
    
    flat out int v_net;
    out vec2 v_pos;
    out float v_light;

    void main() {
        if (u_showPowerNets < 0.5 && u_isExemptLayer < 0.5) {
            if (a_net == 0 || a_net == 1) {
                // Collapse the geometry to avoid rasterization
                gl_Position = vec4(0.0, 0.0, 0.0, 0.0);
                return;
            }
        }

        vec2 rectPos = mix(vec2(a_rect.xy), vec2(a_rect.zw), a_pos.xy);
        float z = u_layerZ + a_pos.z * u_thickness;
        vec4 p = vec4(rectPos, z, 1.0);
        gl_Position = u_viewMat * p;
        // z-fighting mitigation
        gl_Position.z += float(gl_InstanceID % 64) * 1e-6;
        
        v_net = a_net;
        v_pos = a_pos.xy;
        v_light = 0.7 + 0.3 * a_pos.z; // Simple top lighting
    }
`;

const FS_SOURCE = `#version 300 es
    precision mediump float;
    
    uniform vec4 u_color;
    uniform float u_globalAlpha;
    uniform float u_showBoundaries;
    uniform float u_isHighlighting; // 1.0 if any net is highlighted
    uniform lowp usampler2D u_netStates;
    uniform ivec2 u_netStatesSize;
    
    flat in int v_net;
    in vec2 v_pos;
    in float v_light;
    
    out vec4 fragColor;

    void main() {
        vec4 color = u_color;
        color.rgb *= v_light;
        
        bool isHighlighted = false;
        
        if (v_net >= 0 && u_netStatesSize.x > 0) {
            ivec2 texCoords = ivec2(v_net % u_netStatesSize.x, v_net / u_netStatesSize.x);
            uint state = texelFetch(u_netStates, texCoords, 0).r;
            if ((state & 1u) != 0u) isHighlighted = true;
        }

        if (u_isHighlighting > 0.5) {
            if (!isHighlighted) {
                // Dim non-highlighted nets
                color.a *= 0.1;
                color.rgb *= 0.5;
            } else {
                // Highlight
                color.rgb = mix(color.rgb, vec3(1.0, 1.0, 1.0), 0.3);
                color.a = 1.0;
            }
        }
        
        
        if (u_showBoundaries > 0.5) {
            // Boundary drawing might need adjustment for 3D/Perspective if we kept v_sizeScreen
            // For now, let's just use a simple UV-based boundary
            vec2 b = smoothstep(0.0, 0.02, v_pos) * smoothstep(1.0, 0.98, v_pos);
            if (b.x * b.y < 0.5) {
                color = vec4(1.0, 1.0, 1.0, 1.0);
            }
        }
        
        fragColor = color;
        fragColor.a *= u_globalAlpha;
    }
`;

// Standard GDS Layer Colors (approximate)
let z = 0, h=0;
const h_met=200, h_via=500;
const LAYER_CONFIG = [
    { name: "ERRORS",  color: [1.0, 0.5, 0.0, 1.0], z: 5000, h: 500 },
    { name: "NWELL",   color: [0.3, 0.1, 0.2, 1.0], z: z,    h: h=100 },
    { name: "DIFF",    color: [0.2, 0.8, 0.2, 1.0], z: z+=h, h },
    { name: "CHANNEL", color: [0.8, 0.6, 0.6, 1.0], z: z,    h },
    { name: "N_TERM",  color: [0.2, 0.6, 0.2, 1.0], z: z,    h },
    { name: "P_TERM",  color: [0.8, 0.8, 0.2, 1.0], z: z,    h },
    { name: "POLY",    color: [0.8, 0.2, 0.2, 1.0], z: z+=h, h },
    { name: "LICON",   color: [0.5, 0.5, 0.5, 1.0], z: z,    h: h=600 },
    { name: "LI1",     color: [0.3, 0.3, 0.9, 1.0], z: z+=h, h: h=h_met },
    { name: "MCON",    color: [0.6, 0.6, 0.6, 1.0], z: z+=h, h: h=h_via },
    { name: "MET1",    color: [0.7, 0.4, 0.8, 1.0], z: z+=h, h: h=h_met },
    { name: "VIA1",    color: [0.8, 0.8, 0.8, 1.0], z: z+=h, h: h=h_via },
    { name: "MET2",    color: [0.4, 0.8, 0.8, 1.0], z: z+=h, h: h=h_met },
    { name: "VIA2",    color: [0.9, 0.9, 0.9, 1.0], z: z+=h, h: h=h_via },
    { name: "MET3",    color: [0.8, 0.8, 0.2, 1.0], z: z+=h, h: h=h_met },
    { name: "VIA3",    color: [0.9, 0.9, 0.9, 1.0], z: z+=h, h: h=h_via },
    { name: "MET4",    color: [0.2, 0.6, 0.2, 1.0], z: z+=h, h: h=h_met },
    { name: "VIA4",    color: [0.9, 0.9, 0.9, 1.0], z: z+=h, h: h=h_via },
    { name: "MET5",    color: [0.6, 0.2, 0.6, 1.0], z: z+=h, h: h=h_met },
];
console.log(LAYER_CONFIG);

let gl;
let program;
let layers = {}; // { layerID: { count: N, buffer: WebGLBuffer, visible: bool } }
let sortedLids = [];
let layerSpecs = {}; // { lid: { z, h } }
let view = { 
    centerX: 0, centerY: 0, 
    log2zoom: 0, pan: 0, tilt: 0, perspective: 0.5,
    baseScale: 0.001
};
let isDragging = false;
let lastMouse = { x: 0, y: 0 };
let minX = 0, minY = 0, maxX = 0, maxY = 0, isLoaded = false;
let worker = null;
let wireNames = [];
let netAreas = {};

function updateBaseScale() {
    if (!isLoaded) return;
    const width = maxX - minX;
    const height = maxY - minY;
    const viewAspect = gl.canvas.width / Math.max(gl.canvas.height, 1);
    view.baseScale = (width / height > viewAspect) ? (1.0 / width) : (1.0 / height / viewAspect);
}

function resize() {
    const canvas = document.getElementById('glcanvas');
    if (!canvas) return;
    const dpr = window.devicePixelRatio || 1;
    canvas.width = window.innerWidth * dpr;
    canvas.height = window.innerHeight * dpr;
    gl.viewport(0, 0, canvas.width, canvas.height);
    updateBaseScale();
    requestAnimationFrame(render);
}

function updateLayerHeights() {
    for (let i = 0; i < LAYER_CONFIG.length; i++) {
        layerSpecs[i] = { z: LAYER_CONFIG[i].z, h: LAYER_CONFIG[i].h };
    }
}

function _handleMove(dx, dy, isRotate) {
    if (isRotate) {
        view.pan -= dx;
        view.tilt -= dy;
    } else {
        const speed = Math.pow(2.0, -view.log2zoom) / view.baseScale;
        const s = Math.sin(-view.pan), c = Math.cos(view.pan);
        view.centerX += (c * dx + s * dy) * speed;
        view.centerY += (s * dx - c * dy) * speed;
    }
}

let highlightNet = -1;
let highlightedCount = 0;
let soloLayerId = null;
let netStateTexture = null;
let netStatesSize = [0, 0];
let netStateData = null;

function initWebGL() {
    const canvas = document.getElementById('glcanvas');
    gl = canvas.getContext('webgl2', { alpha: false, antialias: true });
    if (!gl) { alert('WebGL2 not supported'); return; }
    

    // Shader Setup
    const vs = createShader(gl, gl.VERTEX_SHADER, VS_SOURCE);
    const fs = createShader(gl, gl.FRAGMENT_SHADER, FS_SOURCE);
    program = createProgram(gl, vs, fs);
    gl.useProgram(program);

    // Cube Geometry (Shared, Indexed)
    const cubeVerts = new Float32Array([
        0,0,0, 1,0,0, 1,1,0, 0,1,0,
        0,0,1, 1,0,1, 1,1,1, 0,1,1
    ]);
    const cubeIndices = new Uint16Array([
        0, 2, 1, 0, 3, 2, // Bottom
        4, 5, 6, 4, 6, 7, // Top
        0, 1, 5, 0, 5, 4, // Front
        2, 3, 7, 2, 7, 6, // Back
        1, 2, 6, 1, 6, 5, // Right
        3, 0, 4, 3, 4, 7  // Left
    ]);

    const vbo = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, vbo);
    gl.bufferData(gl.ARRAY_BUFFER, cubeVerts, gl.STATIC_DRAW);
    
    const ebo = gl.createBuffer();
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, ebo);
    gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, cubeIndices, gl.STATIC_DRAW);
    
    const a_pos = gl.getAttribLocation(program, "a_pos");
    gl.enableVertexAttribArray(a_pos);
    gl.vertexAttribPointer(a_pos, 3, gl.FLOAT, false, 0, 0);

    // Initial GL State
    gl.enable(gl.CULL_FACE);
    gl.cullFace(gl.BACK);

    // Resize Handler
    window.addEventListener('resize', resize);
    resize();

    // Input Handlers
    canvas.addEventListener('mousedown', e => { 
        if (e.button !== 0) return;
        isDragging = true; 
        lastMouse = { x: e.clientX, y: e.clientY }; 
    });
    window.addEventListener('mouseup', () => isDragging = false);
    canvas.addEventListener('mousemove', e => {
        if (!isDragging) return;
        const dx = (e.clientX - lastMouse.x) * 0.001;
        const dy = (e.clientY - lastMouse.y) * 0.001;
        const isRotate = e.shiftKey || (view.rotateMode || false);
        _handleMove(-dx, -dy, isRotate);
        lastMouse = { x: e.clientX, y: e.clientY };
        requestAnimationFrame(render);
    });
    canvas.addEventListener('wheel', e => {
        e.preventDefault();
        const dy = e.deltaY * 0.001;
        if (e.ctrlKey) {
            view.log2zoom -= dy * 10.0;
        } else {
            const dx = e.deltaX * 0.001;
            _handleMove(dx, dy, e.shiftKey);
        }
        requestAnimationFrame(render);
    }, {passive: false});

    window.addEventListener('keydown', e => {
        if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
            e.preventDefault();
            if (soloLayerId !== null) {
                if (sortedLids.length === 0) return;
                let idx = sortedLids.indexOf(String(soloLayerId));
                if (idx === -1) {
                    idx = (e.key === 'ArrowDown') ? 0 : sortedLids.length - 1;
                } else {
                    if (e.key === 'ArrowDown') {
                        idx = (idx + 1) % sortedLids.length;
                    } else {
                        idx = (idx - 1 + sortedLids.length) % sortedLids.length;
                    }
                }
                soloLayerId = sortedLids[idx];
                updateLayerUI();
                requestAnimationFrame(render);
            } else {
                const select = document.getElementById('netSelect');
                if (select.options.length > 0) {
                    if (e.key === 'ArrowDown') {
                        select.selectedIndex = Math.min(select.selectedIndex + 1, select.options.length - 1);
                    } else {
                        select.selectedIndex = Math.max(select.selectedIndex - 1, 0);
                    }
                    highlightNet = parseFloat(select.value);
                    requestAnimationFrame(render);
                }
            }
        }
        if (e.key === 'Escape') {
            soloLayerId = null;
            updateLayerUI();
            requestAnimationFrame(render);
        }
    });

    // Sliders / Toggles
    document.getElementById('alphaSlider').oninput = () => requestAnimationFrame(render);
    document.getElementById('boundaryToggle').onchange = () => requestAnimationFrame(render);
    document.getElementById('powerNetToggle').onchange = () => requestAnimationFrame(render);

    document.getElementById('btnLogTab').onclick = () => {
        document.getElementById('logContainer').classList.toggle('visible');
    };

    document.getElementById('btnReset').onclick = () => {
        if (!isLoaded) return;
        view.pan = 0; view.tilt = 0; view.log2zoom = 0;
        view.centerX = (minX + maxX) / 2;
        view.centerY = (minY + maxY) / 2;
        updateBaseScale();
        requestAnimationFrame(render);
   };
}

function createShader(gl, type, source) {
    const shader = gl.createShader(type);
    gl.shaderSource(shader, source);
    gl.compileShader(shader);
    if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
        console.error(gl.getShaderInfoLog(shader));
        gl.deleteShader(shader);
        return null;
    }
    return shader;
}

function createProgram(gl, vs, fs) {
    const p = gl.createProgram();
    gl.attachShader(p, vs);
    gl.attachShader(p, fs);
    gl.linkProgram(p);
    if (!gl.getProgramParameter(p, gl.LINK_STATUS)) {
        console.error(gl.getProgramInfoLog(p));
        return null;
    }
    return p;
}


function render() {
    if (!gl) return;
    gl.clearColor(0.05, 0.05, 0.05, 1.0);
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);

    // Unified View Matrix Calculation
    const cx = view.centerX, cy = view.centerY;
    const cp = Math.cos(view.pan), sp = Math.sin(view.pan);
    const ct = Math.cos(view.tilt), st = Math.sin(view.tilt);
    const s = 2.0 * Math.pow(2.0, view.log2zoom) * view.baseScale;
    const pers = view.perspective;
    const asp = gl.canvas.width / Math.max(gl.canvas.height, 1);

    // M_view components (T -> R_pan -> S -> R_tilt)
    const m00 = s*cp, m01 = -s*sp, m02 = 0,    m03 = s*(-cx*cp + cy*sp);
    const m10 = ct*s*sp, m11 = ct*s*cp, m12 = -st*s, m13 = ct*s*(-cx*sp - cy*cp);
    const m20 = st*s*sp, m21 = st*s*cp, m22 = ct*s,  m23 = st*s*(-cx*sp - cy*cp);

    // Final matrix for WebGL (column-major)
    const viewMat = new Float32Array([
        m00/asp, m10, -0.05*m20, -pers*m20,
        m01/asp, m11, -0.05*m21, -pers*m21,
        m02/asp, m12, -0.05*m22, -pers*m22,
        m03/asp, m13, -0.05*m23, -pers*m23 + 1
    ]);

    const u_viewMat = gl.getUniformLocation(program, "u_viewMat");
    gl.uniformMatrix4fv(u_viewMat, false, viewMat);

    const u_isHighlighting = gl.getUniformLocation(program, "u_isHighlighting");
    gl.uniform1f(u_isHighlighting, (highlightedCount > 0) ? 1.0 : 0.0);

    const u_alpha = gl.getUniformLocation(program, "u_globalAlpha");
    const baseGlobalAlpha = parseFloat(document.getElementById('alphaSlider').value);
    
    // Net states texture
    const u_netStates = gl.getUniformLocation(program, "u_netStates");
    const u_netStatesSize = gl.getUniformLocation(program, "u_netStatesSize");
    gl.uniform2iv(u_netStatesSize, netStatesSize);
    
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, netStateTexture);
    gl.uniform1i(u_netStates, 0);
    const u_showBoundaries = gl.getUniformLocation(program, "u_showBoundaries");
    gl.uniform1f(u_showBoundaries, document.getElementById('boundaryToggle').checked ? 1.0 : 0.0);

    const u_showPowerNets = gl.getUniformLocation(program, "u_showPowerNets");
    gl.uniform1f(u_showPowerNets, document.getElementById('powerNetToggle').checked ? 1.0 : 0.0);

    const u_isExemptLayer = gl.getUniformLocation(program, "u_isExemptLayer");

    const u_layerZ = gl.getUniformLocation(program, "u_layerZ");
    const u_thickness = gl.getUniformLocation(program, "u_thickness");

    const a_rect = gl.getAttribLocation(program, "a_rect");
    const a_net = gl.getAttribLocation(program, "a_net");

    gl.enable(gl.DEPTH_TEST);
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);

    const reverseOrder = Math.cos(view.tilt) < 0;
    const sortedLidsForRender = Object.keys(layers).sort((a, b) => {
        const zA = layerSpecs[a]?.z || 0;
        const zB = layerSpecs[b]?.z || 0;
        return reverseOrder ? (zB - zA) : (zA - zB);
    });

    for (const lid of sortedLidsForRender) {
        const layer = layers[lid];
        if (layer.count === 0) continue;
        
        // Solo logic
        if (soloLayerId !== null) {
            const isActive = (lid >= 3 && lid <= 5);
            const isSoloActive = (soloLayerId >= 3 && soloLayerId <= 5);
            if (isActive && isSoloActive) { /* match */ }
            else if (String(lid) !== String(soloLayerId)) continue;
        } else {
            if (!layer.visible) continue;
        }

        gl.uniform1f(u_alpha, baseGlobalAlpha);

        const config = LAYER_CONFIG[lid] || { name: "", color: [0.5, 0.5, 0.5, 1.0] };
        const name = config.name;
        const isExempt = name.toUpperCase().endsWith("TERM") || name.toUpperCase() === "LICON";
        gl.uniform1f(u_isExemptLayer, isExempt ? 1.0 : 0.0);
        
        // Use pre-calculated Z and thickness
        const spec = layerSpecs[lid] || { z: 0, h: 0 };
        gl.uniform1f(u_layerZ, spec.z);
        gl.uniform1f(u_thickness, spec.h);

        const color = config.color;
        const u_color = gl.getUniformLocation(program, "u_color");
        gl.uniform4fv(u_color, color);

        gl.bindBuffer(gl.ARRAY_BUFFER, layer.buffer);
        
        // Stride is 5 ints: x, y, w, h, net
        const stride = 5 * 4;
        
        gl.enableVertexAttribArray(a_rect);
        gl.vertexAttribIPointer(a_rect, 4, gl.INT, stride, 0);
        gl.vertexAttribDivisor(a_rect, 1);
        
        gl.enableVertexAttribArray(a_net);
        gl.vertexAttribIPointer(a_net, 1, gl.INT, stride, 4 * 4);
        gl.vertexAttribDivisor(a_net, 1);

        gl.drawElementsInstanced(gl.TRIANGLES, 36, gl.UNSIGNED_SHORT, 0, layer.count);
    }
}

function log(msg) {
    const logContainer = document.getElementById('logContainer');
    logContainer.textContent += msg;
    if (logContainer.classList.contains('visible')) {
        logContainer.scrollTop = logContainer.scrollHeight;
    }
}

async function loadGDS(url, pdk) {
    const logContainer = document.getElementById('logContainer');
    logContainer.textContent = '';
    logContainer.classList.add('visible');
    log(`Loading GDS: ${url} (PDK: ${pdk || 'default'})\n`);

    document.getElementById('loading').innerText = 'Parsing GDS in worker...';
    document.getElementById('loading').style.display = 'block';

    // OASIS support is now enabled via GDSTK integration

    if (worker) worker.terminate();
    worker = new Worker('web/gds_worker.js', { type: 'module' });

    worker.onmessage = (e) => {
        const data = e.data;
        if (data.type === 'log') {
            log(data.message);
        } else if (data.type === 'error') {
            log('ERROR: ' + data.message);
            alert('Error parsing GDS: ' + data.message);
        } else if (data.type === 'done') {
            const stats = data.stats;
            log('='.repeat(30) + '\n');
            log('Parse Complete!\n');
            log(`Total Bytes Src: ${stats.totalBytes.toLocaleString()}\n`);
            log(`Total Time: ${stats.totalTime.toFixed(2)} ms\n`);
            log(`WASM Memory: ${stats.memMB} MB\n`);
            log('='.repeat(30) + '\n');
            processParsedData(stats, data.wireNames);
            setTimeout(() => {
                document.getElementById('logContainer').classList.remove('visible');
            }, 2000); // Give a bit more time to read the stats
        } else if (data.type === 'callResult') {
            if (data.wireData) {
                updateNetStatesFromWireData(data.wireData);
                updateCircuitUI(data.wireData);
            }
        }
    };

    const normalizedUrl = new URL(url, window.location.href).href;
    worker.postMessage({ type: 'start', gdsUrl: normalizedUrl, pdk: pdk });
}

function processParsedData(stats, wireNamesIn) {
    wireNames = wireNamesIn || [];
    const { rectData, layerOffsets } = stats;
    
    if (!rectData || !layerOffsets) {
        log("ERROR: No geometry data received from worker. GDS might be empty or malformed.\n");
        document.getElementById('loading').innerText = 'Error: No geometry data.';
        return;
    }

    const netAreas = {};
    
    minX = Infinity, minY = Infinity, maxX = -Infinity, maxY = -Infinity;

    // Process Layers
    layers = {}; // Reset
    document.getElementById('layerList').innerHTML = ''; // Clear UI
    
    const L_COUNT = layerOffsets.length - 1;
    sortedLids = [];
    
    let maxNet = -1;
    for (let lid = 0; lid < L_COUNT; lid++) {
        const start = layerOffsets[lid] * 5;
        const end = layerOffsets[lid+1] * 5;
        if (start === end) continue;
        
        const data = rectData.subarray(start, end);
        const buffer = gl.createBuffer();
        gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
        gl.bufferData(gl.ARRAY_BUFFER, data, gl.STATIC_DRAW);
        
        layers[lid] = {
            count: data.length / 5,
            buffer: buffer,
            visible: true
        };
        sortedLids.push(String(lid));
        
        // Bounds and Net area
        for (let i = 0; i < data.length; i += 5) {
            const x1 = data[i], y1 = data[i+1], x2 = data[i+2], y2 = data[i+3], net = data[i+4];
            if (x1 < minX) minX = x1;
            if (y1 < minY) minY = y1;
            if (x2 > maxX) maxX = x2;
            if (y2 > maxY) maxY = y2;
            
            if (net !== -1) {
                if (net > maxNet) maxNet = net;
                const area = Math.abs((x2 - x1) * (y2 - y1));
                netAreas[net] = (netAreas[net] || 0) + area;
            }
        }
    }

    if (maxNet >= 0) {
        const texWidth = 2048;
        const texHeight = Math.ceil((maxNet + 1) / texWidth);
        netStatesSize = [texWidth, texHeight];
        netStateData = new Uint8Array(texWidth * texHeight); // all 0 (off)
        
        if (netStateTexture) gl.deleteTexture(netStateTexture);
        netStateTexture = gl.createTexture();
        gl.bindTexture(gl.TEXTURE_2D, netStateTexture);
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.R8UI, texWidth, texHeight, 0, gl.RED_INTEGER, gl.UNSIGNED_BYTE, netStateData);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    } else {
        netStatesSize = [0, 0];
        netStateData = null;
        if (netStateTexture) gl.deleteTexture(netStateTexture);
        netStateTexture = null;
    }
    

    // Top 1000 Nets by Area
    let sortedNets = Object.entries(netAreas)
        .sort((a, b) => b[1] - a[1]);
    
    const select = document.getElementById('netSelect');
    const search = document.getElementById('netSearch');
    
    const netToName = {};
    wireNames.forEach(w => netToName[w.id] = w.name);

    function updateNetList() {
        const query = search.value.toLowerCase();
        select.innerHTML = '';
        let count = 0;
        for (const [netStr, area] of sortedNets) {
            const net = parseInt(netStr);
            const name = netToName[net];
            const label = name ? `${name} (Net ${net})` : `Net ${net} (Area: ${area.toLocaleString()})`;
            
            if (label.toLowerCase().includes(query)) {
                const opt = document.createElement('option');
                opt.value = net;
                opt.innerText = label;
                if (netStateData && net >= 0 && net < netStateData.length && netStateData[net] > 0) {
                    opt.selected = true;
                }
                select.appendChild(opt);
                count++;
                if (count >= 1000) break; // Limit to 1000 visible
            }
        }
    }

    search.oninput = updateNetList;
    search.onkeydown = (e) => {
        if (e.key === 'ArrowDown' && select.options.length > 0) {
            e.preventDefault();
            select.focus();
            if (select.selectedIndex === -1) {
                select.options[0].selected = true;
                syncNetSelection();
            }
        }
    };
    updateNetList();

    document.getElementById('btnClearNet').onclick = () => {
        select.selectedIndex = -1;
        search.value = '';
        updateNetList();
        highlightNet = -1;
        highlightedCount = 0;
        if (netStateData) {
            netStateData.fill(0);
            gl.bindTexture(gl.TEXTURE_2D, netStateTexture);
            gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, netStatesSize[0], netStatesSize[1], gl.RED_INTEGER, gl.UNSIGNED_BYTE, netStateData);
        }
        requestAnimationFrame(render);
    };

    function syncNetSelection() {
        if (!netStateData) return;
        
        // Update highlightNet for the "last clicked" or primary selection
        highlightNet = select.value === "" ? -1 : parseFloat(select.value);
        
        // Sync netStateData with only the visible (filtered) options in the select element.
        // This preserves the state of nets that are currently filtered out.
        for (let i = 0; i < select.options.length; i++) {
            const opt = select.options[i];
            const net = parseInt(opt.value);
            if (net >= 0 && net < netStateData.length) {
                const wasSelected = netStateData[net] > 0;
                const isSelected = opt.selected;
                if (wasSelected !== isSelected) {
                    netStateData[net] = isSelected ? 1 : 0; // Use bit 0 (value 1) for highlight
                    highlightedCount += isSelected ? 1 : -1;
                }
            }
        }
        
        gl.bindTexture(gl.TEXTURE_2D, netStateTexture);
        gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, netStatesSize[0], netStatesSize[1], gl.RED_INTEGER, gl.UNSIGNED_BYTE, netStateData);
        
        requestAnimationFrame(render);
    }

    select.onchange = syncNetSelection;
    select.oninput = syncNetSelection;
    select.onkeyup = syncNetSelection;
    select.onclick = syncNetSelection;

    document.getElementById('btnToggleAll').onclick = () => {
        const anyVisible = Object.values(layers).some(l => l.visible);
        for (const lid in layers) {
            layers[lid].visible = !anyVisible;
            layers[lid].input.checked = !anyVisible;
        }
        requestAnimationFrame(render);
    };

    view.centerX = (minX + maxX) / 2;
    view.centerY = (minY + maxY) / 2;
    isLoaded = true;
    updateLayerHeights();
    updateBaseScale();

    // Now that heights are known, add layer controls in lid-descending order
    sortedLids.sort((a, b) => Number(b) - Number(a));
    sortedLids.forEach(lid => {
        if (lid == 3 || lid == 4) return; // Handled by FET group (LID 5)
        const ui = addLayerControl(lid);
        layers[lid].input = ui.input;
        layers[lid].div = ui.div;
        if (lid == 5) {
            layers[3].input = layers[4].input = ui.input;
            layers[3].div = layers[4].div = ui.div;
        }
    });

    document.getElementById('loading').style.display = 'none';
    updateLayerUI();
    updateCircuitUI();
    requestAnimationFrame(render);
}

function updateCircuitUI(wireData) {
    const monitor = document.getElementById('circuitMonitor');
    if (!monitor) return;
    if (wireNames.length === 0) {
        monitor.innerHTML = '<div style="padding:10px; color:#666;">No circuit data</div>';
        return;
    }

    let html = '<div class="monitor-header">Circuit Monitoring</div>';
    html += '<div class="monitor-controls"><button onclick="stepCircuit()">Step Wave</button></div>';
    html += '<div class="monitor-list">';
    
    // Show top-level nets (labeled ones)
    wireNames.forEach(({id, name}) => {
        const state = (wireData) ? wireData[id] : (netStateData ? netStateData[id] : 0);
        const stateClass = state === 1 ? 'state-high' : 'state-low';
        const label = state === 1 ? 'H' : 'L';
        html += `
            <div class="monitor-row">
                <span class="wire-name">${name}</span>
                <span class="wire-state ${stateClass}" onclick="toggleWire(${id}, ${state})">${label}</span>
            </div>
        `;
    });
    html += '</div>';
    monitor.innerHTML = html;
}

window.toggleWire = (id, currentState) => {
    const newVal = currentState === 1 ? 0 : 1;
    callWasm('wasm_circuit_set_input', [id, newVal], ['wireData']);
};

window.stepCircuit = () => {
    callWasm('wasm_circuit_run_wave', [], ['wireData']);
};

function callWasm(name, args, returnArrays) {
    if (!worker) return;
    worker.postMessage({ type: 'call', name, args, returnArrays });
}

function updateNetStatesFromWireData(wireData) {
    if (!netStateData) return;
    // Update local netStateData for visualization
    for (let i = 0; i < wireData.length; i++) {
        if (i < netStateData.length) {
            netStateData[i] = wireData[i];
        }
    }
    // Update WebGL texture
    gl.bindTexture(gl.TEXTURE_2D, netStateTexture);
    gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, netStatesSize[0], netStatesSize[1], gl.RED_INTEGER, gl.UNSIGNED_BYTE, netStateData);
    requestAnimationFrame(render);
}

function addLayerControl(lid) {
    const div = document.createElement('div');
    div.className = 'layer-toggle';
    const config = LAYER_CONFIG[lid] || { name: "Layer " + lid, color: [0.5, 0.5, 0.5] };

    const input = document.createElement('input');
    input.type = 'checkbox';
    input.checked = true;
    input.onchange = (e) => {
        layers[lid].visible = e.target.checked;
        if (lid == 5) {
            layers[3].visible = e.target.checked;
            layers[4].visible = e.target.checked;
        }
        requestAnimationFrame(render);
    };
    div.appendChild(input);

    const makeSwatch = (id) => {
        const config = LAYER_CONFIG[id];
        if (!config) return document.createElement('span');
        const c = config.color;
        const s = document.createElement('span');
        s.className = 'layer-color';
        s.style.backgroundColor = `rgba(${c[0]*255}, ${c[1]*255}, ${c[2]*255}, 1)`;
        s.style.cursor = 'pointer';
        s.title = config.name;
        s.onclick = () => {
            soloLayerId = (soloLayerId === id) ? null : id;
            updateLayerUI();
            requestAnimationFrame(render);
        };
        return s;
    };

    const makeLabel = (id, text) => {
        const l = document.createElement('span');
        l.innerText = text;
        l.style.cursor = 'pointer';
        l.onclick = () => {
            soloLayerId = (soloLayerId === id) ? null : id;
            updateLayerUI();
            requestAnimationFrame(render);
        };
        return l;
    };

    if (lid == 5) {
        div.appendChild(makeSwatch(4)); // N_TERM
        div.appendChild(makeSwatch(3)); // CHANNEL
        div.appendChild(makeSwatch(5)); // P_TERM
        div.appendChild(makeLabel(5, " FET"));
    } else {
        div.appendChild(makeSwatch(lid));
        div.appendChild(makeLabel(lid, config.name));
    }
    
    document.getElementById('layerList').appendChild(div);
    return { input, div };
}

function updateLayerUI() {
    for (const lid in layers) {
        const layer = layers[lid];
        if (!layer.div) continue;
        if (soloLayerId !== null) {
            const isActive = (lid >= 3 && lid <= 5);
            const isSoloActive = (soloLayerId >= 3 && soloLayerId <= 5);
            if ((isActive && isSoloActive) || (String(lid) === String(soloLayerId))) {
                layer.div.style.opacity = "1.0";
            } else {
                layer.div.style.opacity = "0.3";
            }
        } else {
            layer.div.style.opacity = "1.0";
        }
    }
}

// Start
initWebGL();
const params = new URLSearchParams(window.location.search);
const file = params.get('file');
const pdk = params.get('pdk');
if (file) {
    loadGDS(file, pdk);
} else {
    document.getElementById('loading').innerText = 'No file specified. Use ?file=...';
}
