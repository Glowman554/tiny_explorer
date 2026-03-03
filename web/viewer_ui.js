import { LAYER_CONFIG } from './circuit_viewer.js';
import { makeDraggable } from './draggable.js';
import { createStore } from './store.js';

/**
 * UI Glue Logic - Keeps the Viewer class pure but maintains functionality
 */
// MARK: - Main UI Initializer
export function initViewerUI(viewer) {
    const doc = (id) => document.getElementById(id);
    const logContainer = doc('logContainer');
    const loading = doc('loading');

    // MARK: Panel Toggles
    const togglePanelBtn = (btnId, panelId, defaultVisible = true) => {
        const btn = doc(btnId);
        const panel = doc(panelId);
        if (!btn || !panel) return;
        panel._isVisible = defaultVisible;
        const update = () => {
            panel.classList.toggle('hidden', !panel._isVisible);
            btn.classList.toggle('btn-active', panel._isVisible);
            btn.classList.toggle('btn-inactive', !panel._isVisible);
        };
        btn.onclick = () => {
            panel._isVisible = !panel._isVisible;
            update();
        };
        update();
    };

    togglePanelBtn('toggleUiBtn', 'ui', true);
    togglePanelBtn('toggleMonitorBtn', 'circuitMonitor', true);
    togglePanelBtn('toggleVgaBtn', 'vgaMonitor', false); // Hide VGA by default
    togglePanelBtn('toggleLogBtn', 'logPanel', true);

    const forceShowPanel = (btnId, panelId) => {
        const btn = doc(btnId);
        const panel = doc(panelId);
        if (btn && panel && !panel._isVisible) {
            panel._isVisible = true;
            panel.classList.remove('hidden');
            btn.classList.add('btn-active');
            btn.classList.remove('btn-inactive');
        }
    };
    
    const forceHidePanel = (btnId, panelId) => {
        const btn = doc(btnId);
        const panel = doc(panelId);
        if (btn && panel && panel._isVisible) {
            panel._isVisible = false;
            panel.classList.add('hidden');
            btn.classList.add('btn-inactive');
            btn.classList.remove('btn-active');
        }
    };

    // Close buttons
    if (doc('close-ui')) doc('close-ui').onclick = () => forceHidePanel('toggleUiBtn', 'ui');
    if (doc('close-circuitMonitor')) doc('close-circuitMonitor').onclick = () => forceHidePanel('toggleMonitorBtn', 'circuitMonitor');
    if (doc('close-vgaMonitor')) doc('close-vgaMonitor').onclick = () => forceHidePanel('toggleVgaBtn', 'vgaMonitor');
    if (doc('close-logPanel')) doc('close-logPanel').onclick = () => forceHidePanel('toggleLogBtn', 'logPanel');
    
    // Scale buttons
    const vgaCanvas = doc('vgaCanvas');
    const btnScale05 = doc('scale-vga-05');
    const btnScale10 = doc('scale-vga-10');
    
    // Store current scale globally attached to canvas node
    if (vgaCanvas) vgaCanvas._vgaScale = 0.5;
    
    const setVgaScale = (scale) => {
        if (!vgaCanvas) return;
        vgaCanvas._vgaScale = scale;
        
        // Refresh canvas visual CSS scaling immediately 
        vgaCanvas.style.width = (vgaCanvas.width * scale) + 'px';
        vgaCanvas.style.height = (vgaCanvas.height * scale) + 'px';
        
        // Ensure the panel container stays tight around the scaled canvas
        const panelContent = doc('vgaMonitorContent');
        if (panelContent) {
            // We set the container width to match the scaled canvas
            // This keeps the parent panel tight
            panelContent.style.width = (vgaCanvas.width * scale) + 'px';
        }
        
        if (scale === 0.5) {
            if (btnScale05) { btnScale05.classList.add('btn-active'); btnScale05.classList.remove('btn-inactive'); }
            if (btnScale10) { btnScale10.classList.add('btn-inactive'); btnScale10.classList.remove('btn-active'); }
        } else {
            if (btnScale05) { btnScale05.classList.add('btn-inactive'); btnScale05.classList.remove('btn-active'); }
            if (btnScale10) { btnScale10.classList.add('btn-active'); btnScale10.classList.remove('btn-inactive'); }
        }
    };
    
    // Default to 0.5x visually
    setVgaScale(0.5);
    
    if (btnScale05) btnScale05.onclick = () => setVgaScale(0.5);
    if (btnScale10) btnScale10.onclick = () => setVgaScale(1.0);


    // MARK: Panel Draggability

    makeDraggable('ui');
    makeDraggable('circuitMonitor');
    makeDraggable('vgaMonitor');
    makeDraggable('logPanel');

    // MARK: Engine Callbacks
    viewer.onLog = (msg) => {
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
        const rayLabel = document.getElementById('vgaRayPos');
        if (rayLabel) rayLabel.innerText = `Ray: X:${vga.rayX} Y:${vga.rayY}`;
    };
    
    if (doc('btnReset')) doc('btnReset').onclick = () => viewer.resetView();
    if (doc('btnDownload')) doc('btnDownload').onclick = async () => {
        if (!viewer.currentUrl) return;
        const btn = doc('btnDownload');
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
    };

}

