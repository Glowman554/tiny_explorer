import { Animator } from './animator.js';

/**
 * CircuitViewer - A WebGL-based GDS/OASIS geometry and netlist viewer.
 */

// MARK: - WebGL Shaders
export const VS_SOURCE = `#version 300 es
    in vec3 a_pos;       // Box vertex position (0..1)
    in ivec4 a_rect;     // Rect: x1, y1, x2, y2
    in int a_net;        // Net ID
    in int a_tree;       // Tree ID
    
    uniform mat4 u_viewMat;
    uniform float u_layerZ;
    uniform float u_thickness;
    uniform float u_showPowerNets;
    uniform float u_isExemptLayer;
    
    uniform vec4 u_color;
    uniform float u_isHighlighting; 
    uniform float u_stateMix;       
    uniform lowp usampler2D u_netStates;
    uniform ivec2 u_netStatesSize;
    
    flat out vec4 v_color;
    out vec3 v_pos;
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
        // z-fighting mitigation
        float z = u_layerZ + a_pos.z * u_thickness;
        z += float(gl_InstanceID % 64) * 0.1;
        vec4 p = vec4(rectPos, z, 1.0);
        gl_Position = u_viewMat * p;
        
        v_pos = a_pos;
        v_light = 0.7 + 0.3 * a_pos.z; // Simple top lighting

        // Color computation
        vec3 layerColor = u_color.rgb;
        vec3 stateColor = vec3(0.5, 0.5, 0.5);
        bool isHighlighted = false;
        
        if (a_net >= 0 && u_netStatesSize.x > 0) {
            ivec2 texCoords = ivec2(a_net % u_netStatesSize.x, a_net / u_netStatesSize.x);
            uint state = texelFetch(u_netStates, texCoords, 0).r;
            
            // Bit 0 is Logic Value (1=High, 0=Low)
            if ((state & 1u) != 0u) {
                stateColor = vec3(1.);
            } else {
                stateColor = vec3(0.0);
            }
            
            // Bit 6 is Flipped/Changed flag
            bool isFlipped = (state & 0x40u) != 0u;
            if (isFlipped) {
                stateColor = mix(stateColor, 
                (state & 1u) != 0u ? vec3(1.0, 1.0, 0.0) : vec3(0.0, 1.0, 1.0), 
                0.7); 
            } else {
                stateColor *= 0.5;
            }
            
            // Bit 7 is Selection/Highlight
            if ((state & 0x80u) != 0u) isHighlighted = true;
        }

        vec3 baseRGB = mix(layerColor, stateColor, u_stateMix);
        vec4 computedColor = vec4(baseRGB, u_color.a);
        
        if (u_isHighlighting > 0.5) {
            if (!isHighlighted) {
                // Dim non-highlighted nets
                computedColor.a *= 0.1;
                computedColor.rgb *= 0.5;
            } else {
                // Highlight
                if (a_net == a_tree && a_tree > 1) {
                    // Root wire is red
                    computedColor.rgb = mix(computedColor.rgb, vec3(1.0, 0.0, 0.0), 0.8);
                } else {
                    // Other nets are white-ish
                    computedColor.rgb = mix(computedColor.rgb, vec3(1.0, 1.0, 1.0), 0.3);
                }
                computedColor.a = 1.0;
            }
        }
        
        v_color = computedColor;
    }
`;

export const FS_SOURCE = `#version 300 es
    precision mediump float;
    
    uniform float u_globalAlpha;
    uniform float u_layerAlpha;
    uniform float u_showBoundaries;
    
    flat in vec4 v_color;
    in vec3 v_pos;
    in float v_light;
    
    out vec4 fragColor;

    void main() {
        vec4 color = v_color;
        color.rgb *= v_light;
        
        if (u_showBoundaries > 0.5) {
            vec3 d = min(fwidth(v_pos), 0.1);
            vec3 f = step(d, min(v_pos, 1.0-v_pos));
            color = mix(vec4(1), color, f.x*f.y*f.z);
        }
        
        fragColor = color;
        fragColor.a *= u_globalAlpha * u_layerAlpha;
    }
`;

