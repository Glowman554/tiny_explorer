import { LAYER_CONFIG } from './circuit_viewer.js';
import { makeDraggable } from './draggable.js';
import { createStore } from './store.js';

/**
 * UI Glue Logic - Keeps the Viewer class pure but maintains functionality
 */

// MARK: - Internal Utilities
const $ = (id) => document.getElementById(id);
const on = (id, evt, fn) => { 
    const el = $(id); 
    if (el) el[evt] = fn; 
    return el; 
};

// MARK: - Main UI Initializer
export function initViewerUI(viewer) {
    const loading = $('loading');

    on('loadFlashHex', 'onclick', () => {
        const input = $('flashHexInput');
        const status = $('flashLoadStatus');
        if (!input || !viewer.worker) return;
        const tokens = input.value.trim().split(/\s+/).filter(Boolean);
        const valid = tokens.every(token => /^(?:0x)?[0-9a-fA-F]{2}$/.test(token));
        if (!valid) { if (status) status.textContent = 'Use two-digit hex bytes separated by spaces or newlines.'; return; }
        viewer.worker.postMessage({ type: 'flash_hex', text: input.value });
    });

    // MARK: Panel Management
    const updatePanel = (btnId, panelId, visible) => {
        const btn = $(btnId);
        const panel = $(panelId);
        if (!panel) return;
        panel._isVisible = visible;
        panel.classList.toggle('hidden', !visible);
        if (btn) {
            btn.classList.toggle('btn-active', visible);
            btn.classList.toggle('btn-inactive', !visible);
        }
    };

    const togglePanel = (btnId, panelId, defaultVisible = true) => {
        const panel = $(panelId);
        if (panel) panel._isVisible = defaultVisible;
        on(btnId, 'onclick', () => updatePanel(btnId, panelId, !panel._isVisible));
        updatePanel(btnId, panelId, defaultVisible);
    };

    togglePanel('toggleUiBtn', 'ui', true);
    togglePanel('toggleMonitorBtn', 'circuitMonitor', true);
    togglePanel('toggleVgaBtn', 'vgaMonitor', false); // Hide VGA by default
    togglePanel('toggleLogBtn', 'logPanel', true);
    togglePanel('toggleHelpBtn', 'helpPanel', false); // Help disabled by default

    const forceShowPanel = (btnId, panelId) => updatePanel(btnId, panelId, true);
    const forceHidePanel = (btnId, panelId) => updatePanel(btnId, panelId, false);

    // Close buttons
    on('close-ui', 'onclick', () => forceHidePanel('toggleUiBtn', 'ui'));
    on('close-circuitMonitor', 'onclick', () => forceHidePanel('toggleMonitorBtn', 'circuitMonitor'));
    on('close-vgaMonitor', 'onclick', () => forceHidePanel('toggleVgaBtn', 'vgaMonitor'));
    on('close-logPanel', 'onclick', () => forceHidePanel('toggleLogBtn', 'logPanel'));
    on('close-helpPanel', 'onclick', () => forceHidePanel('toggleHelpBtn', 'helpPanel'));
    
    // Scale buttons
    const vgaCanvas = $('vgaCanvas');
    
    // Store current scale globally attached to canvas node
    if (vgaCanvas) vgaCanvas._vgaScale = 0.5;
    
    const setVgaScale = (scale) => {
        if (!vgaCanvas) return;
        vgaCanvas._vgaScale = scale;
        
        // Refresh canvas visual CSS scaling immediately 
        vgaCanvas.style.width = (vgaCanvas.width * scale) + 'px';
        vgaCanvas.style.height = (vgaCanvas.height * scale) + 'px';
        
        // Ensure the panel container stays tight around the scaled canvas
        const panelContent = $('vgaMonitorContent');
        if (panelContent) {
            panelContent.style.width = (vgaCanvas.width * scale) + 'px';
        }
        
        const btn05 = $('scale-vga-05');
        const btn10 = $('scale-vga-10');
        if (btn05) btn05.classList.toggle('btn-active', scale === 0.5);
        if (btn05) btn05.classList.toggle('btn-inactive', scale !== 0.5);
        if (btn10) btn10.classList.toggle('btn-active', scale === 1.0);
        if (btn10) btn10.classList.toggle('btn-inactive', scale !== 1.0);
    };
    
    setVgaScale(0.5);
    on('scale-vga-05', 'onclick', () => setVgaScale(0.5));
    on('scale-vga-10', 'onclick', () => setVgaScale(1.0));


    // MARK: Panel Draggability

    makeDraggable('ui');
    makeDraggable('circuitMonitor');
    makeDraggable('vgaMonitor');
    makeDraggable('logPanel');
    makeDraggable('helpPanel');

    // MARK: Engine Callbacks
    viewer.onLog = (msg) => {
        const logContainer = $('logContainer');
        if (!logContainer) return;
        logContainer.textContent += msg;
        forceShowPanel('toggleLogBtn', 'logPanel');
        logContainer.parentElement.scrollTop = logContainer.parentElement.scrollHeight;
    };

    viewer.onProgress = (msg) => {
        if (loading) {
            loading.innerText = msg;
            loading.classList.remove('hidden');
        }
        forceShowPanel('toggleLogBtn', 'logPanel');
    };

    viewer.onLoaded = () => {
        if (loading) loading.classList.add('hidden');
        rebuildLayerUI(viewer);
        updateTreeListUI(viewer);
        
        const monitor = document.getElementById('circuitMonitorContent');
        if (monitor) monitor.innerHTML = ''; // Force a completely fresh layout on new GDS 
        buildCircuitMonitorUI(viewer);
        updateCircuitMonitorState(viewer);
        
        // Auto-hide System Log 2 seconds after circuit is fully loaded and shown
        setTimeout(() => {
            forceHidePanel('toggleLogBtn', 'logPanel');
        }, 2000);
    };

    // Initialize sub-controllers
    bindViewControls(viewer);
    bindSimulationControls(viewer);
    bindAnimationControls(viewer);
    bindTreeInspectorUI(viewer);
    bindLayerControls(viewer);
    bindKeyboardHotkeys(viewer);
    // The worker now drives VGA updates and ray properties, remove UI driving
    viewer.onUpdateCircuit = (wireData) => {
        updateCircuitMonitorState(viewer, wireData);
    };

    viewer.onVgaFrame = vga => {
        updateVgaMonitor(viewer, vga);
        const rayLabel = $('vgaRayPos');
        if (rayLabel) rayLabel.innerText = `Ray: X:${vga.rayX} Y:${vga.rayY}`;
    };

    viewer.onMetadata = () => {
        if (viewer.isLoaded) {
            const monitor = document.getElementById('circuitMonitorContent');
            if (monitor) monitor.innerHTML = '';
            buildCircuitMonitorUI(viewer);
            updateCircuitMonitorState(viewer);
        }
    };
    
    on('btnReset', 'onclick', () => viewer.resetView());
    on('btnDownload', 'onclick', async () => {
        if (!viewer.currentUrl) return;
        const btn = $('btnDownload');
        const oldText = btn.innerText;
        try {
            btn.innerText = 'Downloading...';
            btn.disabled = true;
            const url = viewer.currentUrl;
            const name = url.split('/').pop().split('?')[0];
            const response = await fetch(url);
            const blob = await response.blob();
            const link = document.createElement('a');
            link.href = URL.createObjectURL(blob);
            link.download = name;
            link.click();
            URL.revokeObjectURL(link.href);
        } catch (e) {
            console.error('Download failed:', e);
            viewer.log(`ERROR: Download failed: ${e.message}\n`);
        } finally {
            btn.innerText = oldText;
            btn.disabled = false;
        }
    });

}

