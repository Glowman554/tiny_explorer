import { LAYER_CONFIG } from './circuit_viewer.js';

/**
 * UI Glue Logic - Keeps the Viewer class pure but maintains functionality
 */
export function initViewerUI(viewer) {
    const doc = (id) => document.getElementById(id);
    const logContainer = doc('logContainer');
    const logPanel = doc('logPanel');
    const loading = doc('loading');
    const netSelect = doc('netSelect');
    const netSearch = doc('netSearch');
    const alphaSlider = doc('alphaSlider');
    const boundaryToggle = doc('boundaryToggle');
    const powerNetToggle = doc('powerNetToggle');

    viewer.onLog = (msg) => {
        if (!logContainer) return;
        logContainer.textContent += msg;
        if (logPanel) logPanel.classList.remove('collapsed');
        logContainer.parentElement.scrollTop = logContainer.parentElement.scrollHeight;
    };

    viewer.onProgress = (msg) => {
        if (loading) {
            loading.innerText = msg;
            loading.style.display = 'block';
        }
        if (logPanel) logPanel.classList.remove('collapsed');
    };

    viewer.onLoaded = () => {
        if (loading) loading.style.display = 'none';
        rebuildLayerUI(viewer);
        updateNetListUI(viewer);
        updateCircuitMonitor(viewer);
    };

    viewer.onUpdateCircuit = (wireData) => {
        updateCircuitMonitor(viewer, wireData);
    };

    // Toggle logic for all panels
    document.querySelectorAll('.panel-header').forEach(header => {
        header.onclick = () => {
            header.parentElement.classList.toggle('collapsed');
        };
    });

    // Event listeners for viewer state
    if (alphaSlider) alphaSlider.oninput = () => {
        viewer.view.globalAlpha = parseFloat(alphaSlider.value);
        viewer.requestFrame();
    };
    if (boundaryToggle) boundaryToggle.onchange = () => {
        viewer.view.showBoundaries = boundaryToggle.checked;
        viewer.requestFrame();
    };
    if (powerNetToggle) powerNetToggle.onchange = () => {
        viewer.view.showPowerNets = powerNetToggle.checked;
        viewer.requestFrame();
    };
    const stateMixSlider = doc('stateMixSlider');
    if (stateMixSlider) stateMixSlider.oninput = () => {
        viewer.view.stateMix = parseFloat(stateMixSlider.value);
        viewer.requestFrame();
    };
    if (doc('btnReset')) doc('btnReset').onclick = () => viewer.resetView();
    if (doc('btnToggleAll')) doc('btnToggleAll').onclick = () => {
        const anyVisible = Object.values(viewer.layers).some(l => l.visible);
        viewer.toggleAllLayers(!anyVisible);
        rebuildLayerUI(viewer);
    };

    if (netSearch) {
        netSearch.oninput = () => updateNetListUI(viewer);
        netSearch.onkeydown = (e) => {
            if (e.key === 'ArrowDown' && netSelect && netSelect.options.length > 0) {
                e.preventDefault();
                netSelect.focus();
                if (netSelect.selectedIndex === -1) {
                    netSelect.options[0].selected = true;
                    syncNetSelection(viewer);
                }
            }
        };
    }

    if (netSelect) {
        netSelect.onchange = netSelect.oninput = netSelect.onkeyup = netSelect.onclick = () => syncNetSelection(viewer);
    }

    if (doc('btnClearNet')) doc('btnClearNet').onclick = () => {
        if (netSelect) netSelect.selectedIndex = -1;
        if (netSearch) netSearch.value = '';
        viewer.syncSelectedNets([]);
        updateNetListUI(viewer);
    };

    window.addEventListener('keydown', (e) => {
        if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
            const dir = e.key === 'ArrowDown' ? 1 : -1;
            if (viewer.soloLayerId !== null) {
                cycleSoloLayer(viewer, dir);
            } else if (netSelect && netSelect.options.length > 0) {
                e.preventDefault();
                netSelect.selectedIndex = Math.max(0, Math.min(netSelect.selectedIndex + dir, netSelect.options.length - 1));
                syncNetSelection(viewer);
            }
        }
        if (e.key === 'Escape') {
            viewer.setSoloLayer(null);
            rebuildLayerUI(viewer);
        }
    });
}