// MARK: - Core Configuration
const h_met=200, h_via=500;
let _z = 0, _h = 0;
export const LAYER_CONFIG = [
    { name: "ERRORS",  color: [1.0, 0.5, 0.0, 1.0], z: 5000, h: 500 },
    { name: "NWELL",   color: [0.3, 0.1, 0.2, 1.0], z: _z,    h: _h=100 },
    { name: "DIFF",    color: [0.2, 0.8, 0.2, 1.0], z: _z+=_h, h: _h },
    { name: "CHANNEL", color: [0.8, 0.6, 0.6, 1.0], z: _z,    h: _h },
    { name: "N_TERM",  color: [0.2, 0.6, 0.2, 1.0], z: _z,    h: _h },
    { name: "P_TERM",  color: [0.8, 0.8, 0.2, 1.0], z: _z,    h: _h },
    { name: "POLY",    color: [0.8, 0.2, 0.2, 1.0], z: _z+=_h, h: _h },
    { name: "LICON",   color: [0.5, 0.5, 0.5, 1.0], z: _z+5,   h: _h=h_via+h_met-5 },
    { name: "LI1",     color: [0.3, 0.3, 0.9, 1.0], z: _z+=_h, h: _h=h_met },
    { name: "MCON",    color: [0.6, 0.6, 0.6, 1.0], z: _z+=_h, h: _h=h_via },
    { name: "MET1",    color: [0.7, 0.4, 0.8, 1.0], z: _z+=_h, h: _h=h_met },
    { name: "VIA1",    color: [0.8, 0.8, 0.8, 1.0], z: _z+=_h, h: _h=h_via },
    { name: "MET2",    color: [0.4, 0.8, 0.8, 1.0], z: _z+=_h, h: _h=h_met },
    { name: "VIA2",    color: [0.9, 0.9, 0.9, 1.0], z: _z+=_h, h: _h=h_via },
    { name: "MET3",    color: [0.8, 0.8, 0.2, 1.0], z: _z+=_h, h: _h=h_met },
    { name: "VIA3",    color: [0.9, 0.9, 0.9, 1.0], z: _z+=_h, h: _h=h_via },
    { name: "MET4",    color: [0.2, 0.6, 0.2, 1.0], z: _z+=_h, h: _h=h_met },
    { name: "VIA4",    color: [0.9, 0.9, 0.9, 1.0], z: _z+=_h, h: _h=h_via },
    { name: "MET5",    color: [0.6, 0.2, 0.6, 1.0], z: _z+=_h, h: _h=h_met },
].map(l => ({
    ...l, 
    isExempt: l.name.toUpperCase().endsWith("TERM") || l.name.toUpperCase() === "LICON"
}));

// MARK: - CircuitViewer Main Class
export class CircuitViewer {
    constructor(canvasId) {
        this.canvas = document.getElementById(canvasId);
        this.gl = null;
        this.program = null;
        
        // State
        this.layers = {};
        this.sortedLids = [];
        this.layerSpecs = {};
        this.view = { 
            centerX: 0, centerY: 0, 
            log2zoom: 0, pan: 0, tilt: 0, perspective: 0.5,
            baseScale: 0.01,
            rotateMode: false,
            globalAlpha: 0.8,
            showBoundaries: false,
            showPowerNets: true,
            stateMix: 0.0,
            explode: 1.0,
            renderMode: 'cube'
        };
        
        this.isDragging = false;
        this.lastMouse = { x: 0, y: 0 };
        this.bounds = { minX: 0, minY: 0, maxX: 0, maxY: 0 };
        this.isLoaded = false;
        
        this.worker = null;
        this.wireNames = [];
        this.netAreas = {};
        
        this.highlightNet = -1;
        this.highlightedCount = 0;
        this.soloLayerId = null;
        
        this.netStateTexture = null;
        this.netStatesSize = [0, 0];
        this.netStateData = null;
        this.lastWireData = null;

        // Callbacks for UI sync
        this.onLog = null;
        this.onProgress = null;
        this.onLoaded = null;
        this.onUpdateCircuit = null;
        this.onVgaFrame = null;

        this.vga = { width: 0, height: 0, buffer: null };
        this.simSpeed = 0;
        this.autoClock = false;
        this.simInputPauseActive = false;
        this.pendingInputs = [];
        this.workerAckResolve = null;

        this.init();
    }