// MARK: - Sub-Controllers

export function bindViewControls(viewer) {
    const v = viewer.view;
    const update = () => viewer.requestFrame();

    on('alphaSlider', 'oninput', (e) => { v.globalAlpha = parseFloat(e.target.value); update(); });
    on('perspSlider', 'oninput', (e) => { v.perspective = parseFloat(e.target.value); update(); });
    on('explodeSlider', 'oninput', (e) => { v.explode = parseFloat(e.target.value); update(); });
    on('stateMixSlider', 'oninput', (e) => { v.stateMix = parseFloat(e.target.value); update(); });

    on('boundaryToggle', 'onchange', (e) => { v.showBoundaries = e.target.checked; update(); });
    on('cellModeToggle', 'onchange', (e) => { v.renderMode = e.target.checked ? 'cells' : 'standard'; update(); });
    on('powerNetToggle', 'onchange', (e) => { v.showPowerNets = e.target.checked; update(); });
}

export function bindAnimationControls(viewer) {
    if (!viewer.animator) return;
    
    on('btnCaptWp', 'onclick', () => {
        const animTime = $('animTime');
        if (animTime) viewer.animator.defaultTransitionTime = parseFloat(animTime.value);
        viewer.animator.addWaypoint(viewer);
        updateWaypointListUI(viewer.animator, $('wpList'));
    });

    on('btnClearWp', 'onclick', () => {
        viewer.animator.clearWaypoints();
        const btn = $('btnPlayAnim');
        if (btn) {
            btn.innerText = 'Play Sequence';
            btn.className = 'action-btn btn-success'; // Reset classes
        }
        updateWaypointListUI(viewer.animator, $('wpList'));
    });

    on('btnPlayAnim', 'onclick', () => {
        const btn = $('btnPlayAnim');
        if (viewer.animator.isPlaying) {
            viewer.animator.stop();
            btn.innerText = 'Play Sequence';
            btn.className = 'action-btn btn-success';
        } else if (viewer.animator.waypoints.length > 1) {
            const animTime = $('animTime');
            const duration = animTime ? parseFloat(animTime.value) : 2.0;
            viewer.animator.play(true, duration);
            btn.innerText = 'Stop Sequence';
            btn.className = 'action-btn btn-danger';
        }
        viewer.requestFrame();
    });
}