function syncNetSelection(viewer) {
    const netSelect = document.getElementById('netSelect');
    if (!netSelect) return;
    const selectedIds = Array.from(netSelect.selectedOptions).map(opt => parseInt(opt.value));
    viewer.syncSelectedNets(selectedIds);
}

function updateNetListUI(viewer) {
    const select = document.getElementById('netSelect');
    const search = document.getElementById('netSearch');
    if (!select || !search) return;

    const query = search.value.toLowerCase();
    const sortedNets = Object.entries(viewer.netAreas).sort((a, b) => b[1] - a[1]);
    const netToName = {};
    viewer.wireNames.forEach(w => netToName[w.id] = w.name);

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
            if (viewer.netStateData && (viewer.netStateData[net] & 0x80)) opt.selected = true;
            select.appendChild(opt);
            if (++count >= 1000) break;
        }
    }
}

export function rebuildLayerUI(viewer) {
    const layerList = document.getElementById('layerList');
    if (!layerList) return;
    layerList.innerHTML = '';
    
    // Sort layers by Z descending for UI
    const lids = viewer.sortedLids.slice().sort((a, b) => Number(b) - Number(a));
    
    lids.forEach(lid => {
        if (lid == 3 || lid == 4) return; // Hidden FET layers
        
        const div = document.createElement('div');
        div.className = 'layer-toggle';
        const config = LAYER_CONFIG[lid] || { name: "Layer " + lid, color: [0.5, 0.5, 0.5] };

        // Checkbox
        const input = document.createElement('input');
        input.type = 'checkbox';
        input.checked = viewer.layers[lid].visible;
        input.onchange = (e) => {
            viewer.layers[lid].visible = e.target.checked;
            if (lid == 5) {
                if (viewer.layers[3]) viewer.layers[3].visible = e.target.checked;
                if (viewer.layers[4]) viewer.layers[4].visible = e.target.checked;
            }
            viewer.requestFrame();
        };
        div.appendChild(input);

        // Swatches and Labels
        const addControl = (id, text) => {
            const cfg = LAYER_CONFIG[id];
            if (!cfg) return;
            const c = cfg.color;
            const s = document.createElement('span');
            s.className = 'layer-color';
            s.style.backgroundColor = `rgba(${c[0]*255}, ${c[1]*255}, ${c[2]*255}, 1)`;
            s.onclick = () => {
                viewer.setSoloLayer(viewer.soloLayerId === id ? null : id);
                rebuildLayerUI(viewer);
            };
            div.appendChild(s);
            
            if (text) {
                const l = document.createElement('span');
                l.innerText = text;
                l.onclick = s.onclick;
                div.appendChild(l);
            }
        };

        if (lid == 5) {
            addControl(4); addControl(3); addControl(5, " FET");
        } else {
            addControl(lid, " " + config.name);
        }

        // Apply solo highlights
        if (viewer.soloLayerId !== null) {
            const isActive = (lid >= 3 && lid <= 5);
            const isSoloActive = (viewer.soloLayerId >= 3 && viewer.soloLayerId <= 5);
            const match = (isActive && isSoloActive) || (String(lid) === String(viewer.soloLayerId));
            div.style.opacity = match ? '1.0' : '0.3';
            div.style.background = match ? 'rgba(0, 255, 0, 0.1)' : 'transparent';
        }

        layerList.appendChild(div);
    });
}

function cycleSoloLayer(viewer, dir) {
    if (viewer.sortedLids.length === 0) return;
    let idx = viewer.sortedLids.indexOf(String(viewer.soloLayerId));
    if (idx === -1) {
        idx = (dir > 0) ? 0 : viewer.sortedLids.length - 1;
    } else {
        idx = (idx + dir + viewer.sortedLids.length) % viewer.sortedLids.length;
    }
    viewer.setSoloLayer(viewer.sortedLids[idx]);
    rebuildLayerUI(viewer);
}