// MARK: - Sub-Controllers

export function bindViewControls(viewer) {
    const doc = (id) => document.getElementById(id);
    const alphaSlider = doc('alphaSlider');
    const boundaryToggle = doc('boundaryToggle');
    const perspSlider = doc('perspSlider');
    const explodeSlider = doc('explodeSlider');
    const powerNetToggle = doc('powerNetToggle');
    const stateMixSlider = doc('stateMixSlider');
    const renderModeSelect = doc('renderModeSelect');

    if (alphaSlider) alphaSlider.oninput = () => {
        viewer.view.globalAlpha = parseFloat(alphaSlider.value);
        viewer.requestFrame();
    };
    if (boundaryToggle) boundaryToggle.onchange = () => {
        viewer.view.showBoundaries = boundaryToggle.checked;
        viewer.requestFrame();
    };
    if (perspSlider) perspSlider.oninput = () => {
        viewer.view.perspective = parseFloat(perspSlider.value);
        viewer.requestFrame();
    };
    if (explodeSlider) explodeSlider.oninput = () => {
        viewer.view.explode = parseFloat(explodeSlider.value);
        viewer.requestFrame();
    };
    if (powerNetToggle) powerNetToggle.onchange = () => {
        viewer.view.showPowerNets = powerNetToggle.checked;
        viewer.requestFrame();
    };
    if (stateMixSlider) stateMixSlider.oninput = () => {
        viewer.view.stateMix = parseFloat(stateMixSlider.value);
        viewer.requestFrame();
    };
    if (renderModeSelect) renderModeSelect.onchange = () => {
        viewer.setRenderMode(renderModeSelect.value);
    };
}

export function bindAnimationControls(viewer) {
    const doc = (id) => document.getElementById(id);
    const btnCaptWp = doc('btnCaptWp');
    const btnClearWp = doc('btnClearWp');
    const btnPlayAnim = doc('btnPlayAnim');
    const animTime = doc('animTime');
    const wpList = doc('wpList');

    if (btnCaptWp && viewer.animator) {
        btnCaptWp.onclick = () => {
            if (animTime) {
                viewer.animator.defaultTransitionTime = parseFloat(animTime.value);
            }
            viewer.animator.addWaypoint(viewer);
            updateWaypointListUI(viewer.animator, wpList);
        };
    }

    if (btnClearWp && viewer.animator) {
        btnClearWp.onclick = () => {
            viewer.animator.clearWaypoints();
            if (btnPlayAnim) {
                btnPlayAnim.innerText = 'Play Sequence';
                btnPlayAnim.classList.remove('btn-danger');
                btnPlayAnim.classList.add('btn-success');
            }
            updateWaypointListUI(viewer.animator, wpList);
        };
    }

    if (btnPlayAnim && viewer.animator) {
        btnPlayAnim.onclick = () => {
            if (viewer.animator.isPlaying) {
                viewer.animator.stop();
                btnPlayAnim.innerText = 'Play Sequence';
                btnPlayAnim.style.background = '#284';
                viewer.requestFrame();
            } else {
                if (viewer.animator.waypoints.length > 1) {
                    const duration = animTime ? parseFloat(animTime.value) : 2.0;
                    viewer.animator.play(true, duration); // Auto-loop continuously
                    btnPlayAnim.innerText = 'Stop Sequence';
                    btnPlayAnim.classList.remove('btn-success');
                    btnPlayAnim.classList.add('btn-danger');
                    viewer.requestFrame();
                }
            }
        };
    }
}