export function bindSimulationControls(viewer) {
    const simSpeed = $('simSpeed');
    const autoClockToggle = $('autoClockToggle');

    // UI Local State
    const Store = createStore({
        isPaused: true,
        simSpeed: simSpeed ? parseInt(simSpeed.value, 10) : 50,
        autoClock: autoClockToggle ? (autoClockToggle.classList.contains('state-high')) : true
    });
    
    if (viewer.isLoaded) {
        viewer.setSimConfig(0, Store.get().autoClock);
    }

    // 2. Declarative DOM Bindings
    Store.bind('isPaused', (paused) => {
        const btn = $('btnPlayPause');
        if (btn) {
            btn.innerText = paused ? '▶' : '⏸';
            btn.className = `btn-sm ${paused ? 'btn-success' : 'btn-active'}`;
        }
        const label = $('simSpeedLabel');
        if (label) {
            const speed = Store.get().simSpeed;
            label.innerText = paused ? 'READY' : (speed === 100 ? 'MAX' : `${speed}%`);
            label.className = `sim-speed-val ${paused ? 'btn-inactive' : 'btn-active'}`;
        }
    });

    Store.bind('simSpeed', (speed) => {
        const label = $('simSpeedLabel');
        if (label && !Store.get().isPaused) {
            label.innerText = speed === 100 ? 'MAX' : `${speed}%`;
        }
    });

    Store.bind('autoClock', (auto) => {
        const btn = $('autoClockToggle');
        if (btn) {
            btn.className = `monitor-special-btn ${auto ? 'state-high' : 'state-off-red'}`;
            btn.style.flex = '0 0 auto';
        }
    });

    // 3. Engine Sync
    Store.subscribe((state) => {
        if (viewer.isLoaded) {
            viewer.setSimConfig(state.isPaused ? 0 : state.simSpeed, state.isPaused ? false : state.autoClock);
        }
    });

    // 4. Mutation Handling
    on('btnPlayPause', 'onclick', () => {
        const nextPaused = !Store.get().isPaused;
        if (!nextPaused) {
            const mix = $('stateMixSlider');
            if (mix) { mix.value = 0.75; viewer.view.stateMix = 0.75; viewer.requestFrame(); }
        }
        Store.set({ isPaused: nextPaused });
    });

    on('simSpeed', 'oninput', (e) => {
        const speed = parseInt(e.target.value, 10);
        if (Store.get().isPaused) {
            const mix = $('stateMixSlider');
            if (mix) { mix.value = 0.75; viewer.view.stateMix = 0.75; viewer.requestFrame(); }
            Store.set({ isPaused: false, simSpeed: speed });
        } else {
            Store.set({ simSpeed: speed });
        }
    });
    
    on('autoClockToggle', 'onclick', () => Store.set({ autoClock: !Store.get().autoClock }));
}