    // MARK: Initialization
    init() {
        this.gl = this.canvas.getContext('webgl2', { alpha: true, antialias: true });
        this.program = null;
        this.animator = new Animator();
        if (!this.gl) { alert('WebGL2 not supported'); return; }
        
        const gl = this.gl;

        // Shader Setup
        const vs = this.createShader(gl.VERTEX_SHADER, VS_SOURCE);
        const fs = this.createShader(gl.FRAGMENT_SHADER, FS_SOURCE);
        this.program = this.createProgram(vs, fs);
        gl.useProgram(this.program);

        // Cache uniform locations
        this.unis = {
            viewMat: gl.getUniformLocation(this.program, "u_viewMat"),
            isHighlighting: gl.getUniformLocation(this.program, "u_isHighlighting"),
            globalAlpha: gl.getUniformLocation(this.program, "u_globalAlpha"),
            stateMix: gl.getUniformLocation(this.program, "u_stateMix"),
            netStatesSize: gl.getUniformLocation(this.program, "u_netStatesSize"),
            netStates: gl.getUniformLocation(this.program, "u_netStates"),
            showBoundaries: gl.getUniformLocation(this.program, "u_showBoundaries"),
            showPowerNets: gl.getUniformLocation(this.program, "u_showPowerNets"),
            isExemptLayer: gl.getUniformLocation(this.program, "u_isExemptLayer"),
            layerZ: gl.getUniformLocation(this.program, "u_layerZ"),
            thickness: gl.getUniformLocation(this.program, "u_thickness"),
            color: gl.getUniformLocation(this.program, "u_color"),
            layerAlpha: gl.getUniformLocation(this.program, "u_layerAlpha")
        };
        this.attribs = {
            pos: gl.getAttribLocation(this.program, "a_pos"),
            rect: gl.getAttribLocation(this.program, "a_rect"),
            net: gl.getAttribLocation(this.program, "a_net"),
            tree: gl.getAttribLocation(this.program, "a_tree")
        };

        // Cube Geometry (Shared, Indexed)
        const cubeVerts = new Float32Array([
            0,0,0, 1,0,0, 1,1,0, 0,1,0,
            0,0,1, 1,0,1, 1,1,1, 0,1,1
        ]);
        const cubeIndices = new Uint16Array([
            4, 5, 6, 4, 6, 7, // Top (First 6 indices = Quad)
            0, 2, 1, 0, 3, 2, // Bottom
            0, 1, 5, 0, 5, 4, // Front
            2, 3, 7, 2, 7, 6, // Back
            1, 2, 6, 1, 6, 5, // Right
            3, 0, 4, 3, 4, 7  // Left
        ]);

        const vbo = gl.createBuffer();
        gl.bindBuffer(gl.ARRAY_BUFFER, vbo);
        gl.bufferData(gl.ARRAY_BUFFER, cubeVerts, gl.STATIC_DRAW);
        
        this.ebo = gl.createBuffer();
        gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, this.ebo);
        gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, cubeIndices, gl.STATIC_DRAW);
        
        const a_pos = this.attribs.pos;
        gl.enableVertexAttribArray(a_pos);
        gl.vertexAttribPointer(a_pos, 3, gl.FLOAT, false, 0, 0);

        gl.enable(gl.CULL_FACE);
        gl.cullFace(gl.BACK);

        this.setupEventHandlers();
        this.resize();
    }

    setupEventHandlers() {
        window.addEventListener('resize', () => this.resize());
        
        this.canvas.addEventListener('mousedown', e => { 
            if (e.button !== 0) return;
            this.isDragging = true; 
            this.lastMouse = { x: e.clientX, y: e.clientY }; 
        });

        window.addEventListener('mouseup', () => this.isDragging = false);

        this.canvas.addEventListener('mousemove', e => {
            if (!this.isDragging) return;
            const dx = (e.clientX - this.lastMouse.x) * 0.001;
            const dy = (e.clientY - this.lastMouse.y) * 0.001;
            const isRotate = e.shiftKey || this.view.rotateMode;
            this._handleMove(-dx, -dy, isRotate);
            this.lastMouse = { x: e.clientX, y: e.clientY };
            this.requestFrame();
        });

        this.canvas.addEventListener('wheel', e => {
            e.preventDefault();
            const dy = e.deltaY * 0.001;
            if (e.ctrlKey) {
                this.view.log2zoom -= dy * 10.0;
            } else {
                const dx = e.deltaX * 0.001;
                this._handleMove(dx, dy, e.shiftKey);
            }
            this.requestFrame();
        }, {passive: false});
    }

    resetView() {
        if (!this.isLoaded) return;
        this.view.pan = 0; 
        this.view.tilt = 0; 
        this.view.log2zoom = 0;
        this.view.centerX = (this.bounds.minX + this.bounds.maxX) / 2;
        this.view.centerY = (this.bounds.minY + this.bounds.maxY) / 2;
        this.updateBaseScale();
        this.requestFrame();
    }

    toggleAllLayers(visible) {
        for (const lid in this.layers) {
            this.layers[lid].visible = visible;
        }
        this.requestFrame();
    }

    resize() {
        const dpr = window.devicePixelRatio || 1;
        this.canvas.width = window.innerWidth * dpr;
        this.canvas.height = window.innerHeight * dpr;
        this.gl.viewport(0, 0, this.canvas.width, this.canvas.height);
        this.updateBaseScale();
        this.requestFrame();
    }

    updateBaseScale() {
        if (!this.isLoaded) return;
        const width = this.bounds.maxX - this.bounds.minX;
        const height = this.bounds.maxY - this.bounds.minY;
        const viewAspect = this.canvas.width / Math.max(this.canvas.height, 1);
        this.view.baseScale = (width / height > viewAspect) ? (1.0 / width) : (1.0 / height / viewAspect);
    }

    _handleMove(dx, dy, isRotate) {
        if (isRotate) {
            this.view.pan -= dx;
            this.view.tilt -= dy;
        } else {
            const speed = Math.pow(2.0, -this.view.log2zoom) / this.view.baseScale;
            const s = Math.sin(-this.view.pan), c = Math.cos(this.view.pan);
            this.view.centerX += (c * dx + s * dy) * speed;
            this.view.centerY += (s * dx - c * dy) * speed;
        }
    }

    createShader(type, source) {
        const gl = this.gl;
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

    createProgram(vs, fs) {
        const gl = this.gl;
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

    requestFrame() {
        if (!this.frameRequested) {
            this.frameRequested = true;
            this.lastFrameTime = performance.now();
            requestAnimationFrame(() => {
                this.frameRequested = false;
                
                const now = performance.now();
                const dt = (now - (this.lastFrameTime || now)) / 1000.0;
                this.lastFrameTime = now;

                let isAnimating = false;
                if (this.animator && this.animator.isPlaying) {
                    isAnimating = this.animator.tick(dt, this);
                    if (isAnimating) {
                        this.requestFrame();
                    }
                }
                this.render();
            });
        }
    }

    render() {
        const gl = this.gl;
        if (!gl) return;
        
        gl.clearColor(0.05, 0.05, 0.05, 1.0);
        gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);

        // View Matrix Calculation
        const cx = this.view.centerX, cy = this.view.centerY;
        const cp = Math.cos(this.view.pan), sp = Math.sin(this.view.pan);
        const ct = Math.cos(this.view.tilt), st = Math.sin(this.view.tilt);
        const s = 2.0 * Math.pow(2.0, this.view.log2zoom) * this.view.baseScale;
        const pers = this.view.perspective;
        const asp = this.canvas.width / Math.max(this.canvas.height, 1);

        const m00 = s*cp, m01 = -s*sp, m02 = 0,    m03 = s*(-cx*cp + cy*sp);
        const m10 = ct*s*sp, m11 = ct*s*cp, m12 = -st*s, m13 = ct*s*(-cx*sp - cy*cp);
        const m20 = st*s*sp, m21 = st*s*cp, m22 = ct*s,  m23 = st*s*(-cx*sp - cy*cp);

        const viewMat = new Float32Array([
            m00/asp, m10, -0.01*m20, -pers*m20,
            m01/asp, m11, -0.01*m21, -pers*m21,
            m02/asp, m12, -0.01*m22, -pers*m22,
            m03/asp, m13, -0.01*m23, -pers*m23 + 1
        ]);

        gl.uniformMatrix4fv(this.unis.viewMat, false, viewMat);
        gl.uniform1f(this.unis.isHighlighting, (this.highlightedCount > 0) ? 1.0 : 0.0);
        gl.uniform1f(this.unis.globalAlpha, this.view.globalAlpha);
        gl.uniform1f(this.unis.stateMix, this.view.stateMix);
        gl.uniform2iv(this.unis.netStatesSize, this.netStatesSize);
        
        gl.activeTexture(gl.TEXTURE0);
        gl.bindTexture(gl.TEXTURE_2D, this.netStateTexture);
        gl.uniform1i(this.unis.netStates, 0);

        gl.uniform1f(this.unis.showBoundaries, this.view.showBoundaries ? 1.0 : 0.0);
        gl.uniform1f(this.unis.showPowerNets, this.view.showPowerNets ? 1.0 : 0.0);

        gl.enable(gl.DEPTH_TEST);
        gl.enable(gl.BLEND);
        gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);
        //gl.enable(gl.SAMPLE_ALPHA_TO_COVERAGE);

        const reverseOrder = Math.cos(this.view.tilt) < 0;
        const sortedLidsForRender = Object.keys(this.layers).sort((a, b) => {
            const zA = this.layerSpecs[a]?.z || 0;
            const zB = this.layerSpecs[b]?.z || 0;
            return reverseOrder ? (zB - zA) : (zA - zB);
        });

        const isCube = this.view.renderMode === 'cube';
        const indexCount = isCube ? 36 : 6;
        gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, this.ebo);

        const { rect: a_rect, net: a_net } = this.attribs;

        for (const lid of sortedLidsForRender) {
            const layer = this.layers[lid];
            if (!layer || layer.count === 0) continue;
            
            if (this.soloLayerId !== null) {
                const isActive = (lid >= 3 && lid <= 5);
                const isSoloActive = (this.soloLayerId >= 3 && this.soloLayerId <= 5);
                if (isActive && isSoloActive) { /* ok */ }
                else if (String(lid) !== String(this.soloLayerId)) continue;
            } else if (!layer.visible) {
                continue;
            }

            const config = LAYER_CONFIG[lid] || { name: "", color: [0.5, 0.5, 0.5, 1.0], isExempt: false };
            gl.uniform1f(this.unis.isExemptLayer, config.isExempt ? 1.0 : 0.0);
            
            const spec = this.layerSpecs[lid] || { z: 0, h: 0 };
            gl.uniform1f(this.unis.layerZ, spec.z * this.view.explode);
            gl.uniform1f(this.unis.thickness, spec.h);
            gl.uniform4fv(this.unis.color, config.color);
            gl.uniform1f(this.unis.layerAlpha, config.alphaMultiplier !== undefined ? config.alphaMultiplier : 1.0);

            gl.bindBuffer(gl.ARRAY_BUFFER, layer.buffer);
            const stride = 6 * 4;
            
            gl.enableVertexAttribArray(a_rect);
            gl.vertexAttribIPointer(a_rect, 4, gl.INT, stride, 0);
            gl.vertexAttribDivisor(a_rect, 1);
            
            gl.enableVertexAttribArray(a_net);
            gl.vertexAttribIPointer(a_net, 1, gl.INT, stride, 4 * 4);
            gl.vertexAttribDivisor(a_net, 1);

            if (this.attribs.tree !== -1) {
                gl.enableVertexAttribArray(this.attribs.tree);
                gl.vertexAttribIPointer(this.attribs.tree, 1, gl.INT, stride, 5 * 4);
                gl.vertexAttribDivisor(this.attribs.tree, 1);
            }

            gl.drawElementsInstanced(gl.TRIANGLES, indexCount, gl.UNSIGNED_SHORT, 0, layer.count);
        }
    }

    log(msg) {
        this.onLog?.(msg);
    }

    async loadGDS(url, pdk) {
        this.vgaRunning = false;
        if (this.vgaInterval) {
            clearInterval(this.vgaInterval);
            this.vgaInterval = null;
        }
        this.onLog?.(`Loading GDS: ${url} (PDK: ${pdk || "default"})\n`);
        this.onProgress?.("Parsing GDS in worker...");

        if (this.worker) this.worker.terminate();
        this.worker = new Worker("web/gds_worker.js", { type: "module" });

        this.worker.onmessage = (e) => {
            const data = e.data;
            if (data.type === "log") {
                this.log(data.message);
            } else if (data.type === "error") {
                this.log("ERROR: " + data.message);
                this.onProgress?.("Error: " + data.message);
            } else if (data.type === "done") {
                if (data.stats.vgaWidth) this.vga.width = data.stats.vgaWidth;
                if (data.stats.vgaHeight) this.vga.height = data.stats.vgaHeight;
                this.processParsedData(data.stats, data.wireNames);
            } else if (data.type === "callResult") {
                if (data.name === "wasm_vga_width") this.vga.width = data.result;
                if (data.name === "wasm_vga_height") this.vga.height = data.result;

                if (data.wireData) {
                    this.updateNetStatesFromWireData(data.wireData);
                    this.onUpdateCircuit?.(data.wireData);
                }
                if (data.vga_buffer) {
                    this.vga.buffer = data.vga_buffer;
                    // Ray positions are now passed in data.stats or data directly from worker
                }
            } else if (data.type === 'simUpdate') {
                if (this.simInputPauseActive) return; // Drop stale incoming frames while freezing for input manipulation
                
                if (data.wireData) {
                    this.updateNetStatesFromWireData(data.wireData);
                    this.onUpdateCircuit?.(data.wireData);
                }
                if (data.vga_buffer) {
                    this.vga.buffer = data.vga_buffer;
                    this.onVgaFrame?.(data.vga_buffer, this.vga.width, this.vga.height, data.rayX, data.rayY);
                }
            } else if (data.type === 'ack_response') {
                if (this.workerAckResolve) {
                    this.workerAckResolve();
                    this.workerAckResolve = null;
                }
            }
        };

        const normalizedUrl = new URL(url, window.location.href).href;
        this.worker.postMessage({ type: "start", gdsUrl: normalizedUrl, pdk: pdk });
    }

    // MARK: GDS Data Processing
    processParsedData(stats, wireNames) {
        this.wireNames = wireNames || [];
        const { rectData, layerOffsets } = stats;
        const gl = this.gl;
        
        if (!rectData || !layerOffsets) {
            this.log("ERROR: No geometry data.\n");
            return;
        }

        this.netAreas = {};
        this.treeAreas = {};
        this.treeToNets = {};
        this.bounds = { minX: Infinity, minY: Infinity, maxX: -Infinity, maxY: -Infinity };
        this.layers = {};
        this.sortedLids = [];
        
        const L_COUNT = layerOffsets.length - 1;
        let maxNet = -1;

        for (let lid = 0; lid < L_COUNT; lid++) {
            const start = layerOffsets[lid] * 6;
            const end = layerOffsets[lid+1] * 6;
            if (start === end) continue;
            
            const data = rectData.subarray(start, end);
            const buffer = gl.createBuffer();
            gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
            gl.bufferData(gl.ARRAY_BUFFER, data, gl.STATIC_DRAW);
            
            this.layers[lid] = { count: data.length / 6, buffer: buffer, visible: true };
            this.sortedLids.push(String(lid));
            
            for (let i = 0; i < data.length; i += 6) {
                const x1 = data[i], y1 = data[i+1], x2 = data[i+2], y2 = data[i+3], net = data[i+4], tree = data[i+5];
                if (x1 < this.bounds.minX) this.bounds.minX = x1;
                if (y1 < this.bounds.minY) this.bounds.minY = y1;
                if (x2 > this.bounds.maxX) this.bounds.maxX = x2;
                if (y2 > this.bounds.maxY) this.bounds.maxY = y2;
                
                if (net !== -1) {
                    if (net > maxNet) maxNet = net;
                    const area = Math.abs((x2 - x1) * (y2 - y1));
                    this.netAreas[net] = (this.netAreas[net] || 0) + area;
                    if (tree !== -1) {
                        this.treeAreas[tree] = (this.treeAreas[tree] || 0) + area;
                        if (!this.treeToNets[tree]) this.treeToNets[tree] = new Set();
                        this.treeToNets[tree].add(net);
                    }
                }
            }
        }

        this.setupNetStateTexture(maxNet);
        this.isLoaded = true;
        this.resetView();

        for (let i = 0; i < LAYER_CONFIG.length; i++) {
            this.layerSpecs[i] = { z: LAYER_CONFIG[i].z, h: LAYER_CONFIG[i].h };
        }

        this.onLoaded?.();
        this.requestFrame();
    }

    setupNetStateTexture(maxNet) {
        const gl = this.gl;
        if (maxNet >= 0) {
            const texWidth = 2048;
            const texHeight = Math.ceil((maxNet + 1) / texWidth);
            this.netStatesSize = [texWidth, texHeight];
            this.netStateData = new Uint8Array(texWidth * texHeight);
            
            if (this.netStateTexture) gl.deleteTexture(this.netStateTexture);
            this.netStateTexture = gl.createTexture();
            gl.bindTexture(gl.TEXTURE_2D, this.netStateTexture);
            gl.texImage2D(gl.TEXTURE_2D, 0, gl.R8UI, texWidth, texHeight, 0, gl.RED_INTEGER, gl.UNSIGNED_BYTE, this.netStateData);
            gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
            gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
        } else {
            this.netStatesSize = [0, 0];
            this.netStateData = null;
        }
    }

    syncSelectedNets(selectedIds) {
        if (!this.netStateData) return;
        
        // Clear bit 7 (highlight) for all
        for (let i = 0; i < this.netStateData.length; i++) {
            this.netStateData[i] &= ~0x80;
        }

        this.highlightedCount = selectedIds.length;
        selectedIds.forEach(id => {
            if (id >= 0 && id < this.netStateData.length) this.netStateData[id] |= 0x80;
        });
        
        this.updateNetStateTexture();
        this.requestFrame();
    }

    updateNetStateTexture() {
        if (!this.netStateTexture) return;
        const gl = this.gl;
        gl.bindTexture(gl.TEXTURE_2D, this.netStateTexture);
        gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, this.netStatesSize[0], this.netStatesSize[1], gl.RED_INTEGER, gl.UNSIGNED_BYTE, this.netStateData);
        this.requestFrame();
    }

    setSoloLayer(lid) {
        this.soloLayerId = lid;
        this.requestFrame();
    }

    setRenderMode(mode) {
        if (mode !== 'quad' && mode !== 'cube') return;
        this.view.renderMode = mode;
        this.requestFrame();
    }

    // MARK: Simulation Bridge
    async flushPendingInputs() {
        if (this.simInputPauseActive || this.pendingInputs.length === 0) return;
        this.simInputPauseActive = true;

        const isRunning = this.simSpeed > 0 || this.autoClock;
        
        if (isRunning) {
            // Signal pausing execution
            this.worker.postMessage({ type: 'sim_config', speed: 0, autoClock: false });
            // Wait till last messages arrive and state is accurately aligned
            await this.waitForWorkerAck();
        }

        let updateOccurred = false;

        // Process accumulated toggles
        while (this.pendingInputs.length > 0) {
            const inputs = [...this.pendingInputs];
            this.pendingInputs = [];
            
            let latestInputs = {};
            for (let inp of inputs) {
                latestInputs[inp.id] = inp.val;
            }

            for (let idStr in latestInputs) {
                let id = parseInt(idStr);
                let val = latestInputs[id];
                this.callWasm("wasm_circuit_set_input", [id, val], []);
                updateOccurred = true;
            }
        }

        if (updateOccurred) {
            if (isRunning) {
                // Ensure inputs are deeply routed
                await this.waitForWorkerAck();
            } else {
                // If paused, comb logic affects others, so we need fresh wireData
                this.callWasm("wasm_circuit_is_settled", [], ["wireData"]);
            }
        }

        if (isRunning) {
            // Resume execution
            this.worker.postMessage({ type: 'sim_config', speed: this.simSpeed, autoClock: this.autoClock });
        }

        this.simInputPauseActive = false;
        
        if (this.pendingInputs.length > 0) {
            this.flushPendingInputs();
        }
    }

    toggleWire(id) {
        const currentVal = this.netStateData ? (this.netStateData[id] & 1) : 0;
        const newVal = currentVal === 1 ? 0 : 1;
        
        // Optimistic state update immediately
        if (this.netStateData) {
            this.netStateData[id] = (this.netStateData[id] & ~1) | newVal;
            // Add a visual 'flipped' highlight bit (0x40)
            this.netStateData[id] |= 0x40;
            this.updateNetStateTexture();
            this.onUpdateCircuit?.();
        }

        this.pendingInputs.push({id, val: newVal});
        this.flushPendingInputs();
    }

    async waitForWorkerAck() {
        return new Promise(resolve => {
            this.workerAckResolve = resolve;
            this.worker.postMessage({ type: 'ack_request' });
        });
    }

    setSimConfig(speed, autoClock) {
        this.simSpeed = speed;
        this.autoClock = autoClock;
        if (!this.worker) return;
        
        if (!this.simInputPauseActive) {
            this.worker.postMessage({ type: 'sim_config', speed, autoClock });
        }
    }

    callWasm(name, args, returnArrays) {
        if (!this.worker) return;
        this.worker.postMessage({ type: "call", name, args, returnArrays });
    }

    updateNetStatesFromWireData(wireData) {
        if (!this.netStateData) return;
        
        for (let i = 0; i < wireData.length; i++) {
            if (i < this.netStateData.length) {
                // Bit 0: Logic Value
                const val = wireData[i] & 1;
                
                // Bit 6: Flipped (compared to last update)
                let flipped = 0;
                if (this.lastWireData && i < this.lastWireData.length) {
                    const lastVal = this.lastWireData[i] & 1;
                    if (val !== lastVal) flipped = 0x40;
                }

                // Preserve bit 7 (selection highlight)
                let highlight = this.netStateData[i] & 0x80;
                this.netStateData[i] = val | flipped | highlight;
            }
        }
        
        // Save current state for next comparison
        if (!this.lastWireData || this.lastWireData.length !== wireData.length) {
            this.lastWireData = new Uint8Array(wireData);
        } else {
            this.lastWireData.set(wireData);
        }

        this.updateNetStateTexture();
        this.requestFrame();
    }
}