export function bindSimulationControls(viewer) {
    const doc = (id) => document.getElementById(id);
    const simSpeed = doc('simSpeed');
    const simSpeedLabel = doc('simSpeedLabel');
    const autoClockToggle = doc('autoClockToggle');
    const btnPlayPause = doc('btnPlayPause');
    const stateMixSlider = doc('stateMixSlider');

    // UI Local State
    // 1. Define Reactive State
    const Store = createStore({
        isPaused: true,
        simSpeed: simSpeed ? parseInt(simSpeed.value, 10) : 50,
        autoClock: autoClockToggle ? autoClockToggle.checked : false
    });

    // 2. Declarative DOM Bindings (Run automatically whenever state changes)
    Store.bind('isPaused', (paused) => {
        if (btnPlayPause) {
            btnPlayPause.innerText = paused ? '▶' : '⏸';
            btnPlayPause.classList.toggle('btn-inactive', paused);
            btnPlayPause.classList.toggle('btn-active', !paused);
        }
        if (simSpeedLabel) {
            const speed = Store.get().simSpeed;
            simSpeedLabel.innerText = paused ? 'PAUSED' : (speed === 100 ? 'MAX' : `${speed}%`);
            simSpeedLabel.classList.toggle('btn-inactive', paused);
            simSpeedLabel.classList.toggle('btn-active', !paused);
        }
    });

    Store.bind('simSpeed', (speed) => {
        if (simSpeedLabel && !Store.get().isPaused) {
            simSpeedLabel.innerText = speed === 100 ? 'MAX' : `${speed}%`;
        }
    });

    // 3. Engine Sync (Sync our reactive state back to the C++ core)
    Store.subscribe((state) => {
        if (!viewer.isLoaded) return;
        const actualSpeed = state.isPaused ? 0 : state.simSpeed;
        viewer.setSimConfig(actualSpeed, state.isPaused ? false : state.autoClock);
    });

    // 4. Pure Input Mutation Handling
    if (btnPlayPause) {
        btnPlayPause.onclick = () => {
            const nextPaused = !Store.get().isPaused;
            if (!nextPaused && stateMixSlider) {
                stateMixSlider.value = 0.75;
                viewer.view.stateMix = 0.75;
                viewer.requestFrame();
            }
            Store.set({ isPaused: nextPaused });
        };
    }

    if (simSpeed) {
        simSpeed.oninput = () => {
            if (Store.get().isPaused) {
                Store.set({ isPaused: false, simSpeed: parseInt(simSpeed.value, 10) });
                if (stateMixSlider) {
                    stateMixSlider.value = 0.75;
                    viewer.view.stateMix = 0.75;
                    viewer.requestFrame();
                }
            } else {
                Store.set({ simSpeed: parseInt(simSpeed.value, 10) });
            }
        };
    }
    
    if (autoClockToggle) {
        autoClockToggle.onchange = () => Store.set({ autoClock: autoClockToggle.checked });
    }
}

export function bindTreeInspectorUI(viewer) {
    const doc = (id) => document.getElementById(id);
    const treeSearch = doc('treeSearch');
    const treeSelect = doc('treeSelect');
    const treeIdList = doc('treeIdList');
    const btnHighlightIds = doc('btnHighlightIds');
    const btnClearTrees = doc('btnClearTrees');

    if (treeSearch) {
        treeSearch.oninput = () => updateTreeListUI(viewer);
        treeSearch.onkeydown = (e) => {
            if (e.key === 'ArrowDown' && treeSelect && treeSelect.options.length > 0) {
                e.preventDefault();
                treeSelect.focus();
                if (treeSelect.selectedIndex === -1) {
                    treeSelect.options[0].selected = true;
                    syncTreeSelection(viewer);
                }
            }
        };
    }

    if (treeSelect) {
        treeSelect.onchange = treeSelect.oninput = treeSelect.onkeyup = treeSelect.onclick = () => syncTreeSelection(viewer);
    }

    if (btnHighlightIds) {
        btnHighlightIds.onclick = () => {
            if (!treeIdList) return;
            const ids = parseIdList(treeIdList.value);
            // Translate Tree IDs to Net IDs since highlight sync operates via nets internally under the hood
            const selectedNets = new Set();
            ids.forEach(treeId => {
                 if (viewer.treeToNets[treeId]) {
                      viewer.treeToNets[treeId].forEach(netId => selectedNets.add(netId));
                 }
            });
            viewer.syncSelectedNets(Array.from(selectedNets));
            updateTreeListUI(viewer);
        };
    }

    if (treeIdList) {
        treeIdList.onkeydown = (e) => {
            if (e.key === 'Enter') {
                btnHighlightIds.click();
            }
        };
    }

    if (btnClearTrees) {
        btnClearTrees.onclick = () => {
            if (treeSelect) treeSelect.selectedIndex = -1;
            if (treeSearch) treeSearch.value = '';
            if (treeIdList) treeIdList.value = '';
            viewer.syncSelectedNets([]);
            updateTreeListUI(viewer);
        };
    }
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
    const minSlider = document.getElementById('layerMinIdx');
    const maxSlider = document.getElementById('layerMaxIdx');

    if (minSlider) minSlider.oninput = () => rebuildLayerUI(viewer);
    if (maxSlider) maxSlider.oninput = () => rebuildLayerUI(viewer);
}