export function bindTreeInspectorUI(viewer) {
    on('treeSearch', 'oninput', () => updateTreeListUI(viewer));
    on('treeSearch', 'onkeydown', (e) => {
        if (e.key === 'ArrowDown') {
            const select = $('treeSelect');
            if (select && select.options.length > 0) {
                e.preventDefault();
                select.focus();
                if (select.selectedIndex === -1) {
                    select.options[0].selected = true;
                    syncTreeSelection(viewer);
                }
            }
        }
    });

    const selectTrigger = () => syncTreeSelection(viewer);
    const select = $('treeSelect');
    if (select) {
        select.onchange = select.oninput = select.onkeyup = select.onclick = selectTrigger;
    }

    on('btnHighlightIds', 'onclick', () => {
        const input = $('treeIdList');
        if (!input) return;
        const ids = parseIdList(input.value);
        const selectedNets = new Set();
        ids.forEach(treeId => {
             if (viewer.treeToNets[treeId]) {
                  viewer.treeToNets[treeId].forEach(netId => selectedNets.add(netId));
             }
        });
        viewer.syncSelectedNets(Array.from(selectedNets));
        updateTreeListUI(viewer);
    });

    on('treeIdList', 'onkeydown', (e) => {
        if (e.key === 'Enter') {
            const btn = $('btnHighlightIds');
            if (btn) btn.click();
        }
    });

    on('btnClearTrees', 'onclick', () => {
        const select = $('treeSelect');
        if (select) select.selectedIndex = -1;
        const search = $('treeSearch');
        if (search) search.value = '';
        const list = $('treeIdList');
        if (list) list.value = '';
        viewer.syncSelectedNets([]);
        updateTreeListUI(viewer);
    });
}

export function bindKeyboardHotkeys(viewer) {
    window.addEventListener('keydown', (e) => {
        const treeSelect = document.getElementById('treeSelect');
        const btnPlayAnim = document.getElementById('btnPlayAnim');

        if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
            const dir = e.key === 'ArrowDown' ? 1 : -1;
            if (treeSelect && treeSelect.options.length > 0) {
                e.preventDefault();
                treeSelect.selectedIndex = Math.max(0, Math.min(treeSelect.selectedIndex + dir, treeSelect.options.length - 1));
                syncTreeSelection(viewer);
            }
        }
        
        // Don't trigger hotkeys if the user is typing in a field
        if (e.target.tagName === 'INPUT' || e.target.tagName === 'SELECT' || e.target.tagName === 'TEXTAREA') return;

        if (e.key.toLowerCase() === 'c') {
            const clkWire = viewer.wireNames.find(w => /^clk$/i.test(w.name));
            if (clkWire) {
                viewer.toggleWire(clkWire.id);
            }
        }
        if (e.key === ' ') {
            e.preventDefault();
            viewer.stepCircuit();
        }
        if (e.key.toLowerCase() === 'p') {
            if (btnPlayAnim) btnPlayAnim.click();
        }
    });
}

// MARK: - Tree Hierarchy UI
function syncTreeSelection(viewer) {
    const treeSelect = document.getElementById('treeSelect');
    
    const selectedIds = new Set();
    
    if (treeSelect && viewer.treeAreas) {
        // Find all nets that belong to the selected trees
        Array.from(treeSelect.selectedOptions).forEach(opt => {
            const treeId = parseInt(opt.value);
            if (viewer.treeToNets[treeId]) {
                viewer.treeToNets[treeId].forEach(netId => selectedIds.add(netId));
            }
        });
    }
    
    viewer.syncSelectedNets(Array.from(selectedIds));
}

