const VS_SOURCE = `#version 300 es
    in vec3 a_pos;       // Box vertex position (0..1)
    in vec4 a_rect;      // Rect: x1, y1, x2, y2
    in float a_net;      // Net ID
    
    uniform mat4 u_viewMat;
    uniform float u_layerZ;
    uniform float u_thickness;
    uniform float u_showPowerNets;
    uniform float u_isExemptLayer;
    
    out float v_net;
    out vec2 v_pos;
    out float v_light;

    void main() {
        if (u_showPowerNets < 0.5 && u_isExemptLayer < 0.5) {
            if (a_net > -0.5 && a_net < 1.5) {
                // Collapse the geometry to avoid rasterization
                gl_Position = vec4(0.0, 0.0, 0.0, 0.0);
                return;
            }
        }

        vec2 rectPos = mix(a_rect.xy, a_rect.zw, a_pos.xy);
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
    uniform float u_highlightNet; // -1 if none
    uniform float u_globalAlpha;
    uniform float u_showBoundaries;
    
    in float v_net;
    in vec2 v_pos;
    in float v_light;
    
    out vec4 fragColor;

    void main() {
        vec4 color = u_color;
        color.rgb *= v_light;
        
        if (u_highlightNet >= 0.0) {
            if (abs(v_net - u_highlightNet) > 0.1) {
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
const LAYER_COLORS = [
    [1.0, 0.5, 0.0, 1.0], // 0: ERRORS (Orange)
    [0.3, 0.1, 0.2, 1.0], // 1: NWELL (pale yellow)
    [0.2, 0.8, 0.2, 1.0], // 2: DIFF (skip/invisible usually)
    [0.0, 0.0, 0.0, 0.0], // 3: CHANNEL (skip/invisible usually)
    [0.2, 0.6, 0.2, 1.0], // 4: N_TERM (lime green)
    [0.8, 0.8, 0.2, 1.0], // 5: P_TERM (yellow-ish)
    [0.8, 0.2, 0.2, 1.0], // 6: POLY (red)
    [0.5, 0.5, 0.5, 1.0], // 7: LICON (grey)
    [0.3, 0.3, 0.9, 1.0], // 8: LI1 (blue)
    [0.6, 0.6, 0.6, 1.0], // 9: MCON
    [0.7, 0.4, 0.8, 1.0], // 10: MET1 (purple)
    [0.8, 0.8, 0.8, 1.0], // 11: VIA1
    [0.4, 0.8, 0.8, 1.0], // 12: MET2 (cyan)
    [0.9, 0.9, 0.9, 1.0], // 13: VIA2
    [0.8, 0.8, 0.2, 1.0], // 14: MET3
    [0.9, 0.9, 0.9, 1.0], // 15: VIA3
    [0.2, 0.6, 0.2, 1.0], // 16: MET4
    [0.9, 0.9, 0.9, 1.0], // 17: VIA4
    [0.6, 0.2, 0.6, 1.0], // 18: MET5
];

const LAYER_NAMES = [
    "ERRORS", "NWELL", "DIFF", "CHANNEL", "N_TERM", "P_TERM", "POLY", "LICON", 
    "LI1", "MCON", "MET1", "VIA1", "MET2", "VIA2", "MET3", "VIA3", "MET4", "VIA4", "MET5"
];


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
    const width = maxX - minX;
    const baseH = 500.0;//width * 0.001;
    let currentZ = 0;
    
    // Sort lids ascending to calculate stacking
    const lids = Object.keys(layers).map(Number).sort((a,b)=>a-b);
    
    let termZ = -1, termTopZ = -1;
    lids.forEach(lid => {
        let h = baseH; // default
        const name = (LAYER_NAMES[lid] || "").toUpperCase();
        if (lid === 0) { // ERRORS
            h = baseH * 2.0; // Make errors thick
        } else if (name.includes("VIA") || name.includes("CON") || name === "LICON" || name === "MCON") {
            h = baseH * 1.0; // VIAs are taller
        } else if (name.includes("MET") || name.includes("LI")) {
            h = baseH * 0.4; // Metals are flatter
        } else {
            h = baseH * 0.2; // Base layers are very flat
        }
        
        if (lid === 0) {
            // ERRORS/Markers: put them slightly above the highest layer at the end
            // We'll calculate their Z after the loop to be sure
            layerSpecs[lid] = { z: -1, h: h }; 
        } else if (name.includes("TERM")) {
            if (termZ === -1) {
                termZ = currentZ;
                termTopZ = currentZ + h;
                currentZ += h;
            }
            layerSpecs[lid] = { z: termZ, h: h };
        } else if (name === "LICON" && termTopZ !== -1) {
            layerSpecs[lid] = { z: termTopZ, h: (currentZ + h) - termTopZ };
            currentZ += h;
        } else {
            layerSpecs[lid] = { z: currentZ, h: h };
            currentZ += h;
        }
    });
    
    if (layerSpecs[0]) {
        layerSpecs[0].z = currentZ + baseH;
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
let soloLayerId = null;

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

    const u_highlight = gl.getUniformLocation(program, "u_highlightNet");
    gl.uniform1f(u_highlight, highlightNet);

    const u_alpha = gl.getUniformLocation(program, "u_globalAlpha");
    const baseGlobalAlpha = parseFloat(document.getElementById('alphaSlider').value);
    
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
            if (String(lid) !== String(soloLayerId)) continue;
        } else {
            if (!layer.visible) continue;
        }

        gl.uniform1f(u_alpha, baseGlobalAlpha);

        const name = LAYER_NAMES[lid] || "";
        const isExempt = name.toUpperCase().endsWith("TERM") || name.toUpperCase() === "LICON";
        gl.uniform1f(u_isExemptLayer, isExempt ? 1.0 : 0.0);
        
        // Use pre-calculated Z and thickness
        const spec = layerSpecs[lid] || { z: 0, h: 0 };
        gl.uniform1f(u_layerZ, spec.z);
        gl.uniform1f(u_thickness, spec.h);

        const color = LAYER_COLORS[lid] || [0.5, 0.5, 0.5, 1.0];
        const u_color = gl.getUniformLocation(program, "u_color");
        gl.uniform4fv(u_color, color);

        gl.bindBuffer(gl.ARRAY_BUFFER, layer.buffer);
        
        // Stride is 5 floats: x, y, w, h, net
        const stride = 5 * 4;
        
        gl.enableVertexAttribArray(a_rect);
        gl.vertexAttribPointer(a_rect, 4, gl.FLOAT, false, stride, 0);
        gl.vertexAttribDivisor(a_rect, 1);
        
        gl.enableVertexAttribArray(a_net);
        gl.vertexAttribPointer(a_net, 1, gl.FLOAT, false, stride, 4 * 4);
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
            processParsedData(stats);
            setTimeout(() => {
                document.getElementById('logContainer').classList.remove('visible');
            }, 2000); // Give a bit more time to read the stats
        }
    };

    const normalizedUrl = new URL(url, window.location.href).href;
    worker.postMessage({ type: 'start', gdsUrl: normalizedUrl, pdk: pdk });
}

let worker;
function processParsedData(stats) {
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
    
    for (let lid = 0; lid < L_COUNT; lid++) {
        const start = layerOffsets[lid] * 5;
        const end = layerOffsets[lid+1] * 5;
        if (start === end) continue;
        
        const data = new Float32Array(rectData.subarray(start, end));
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
                const area = Math.abs((x2 - x1) * (y2 - y1));
                netAreas[net] = (netAreas[net] || 0) + area;
            }
        }
    }
    

    // Top 1000 Nets by Area
    let sortedNets = Object.entries(netAreas)
        .sort((a, b) => b[1] - a[1]);
    
    const select = document.getElementById('netSelect');
    const search = document.getElementById('netSearch');

    function updateNetList() {
        const query = search.value.toLowerCase();
        select.innerHTML = '';
        let count = 0;
        for (const [net, area] of sortedNets) {
            const label = `Net ${net} (Area: ${area.toLocaleString()})`;
            if (label.toLowerCase().includes(query)) {
                const opt = document.createElement('option');
                opt.value = net;
                opt.innerText = label;
                select.appendChild(opt);
                count++;
                if (count >= 1000) break; // Limit to 1000 visible
            }
        }
    }

    search.oninput = updateNetList;
    updateNetList();

    document.getElementById('btnClearNet').onclick = () => {
        select.value = -1;
        search.value = '';
        updateNetList();
        highlightNet = -1;
        requestAnimationFrame(render);
    };

    select.onchange = (e) => {
        highlightNet = parseFloat(e.target.value);
        requestAnimationFrame(render);
    };

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
        const ui = addLayerControl(lid);
        layers[lid].input = ui.input;
        layers[lid].div = ui.div;
    });

    document.getElementById('loading').style.display = 'none';
    updateLayerUI();
    requestAnimationFrame(render);
}

function addLayerControl(lid) {
    const div = document.createElement('div');
    div.className = 'layer-toggle';
    
    const input = document.createElement('input');
    input.type = 'checkbox';
    input.checked = true;
    input.onchange = (e) => {
        layers[lid].visible = e.target.checked;
        requestAnimationFrame(render);
    };
    
    const color = document.createElement('span');
    color.className = 'layer-color';
    const c = LAYER_COLORS[lid] || [0.5, 0.5, 0.5];
    color.style.backgroundColor = `rgba(${c[0]*255}, ${c[1]*255}, ${c[2]*255}, 1)`;
    
    const label = document.createElement('span');
    const name = LAYER_NAMES[lid] || ("Layer " + lid);
    label.innerText = name;
    label.style.cursor = 'pointer';
    label.onclick = () => {
        if (soloLayerId === lid) {
            soloLayerId = null;
        } else {
            soloLayerId = lid;
        }
        updateLayerUI();
        requestAnimationFrame(render);
    };
    
    div.appendChild(input);
    div.appendChild(color);
    div.appendChild(label);
    
    document.getElementById('layerList').appendChild(div);
    return { input, div };
}

function updateLayerUI() {
    for (const lid in layers) {
        const layer = layers[lid];
        if (soloLayerId !== null) {
            if (String(lid) === String(soloLayerId)) {
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