export function rebuildLayerUI(viewer) {
    const layerList = document.getElementById('layerList');
    const minSlider = document.getElementById('layerMinIdx');
    const maxSlider = document.getElementById('layerMaxIdx');
    const rangeFill = document.getElementById('layerRangeFill');
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
        return `<div class="monitor-special-row flex flex-wrap">
            ${filtered.map(w => `<div class="monitor-special-btn" data-wire-id="${w.id}">${w.name}</div>`).join('')}
        </div>`;
    };

    let importantHtml = renderSingleBitsHTML(singleBits, isImportant);
    let detailsHtml = renderSingleBitsHTML(singleBits, n => !isImportant(n));
    let hiddenCount = singleBits.filter(w => !isImportant(w.name)).length;

    Array.from(groupsMap.keys()).sort().forEach(groupName => {
        const groupWires = groupsMap.get(groupName);
        const important = isImportant(groupName);
        const isTTPort = ['ui_in', 'uo_out', 'uio_in', 'uio_out', 'uo', 'ui', 'uio'].includes(groupName.toLowerCase());
        const count = Math.max(isTTPort ? 8 : 0, groupWires.length);
        if (!important) hiddenCount++;

        const wireIds = [];
        let gridHtml = `<div class="monitor-group-label" data-group-name="${groupName}"></div><div class="monitor-btn-grid">`;
        
        for (let i = 0; i < count; i++) {
            const wire = groupWires[i];
            if (wire) {
                gridHtml += `<div class="monitor-btn" data-wire-id="${wire.id}">${i}</div>`;
                wireIds.push(wire.id);
            } else {
                gridHtml += `<div class="monitor-btn empty">${i}</div>`;
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
    monitor.onclick = (e) => {
        const wireId = e.target.getAttribute('data-wire-id');
        if (wireId) viewer.toggleWire(parseInt(wireId));
    };

    const controlsUI = document.getElementById('circuitControls');
    if (controlsUI) {
        controlsUI.classList.remove('hidden');
        controlsUI.style.display = 'block'; // Fallback for any other logic checking display
    }

    // Build the high-speed Cache Maps
    viewer._uiButtonNodes = Array.from(monitor.querySelectorAll('[data-wire-id]')).map(el => ({
        wireId: parseInt(el.getAttribute('data-wire-id')),
        el: el,
        isGroup: el.classList.contains('monitor-btn')
    }));

    viewer._uiGroupNodes = Array.from(monitor.querySelectorAll('[data-group-name]')).map(el => ({
        name: el.getAttribute('data-group-name'),
        labelEl: el,
        wires: JSON.parse(el.getAttribute('data-wires'))
    }));
}

function updateCircuitMonitorState(viewer, wireData) {
    if (!viewer._uiButtonNodes || !viewer._uiGroupNodes) return;

    const getFullState = (id) => {
        return (wireData) ? wireData[id] : (viewer.netStateData ? viewer.netStateData[id] : 0);
    };

    // Fast-path: Update all buttons via bound DOM nodes
    for (let node of viewer._uiButtonNodes) {
        const raw = getFullState(node.wireId);
        const state = raw & 1;
        
        let targetClass = node.isGroup ? 'monitor-btn' : 'monitor-special-btn';
        if (state === 1) targetClass += ' state-high';
        else targetClass += ' state-low';
        if (raw & 0x40) targetClass += ' state-flipped';
        
        if (node.el.className !== targetClass) {
            node.el.className = targetClass;
        }
    }

    // Fast-path: Update group hex/dec text labels dynamically
    for (let group of viewer._uiGroupNodes) {
        let val = 0n;
        for (let i = 0; i < group.wires.length; i++) {
            const wireId = group.wires[i];
            if (wireId !== null) {
                const bit = BigInt(getFullState(wireId) & 1);
                val |= (bit << BigInt(i));
            }
        }
        
        const hex = val.toString(16).toUpperCase().padStart(2, "0");
        const dec = val.toString().padStart(3, " ");
        
        // Fast DOM write
        const content = `${group.name} <span class="monitor-hex-val">0x${hex} (${dec})</span>`;
        if (group.labelEl.innerHTML !== content) {
            group.labelEl.innerHTML = content;
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