export function updateTreeListUI(viewer) {
    const select = document.getElementById('treeSelect');
    const search = document.getElementById('treeSearch');
    if (!select || !viewer.treeAreas) return;

    const query = search ? search.value.toLowerCase() : "";
    const sortedTrees = Object.entries(viewer.treeAreas).sort((a, b) => b[1] - a[1]);

    const netToName = {};
    if (viewer.wireNames) {
        viewer.wireNames.forEach(w => {
            const isPower = /^(v(dd|ss|pwr|gnd)|vccd|vssd)\d*$/i.test(w.name);
            if (!netToName[w.id] || isPower) {
                netToName[w.id] = w.name;
            }
        });
    }

    let count = 0;
    const optionsHtml = [];
    for (const [treeStr, area] of sortedTrees) {
        const treeId = parseInt(treeStr);
        const name = netToName[treeId];
        const label = name ? `${name} (Tree ${treeId}) (Area: ${area.toLocaleString()})` : `Tree ${treeId} (Area: ${area.toLocaleString()})`;
        
        if (label.toLowerCase().includes(query)) {
            let selectedAttr = '';
            // Re-select if any net in this tree was highlighted
            if (viewer.netStateData && viewer.treeToNets[treeId]) {
                const anyNetHighlighted = Array.from(viewer.treeToNets[treeId]).some(netId => viewer.netStateData[netId] & 0x80);
                if (anyNetHighlighted) selectedAttr = 'selected';
            }
            optionsHtml.push(`<option value="${treeId}" ${selectedAttr}>${label}</option>`);
            if (++count >= 1000) break;
        }
    }
    select.innerHTML = optionsHtml.join('');
}

// MARK: - Context & Layers UI
export function bindLayerControls(viewer) {
    on('layerMinIdx', 'oninput', () => rebuildLayerUI(viewer));
    on('layerMaxIdx', 'oninput', () => rebuildLayerUI(viewer));
}

export function rebuildLayerUI(viewer) {
    const layerList = $('layerList');
    const minSlider = $('layerMinIdx');
    const maxSlider = $('layerMaxIdx');
    const rangeFill = $('layerRangeFill');
    if (!layerList || !minSlider || !maxSlider) return;

    // Filter out hidden FET layers (3=CHANNEL, 4=N_TERM) for the UI list
    // Sort ascending by Z so slider left=bottom, right=top
    const lids = viewer.sortedLids.filter(lid => lid != 3 && lid != 4)
        .sort((a, b) => (LAYER_CONFIG[a]?.z || 0) - (LAYER_CONFIG[b]?.z || 0));

    if (lids.length === 0) return;

    if (minSlider.max != lids.length - 1) {
        minSlider.min = 0;
        minSlider.max = lids.length - 1;
        minSlider.value = 0;
        maxSlider.min = 0;
        maxSlider.max = lids.length - 1;
        maxSlider.value = lids.length - 1;
    }

    const minIdx = parseInt(minSlider.value);
    const maxIdx = parseInt(maxSlider.value);
    const actualMin = Math.min(minIdx, maxIdx);
    const actualMax = Math.max(minIdx, maxIdx);

    if (lids.length > 1) {
        const percentMin = (actualMin / (lids.length - 1)) * 100;
        const percentMax = (actualMax / (lids.length - 1)) * 100;
        rangeFill.style.left = `${percentMin}%`;
        rangeFill.style.width = `${percentMax - percentMin}%`;
    }

    // Apply visibility
    for (const lid of viewer.sortedLids) {
        if (viewer.layers[lid]) viewer.layers[lid].visible = false;
    }

    for (let i = actualMin; i <= actualMax; i++) {
        const lid = lids[i];
        if (viewer.layers[lid]) viewer.layers[lid].visible = true;
        if (lid == 5) {
            if (viewer.layers[3]) viewer.layers[3].visible = true;
            if (viewer.layers[4]) viewer.layers[4].visible = true;
        }
    }

    // Sort descending for the UI badges display (top layer first)
    const displayLids = lids.slice().sort((a, b) => (LAYER_CONFIG[b]?.z || 0) - (LAYER_CONFIG[a]?.z || 0));

    layerList.innerHTML = displayLids.map(lid => {
        const isVisible = viewer.layers[lid].visible;
        const className = 'layer-badge' + (isVisible ? '' : ' hidden-layer');
        const config = LAYER_CONFIG[lid] || { name: "Layer " + lid, color: [0.5, 0.5, 0.5] };
        const c = config.color;
        const colorStr = `rgba(${c[0]*255}, ${c[1]*255}, ${c[2]*255}, 1)`;
        const name = lid == 5 ? "FET" : config.name;
        
        return `<div class="${className}" data-lid="${lid}">
            <span class="layer-color" style="background-color: ${colorStr};"></span>
            <span>${name}</span>
        </div>`;
    }).join('');

    viewer.requestFrame();
}

// MARK: - Simulation Monitor