export function updateCircuitMonitor(viewer, wireData) {
    const monitor = document.getElementById('circuitMonitorContent');
    if (!monitor) return;
    if (viewer.wireNames.length === 0) {
        monitor.innerHTML = '<div style="padding:10px; color:#666;">No circuit data</div>';
        return;
    }

    const getState = (id) => {
        const d = (wireData) ? wireData[id] : (viewer.netStateData ? viewer.netStateData[id] : 0);
        return d & 1; // Simulation value is bit 0
    };

    // TT-style signals
    const specialSpecs = [
        { name: 'VDD', id: 0 },
        { name: 'VSS', id: 1 },
        { name: 'CLK', pattern: /^clk$/i },
        { name: 'ENA', pattern: /^ena$/i },
        { name: 'RST', pattern: /^rst_n$/i },
    ];
    
    const groups = [
        { label: 'ui_in (Inputs)', pattern: /^ui_in\[(\d+)\]$/ },
        { label: 'uo_out (Outputs)', pattern: /^uo_out\[(\d+)\]$/ },
        { label: 'uio_in (Bidi In)', pattern: /^uio_in\[(\d+)\]$/ },
        { label: 'uio_out (Bidi Out)', pattern: /^uio_out\[(\d+)\]$/ }
    ];

    const findWire = (pattern) => viewer.wireNames.find(w => pattern.test(w.name));
    const findGroupWires = (pattern) => {
        const res = new Array(8).fill(null);
        viewer.wireNames.forEach(w => {
            const m = w.name.match(pattern);
            if (m) {
                const idx = parseInt(m[1]);
                if (idx < 8) res[idx] = w;
            }
        });
        return res;
    };

    monitor.innerHTML = '';
    
    // Controls
    const controls = document.createElement('div');
    controls.className = 'monitor-controls';
    controls.innerHTML = '<button id="btnStepWave">Step Wave</button>';
    monitor.appendChild(controls);
    const btnStep = document.getElementById('btnStepWave');
    if (btnStep) btnStep.onclick = () => viewer.stepCircuit();

    // Special Row
    const specialRow = document.createElement('div');
    specialRow.className = 'monitor-special-row';
    specialSpecs.forEach(spec => {
        const wire = (spec.id !== undefined) ? { id: spec.id } : findWire(spec.pattern);
        const btn = document.createElement('div');
        btn.className = 'monitor-special-btn';
        btn.innerText = spec.name;
        if (wire) {
            const state = getState(wire.id);
            if (state === 1) btn.classList.add('state-high');
            else btn.classList.add('state-low');
            btn.onclick = () => viewer.toggleWire(wire.id, state);
        } else {
            btn.style.opacity = '0.2';
            btn.style.cursor = 'default';
        }
        specialRow.appendChild(btn);
    });
    monitor.appendChild(specialRow);

    // Groups
    groups.forEach(g => {
        const label = document.createElement('div');
        label.className = 'monitor-group-label';
        label.innerText = g.label;
        monitor.appendChild(label);

        const grid = document.createElement('div');
        grid.className = 'monitor-grid';
        const groupWires = findGroupWires(g.pattern);
        groupWires.forEach((wire, i) => {
            const btn = document.createElement('div');
            btn.className = 'monitor-btn';
            btn.innerText = i;
            if (wire) {
                const state = getState(wire.id);
                if (state === 1) btn.classList.add('state-high');
                else btn.classList.add('state-low');
                btn.onclick = () => viewer.toggleWire(wire.id, state);
            } else {
                btn.style.opacity = '0.15';
                btn.style.cursor = 'default';
                btn.style.color = '#333';
            }
            grid.appendChild(btn);
        });
        monitor.appendChild(grid);
    });
}