function buildCircuitMonitorUI(viewer) {
    const monitor = document.getElementById('circuitMonitorContent');
    if (!monitor) return;
    
    if (viewer.wireNames.length === 0) {
        monitor.innerHTML = '<div class="waiting-text">No circuit data</div>';
        return;
    }

    const getPinDesc = (name, bitIdx = null) => {
        if (!viewer.metadata || !viewer.metadata.pinout) return '';
        const pinout = viewer.metadata.pinout;
        
        let key = name;
        if (bitIdx !== null) {
            // Normalize names like ui_in[0] -> ui[0]
            const base = name.replace(/_(in|out)$/, '');
            key = `${base}[${bitIdx}]`;
        }
        
        return pinout[key] || pinout[key.toLowerCase()] || '';
    };

    // Regex allows "{IO,UI,UO,UIO}_*", clk, ena, rst(_n), arrays with simple names (x[0..7], a[0..7])
    const importantRegex = /^(?:[iu]o(_in|_out)?|uio(_in|_out)?|clk|ena|rst_?n?|[a-z]{1,2})$/i;
    const isImportant = (name) => importantRegex.test(name) || /^(?:io|ui|uo|uio)_/i.test(name);

    // Filter and group signals
    const groupsMap = new Map();
    const singleBits = [];

    viewer.wireNames.forEach(w => {
        const nameUpper = w.name.toUpperCase();
        if (nameUpper === 'VDD' || nameUpper === 'VSS' || w.id <= 1) return;

        const m = w.name.match(/^(.+)\[(\d+)\]$/);
        if (m) {
            if (!groupsMap.has(m[1])) groupsMap.set(m[1], []);
            groupsMap.get(m[1])[parseInt(m[2])] = w;
        } else {
            singleBits.push(w);
        }
    });

    singleBits.sort((a, b) => a.name.localeCompare(b.name, undefined, {sensitivity: 'base', numeric: true}));

    const renderSingleBitsHTML = (wires, filterFunc) => {
        const filtered = wires.filter(w => filterFunc(w.name));
        if (!filtered.length) return '';
        return `<div class="monitor-special-row flex flex-wrap" style="margin-bottom: 4px; gap: 4px;">
            ${filtered.map(w => {
                const desc = getPinDesc(w.name);
                const tooltipAttr = desc ? ` data-tooltip="${desc}"` : '';
                return `<div class="monitor-special-btn"${tooltipAttr} data-wire-id="${w.id}">${w.name}</div>`;
            }).join('')}
        </div>`;
    };

    const specialMonitor = document.getElementById('monitorSpecialSignals');
    if (specialMonitor) specialMonitor.innerHTML = renderSingleBitsHTML(singleBits, isImportant);
    
    let importantHtml = '';
    let detailsHtml = renderSingleBitsHTML(singleBits, n => !isImportant(n));
    let hiddenCount = singleBits.filter(w => !isImportant(w.name)).length;

    // Show/Hide Seven Segment Display based on uo_out group presence
    const hasUoOut = Array.from(groupsMap.keys()).some(n => n.toLowerCase() === 'uo_out' || n.toLowerCase() === 'uo');
    const segContainer = document.getElementById('sevenSegDisplay');
    if (segContainer) segContainer.classList.toggle('hidden', !hasUoOut);

    // Show/Hide Auto Clock based on clk presence
    const hasClock = singleBits.some(w => w.name.toLowerCase() === 'clk');
    const autoClockBtn = document.getElementById('autoClockToggle');
    if (autoClockBtn) {
        const btnContainer = autoClockBtn.parentElement;
        if (btnContainer) btnContainer.classList.toggle('hidden', !hasClock);
    }
    
    // Hide the whole footer controls group if nothing is visible inside
    const controlsFooter = document.querySelector('.monitor-controls-group:last-child');
    if (controlsFooter) {
        controlsFooter.classList.toggle('hidden', !hasClock && !hasUoOut);
    }

    Array.from(groupsMap.keys()).sort().forEach(groupName => {
        const groupWires = groupsMap.get(groupName);
        const important = isImportant(groupName);
        const isTTPort = ['ui_in', 'uo_out', 'uio_in', 'uio_out', 'uo', 'ui', 'uio'].includes(groupName.toLowerCase());
        const count = Math.max(isTTPort ? 8 : 0, groupWires.length);
        if (!important) hiddenCount++;

        const wireIds = [];
        let gridHtml = `<div class="monitor-group-label" data-group-name="${groupName}">${groupName} <span class="monitor-hex-val"></span></div><div class="monitor-btn-grid">`;
        
        for (let i = 0; i < count; i++) {
            const wire = groupWires[i];
            const desc = getPinDesc(groupName, i);
            const tooltipAttr = desc ? ` data-tooltip="${desc}"` : '';
            if (wire) {
                gridHtml += `<div class="monitor-btn"${tooltipAttr} data-wire-id="${wire.id}">${i}</div>`;
                wireIds.push(wire.id);
            } else {
                gridHtml += `<div class="monitor-btn empty"${tooltipAttr}>${i}</div>`;
                wireIds.push(null);
            }
        }
        gridHtml += `</div>`;
        
        // Inject wireIds payload into the label node invisibly for caching later
        gridHtml = gridHtml.replace('>', ` data-wires='${JSON.stringify(wireIds)}'>`);

        if (important) importantHtml += gridHtml;
        else detailsHtml += gridHtml;
    });

    monitor.innerHTML = importantHtml + (hiddenCount > 0 ? `<details><summary>${hiddenCount} hidden signals...</summary>${detailsHtml}</details>` : '');

    // Event Delegation instead of 50 bound handlers
    const setupHandlers = (container) => {
        container.onclick = (e) => {
            const wireId = e.target.getAttribute('data-wire-id');
            if (wireId) viewer.toggleWire(parseInt(wireId));
        };

        container.onmouseover = (e) => {
            const text = e.target.getAttribute('data-tooltip');
            const tt = $('tooltip');
            if (text && tt) {
                tt.innerText = text;
                tt.style.display = 'block';
                const rect = e.target.getBoundingClientRect();
                tt.style.left = (rect.left + rect.width/2 - tt.offsetWidth/2) + 'px';
                tt.style.top = (rect.top - tt.offsetHeight - 8) + 'px';
                const ttRect = tt.getBoundingClientRect();
                if (ttRect.left < 5) tt.style.left = '5px';
                if (ttRect.right > window.innerWidth - 5) tt.style.left = (window.innerWidth - ttRect.width - 5) + 'px';
            }
        };

        container.onmouseout = () => {
            const tt = $('tooltip');
            if (tt) tt.style.display = 'none';
        };
    };

    setupHandlers(monitor);
    if (specialMonitor) setupHandlers(specialMonitor);

    on('circuitControls', 'classList', { remove: 'hidden' }); // Minimal sync
    const controlsUI = $('circuitControls');
    if (controlsUI) controlsUI.style.display = 'block';

    // Build the high-speed Cache Maps
    const allButtons = Array.from(monitor.querySelectorAll('[data-wire-id]'));
    if (specialMonitor) allButtons.push(...Array.from(specialMonitor.querySelectorAll('[data-wire-id]')));

    viewer._uiButtonNodes = allButtons.map(el => ({
        wireId: parseInt(el.getAttribute('data-wire-id')),
        el: el,
        isGroup: el.classList.contains('monitor-btn'),
        _lastRaw: -1
    }));

    viewer._uiGroupNodes = Array.from(monitor.querySelectorAll('[data-group-name]')).map(el => ({
        name: el.getAttribute('data-group-name'),
        valEl: el.querySelector('.monitor-hex-val'),
        wires: JSON.parse(el.getAttribute('data-wires')),
        _lastVal: -1n
    }));
}

function updateCircuitMonitorState(viewer, wireData) {
    const simStates = wireData || viewer.netStateData;
    if (!simStates || !viewer._uiButtonNodes || !viewer._uiGroupNodes) return;

    // We use netStateData as the master for metadata (flipped/highlight) 
    // and simStates for the live logic bits.
    const metaStates = viewer.netStateData;

    // 1. Update individual buttons
    for (let node of viewer._uiButtonNodes) {
        const id = node.wireId;
        const sim = simStates[id];
        const meta = metaStates[id];
        
        // Combine live state (bit 0) with UI metadata (bit 6: flipped, bit 7: highlight)
        const combined = (meta & 0xC0) | (sim & 1);
        
        if (node._lastRaw === combined) continue;
        node._lastRaw = combined;

        const state = combined & 1;
        const flipped = combined & 0x40;
        
        let target = node.isGroup ? 'monitor-btn' : 'monitor-special-btn';
        if (state === 1) target += ' state-high';
        else target += ' state-low';
        if (flipped) target += ' state-flipped';
        
        node.el.className = target;
    }

    // 2. Update multi-bit group labels
    for (let group of viewer._uiGroupNodes) {
        let val = 0n;
        const wires = group.wires;
        for (let i = 0; i < wires.length; i++) {
            const id = wires[i];
            if (id !== null && (simStates[id] & 1)) {
                val |= (1n << BigInt(i));
            }
        }
        
        if (group._lastVal === val) continue;
        group._lastVal = val;
        
        const hex = val.toString(16).toUpperCase().padStart(2, "0");
        const dec = val.toString().padStart(3, " ");
        group.valEl.textContent = `0x${hex} (${dec})`;

        // Update Seven Segment Display
        if (group.name.toLowerCase() === 'uo_out' || group.name.toLowerCase() === 'uo') {
            const segContainer = document.getElementById('sevenSegDisplay');
            if (segContainer) {
                for (let i = 0; i < 8; i++) {
                    const seg = document.getElementById(`7seg-${i}`);
                    if (seg) {
                        const on = (val >> BigInt(i)) & 1n;
                        seg.classList.toggle('on', on === 1n);
                    }
                }
            }
        }
    }
}

// MARK: - CRT / VGA View
export function updateVgaMonitor(viewer, {buffer, width, height, rayX, rayY, stride}) {
    const canvas = document.getElementById('vgaCanvas');
    const placeholder = document.getElementById('vgaPlaceholder');
    if (!canvas || !placeholder) return;

    if (!buffer) return;

    if (canvas.width !== width || canvas.height !== height || placeholder.style.display !== 'none') {
        canvas.width = width;
        canvas.height = height;
        
        const scale = canvas._vgaScale || 0.5;
        canvas.style.width = (width * scale) + 'px';
        canvas.style.height = (height * scale) + 'px';
        
        const content = document.getElementById('vgaMonitorContent');
        if (content) content.style.width = (width * scale) + 'px';
        
        canvas.classList.remove('hidden');
        placeholder.classList.add('hidden');
    }

    const ctx = canvas.getContext('2d');
    const imageData = ctx.createImageData(width, height);
    const data = imageData.data;

    // RGB (with stride) to Canvas (RGBA)
    const s = stride || width;
    for (let y = 0; y < height; y++) {
        for (let x = 0; x < width; x++) {
            const di = (y * width + x) * 4;
            const si = (y * s + x) * 3;
            data[di] = buffer[si];
            data[di+1] = buffer[si+1];
            data[di+2] = buffer[si+2];
            data[di+3] = 255;
        }
    }

    ctx.putImageData(imageData, 0, 0);

    // Draw ray spot
    if (typeof rayX === 'number' && typeof rayY === 'number') {
        ctx.fillStyle = '#f00';
        ctx.fillRect(rayX - 1, rayY - 4, 3, 9);
        ctx.fillRect(rayX - 4, rayY - 1, 9, 3);
    }
}

// MARK: - Utilities
function parseIdList(str) {
    const ids = new Set();
    const parts = str.split(/[,\s]+/);
    for (const part of parts) {
        if (!part.trim()) continue;
        if (part.includes('-')) {
            const [startStr, endStr] = part.split('-');
            const start = parseInt(startStr.trim());
            const end = parseInt(endStr.trim());
            if (!isNaN(start) && !isNaN(end)) {
                for (let i = Math.min(start, end); i <= Math.max(start, end); i++) {
                    ids.add(i);
                }
            }
        } else {
            const id = parseInt(part.trim());
            if (!isNaN(id)) {
                ids.add(id);
            }
        }
    }
    return Array.from(ids);
}

function updateWaypointListUI(animator, container) {
    if (!container) return;
    if (animator.waypoints.length === 0) {
        container.innerText = "0 Waypoints";
        return;
    }
    
    let html = `<b>${animator.waypoints.length} Waypoints</b><br>`;
    html += `Total Time: ${animator.totalDuration.toFixed(1)}s<br>`;
    container.innerHTML = html;
}
