import { LAYER_CONFIG } from './circuit_viewer.js';

/**
 * UI Glue Logic - Keeps the Viewer class pure but maintains functionality
 */
// MARK: - Main UI Initializer
export function initViewerUI(viewer) {
    const doc = (id) => document.getElementById(id);
    const logContainer = doc('logContainer');
    const logPanel = doc('logPanel');
    const loading = doc('loading');
    const treeSelect = doc('treeSelect');
    const treeSearch = doc('treeSearch');
    const treeIdList = doc('treeIdList');
    const alphaSlider = doc('alphaSlider');
    const boundaryToggle = doc('boundaryToggle');
    const powerNetToggle = doc('powerNetToggle');
    const btnHighlightIds = doc('btnHighlightIds');

    // MARK: Panel Toggles
    const togglePanelBtn = (btnId, panelId, defaultVisible = true) => {
        const btn = doc(btnId);
        const panel = doc(panelId);
        if (!btn || !panel) return;
        panel._isVisible = defaultVisible;
        const update = () => {
            panel.style.display = panel._isVisible ? 'flex' : 'none';
            btn.style.color = panel._isVisible ? '#0f0' : '#888';
            btn.style.borderColor = panel._isVisible ? '#0f0' : '#444';
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
            panel.style.display = 'flex';
            btn.style.color = '#0f0';
            btn.style.borderColor = '#0f0';
        }
    };
    
    const forceHidePanel = (btnId, panelId) => {
        const btn = doc(btnId);
        const panel = doc(panelId);
        if (btn && panel && panel._isVisible) {
            panel._isVisible = false;
            panel.style.display = 'none';
            btn.style.color = '#888';
            btn.style.borderColor = '#444';
        }
    };

    // Close buttons
    if (doc('close-ui')) doc('close-ui').onclick = () => forceHidePanel('toggleUiBtn', 'ui');
    if (doc('close-circuitMonitor')) doc('close-circuitMonitor').onclick = () => forceHidePanel('toggleMonitorBtn', 'circuitMonitor');
    if (doc('close-vgaMonitor')) doc('close-vgaMonitor').onclick = () => forceHidePanel('toggleVgaBtn', 'vgaMonitor');
    if (doc('close-logPanel')) doc('close-logPanel').onclick = () => forceHidePanel('toggleLogBtn', 'logPanel');
    
    // Scale buttons
    const vgaCanvas = doc('vgaCanvas');
    const vgaPanelContent = doc('vgaMonitorContent');
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
            panelContent.style.width = (vgaCanvas.width * scale + 24) + 'px'; 
        }
        
        if (scale === 0.5) {
            if (btnScale05) btnScale05.style.color = '#0f0';
            if (btnScale10) btnScale10.style.color = '#888';
        } else {
            if (btnScale05) btnScale05.style.color = '#888';
            if (btnScale10) btnScale10.style.color = '#0f0';
        }
    };
    
    // Default to 0.5x visually
    setVgaScale(0.5);
    
    if (btnScale05) btnScale05.onclick = () => setVgaScale(0.5);
    if (btnScale10) btnScale10.onclick = () => setVgaScale(1.0);


    // MARK: Panel Draggability
    let highestZIndex = 100;
    const bringToFront = (panel) => {
        highestZIndex++;
        panel.style.zIndex = highestZIndex;
    };

    const makeDraggable = (panelId) => {
        const panel = doc(panelId);
        if (!panel) return;
        
        panel.addEventListener('mousedown', () => bringToFront(panel));

        const header = panel.querySelector('.panel-header');
        if (!header) return;

        let isDragging = false;
        let startX, startY;
        let currentX, currentY;

        header.addEventListener('mousedown', (e) => {
            if (e.button !== 0) return; // Only left click
            isDragging = true;
            
            const rect = panel.getBoundingClientRect();
            
            // Clear right/bottom CSS constraints if set, replacing with explicit left/top
            if (panel.style.right !== '' || !panel.style.left) {
                panel.style.left = rect.left + 'px';
                panel.style.right = 'auto';
            }
            if (panel.style.bottom !== '' || !panel.style.top) {
                panel.style.top = rect.top + 'px';
                panel.style.bottom = 'auto';
            }
            
            currentX = parseFloat(panel.style.left) || rect.left;
            currentY = parseFloat(panel.style.top) || rect.top;
            startX = e.clientX;
            startY = e.clientY;
            
            document.addEventListener('mousemove', onMouseMove);
            document.addEventListener('mouseup', onMouseUp);
            e.preventDefault();
        });

        const onMouseMove = (e) => {
            if (!isDragging) return;
            const dx = e.clientX - startX;
            const dy = e.clientY - startY;
            panel.style.left = (currentX + dx) + 'px';
            panel.style.top = (currentY + dy) + 'px';
        };

        const onMouseUp = () => {
            isDragging = false;
            document.removeEventListener('mousemove', onMouseMove);
            document.removeEventListener('mouseup', onMouseUp);
        };
    };

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
            loading.style.display = 'block';
        }
        forceShowPanel('toggleLogBtn', 'logPanel');
    };

    viewer.onLoaded = () => {
        if (loading) loading.style.display = 'none';
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

    viewer.onUpdateCircuit = (wireData) => {
        updateCircuitMonitorState(viewer, wireData);
    };

    viewer.onVgaFrame = (buffer, width, height) => {
        updateVgaMonitor(viewer, buffer, width, height);
    };

    // Toggle logic for all panels has been moved to top menu

    // Event listeners for viewer state
    // MARK: View Controls
    if (alphaSlider) alphaSlider.oninput = () => {
        viewer.view.globalAlpha = parseFloat(alphaSlider.value);
        viewer.requestFrame();
    };
    if (boundaryToggle) boundaryToggle.onchange = () => {
        viewer.view.showBoundaries = boundaryToggle.checked;
        viewer.requestFrame();
    };
    const perspSlider = doc('perspSlider');
    if (perspSlider) perspSlider.oninput = () => {
        viewer.view.perspective = parseFloat(perspSlider.value);
        viewer.requestFrame();
    };
    const explodeSlider = doc('explodeSlider');
    if (explodeSlider) explodeSlider.oninput = () => {
        viewer.view.explode = parseFloat(explodeSlider.value);
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
    const renderModeSelect = doc('renderModeSelect');
    if (renderModeSelect) renderModeSelect.onchange = () => {
        viewer.setRenderMode(renderModeSelect.value);
    };

    // MARK: Simulation Controls
    const simSpeed = doc('simSpeed');
    const simSpeedLabel = doc('simSpeedLabel');
    const autoClockToggle = doc('autoClockToggle');
    const btnPlayPause = doc('btnPlayPause');

    let isPaused = true;

    const updateSimConfig = () => {
        if (!viewer.isLoaded) return;
        const speedVal = parseInt(simSpeed.value, 10);
        const autoClk = autoClockToggle.checked;
        const speed = isPaused ? 0 : speedVal;
        
        if (isPaused) {
            if (btnPlayPause) {
                btnPlayPause.innerText = '▶';
                btnPlayPause.style.color = '#aa0';
            }
            if (simSpeedLabel) {
                simSpeedLabel.innerText = 'PAUSED';
                simSpeedLabel.style.color = '#aa0';
            }
        } else {
            if (btnPlayPause) {
                btnPlayPause.innerText = '⏸';
                btnPlayPause.style.color = '#0f0';
            }
            if (simSpeedLabel) {
                simSpeedLabel.style.color = '#0f0';
                if (speedVal === 100) simSpeedLabel.innerText = 'MAX';
                else simSpeedLabel.innerText = `${speedVal}%`;
            }
        }
        
        viewer.setSimConfig(speed, isPaused ? false : autoClk);
    };

    if (btnPlayPause) {
        btnPlayPause.onclick = () => {
            isPaused = !isPaused;
            if (!isPaused && stateMixSlider) {
                stateMixSlider.value = 0.75;
                viewer.view.stateMix = 0.75;
                viewer.requestFrame();
            }
            updateSimConfig();
        };
    }

    if (simSpeed) simSpeed.oninput = () => {
        if (isPaused) {
            isPaused = false;
            if (stateMixSlider) {
                stateMixSlider.value = 0.75;
                viewer.view.stateMix = 0.75;
                viewer.requestFrame();
            }
        }
        updateSimConfig();
    };
    if (autoClockToggle) autoClockToggle.onchange = updateSimConfig;
    
    // The worker now drives VGA updates and ray properties, remove UI driving
    viewer.onVgaFrame = (buffer, width, height, rayX, rayY) => {
        updateVgaMonitor(viewer, buffer, width, height, rayX, rayY);
        const rayLabel = document.getElementById('vgaRayPos');
        if (rayLabel) rayLabel.innerText = `Ray: X:${rayX} Y:${rayY}`;
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

    const layerMinIdx = doc('layerMinIdx');
    if (layerMinIdx) layerMinIdx.oninput = () => rebuildLayerUI(viewer);
    const layerMaxIdx = doc('layerMaxIdx');
    if (layerMaxIdx) layerMaxIdx.oninput = () => rebuildLayerUI(viewer);

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

    // Animator Integrations
    // MARK: Animation Controls
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
                btnPlayAnim.style.background = '#284';
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
                    btnPlayAnim.style.background = '#822';
                    viewer.requestFrame();
                }
            }
        };
    }

    if (doc('btnClearTrees')) doc('btnClearTrees').onclick = () => {
        if (treeSelect) treeSelect.selectedIndex = -1;
        if (treeSearch) treeSearch.value = '';
        if (treeIdList) treeIdList.value = '';
        viewer.syncSelectedNets([]);
        updateTreeListUI(viewer);
    };

    // MARK: Keyboard Integrations
    window.addEventListener('keydown', (e) => {
        if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
            const dir = e.key === 'ArrowDown' ? 1 : -1;
            if (viewer.soloLayerId !== null) {
                cycleSoloLayer(viewer, dir);
            } else if (treeSelect && treeSelect.options.length > 0) {
                e.preventDefault();
                treeSelect.selectedIndex = Math.max(0, Math.min(treeSelect.selectedIndex + dir, treeSelect.options.length - 1));
                syncTreeSelection(viewer);
            }
        }
        if (e.key === 'Escape') {
            viewer.setSoloLayer(null);
            rebuildLayerUI(viewer);
        }
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

    updateSimConfig();
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

    select.innerHTML = '';
    let count = 0;
    for (const [treeStr, area] of sortedTrees) {
        const treeId = parseInt(treeStr);
        const name = netToName[treeId];
        const label = name ? `${name} (Tree ${treeId}) (Area: ${area.toLocaleString()})` : `Tree ${treeId} (Area: ${area.toLocaleString()})`;
        
        if (label.toLowerCase().includes(query)) {
            const opt = document.createElement('option');
            opt.value = treeId;
            opt.innerText = label;
            
            // Re-select if any net in this tree was highlighted
            if (viewer.netStateData && viewer.treeToNets[treeId]) {
                const anyNetHighlighted = Array.from(viewer.treeToNets[treeId]).some(netId => viewer.netStateData[netId] & 0x80);
                if (anyNetHighlighted) opt.selected = true;
            }
            
            select.appendChild(opt);
            if (++count >= 1000) break;
        }
    }
}

// MARK: - Context & Layers UI
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

    layerList.innerHTML = '';
    
    // Sort descending for the UI badges display (top layer first)
    const displayLids = lids.slice().sort((a, b) => (LAYER_CONFIG[b]?.z || 0) - (LAYER_CONFIG[a]?.z || 0));

    displayLids.forEach(lid => {
        const div = document.createElement('div');
        div.className = 'layer-badge' + (viewer.layers[lid].visible ? '' : ' hidden-layer');
        const config = LAYER_CONFIG[lid] || { name: "Layer " + lid, color: [0.5, 0.5, 0.5] };

        const c = config.color;
        const s = document.createElement('span');
        s.className = 'layer-color';
        s.style.backgroundColor = `rgba(${c[0]*255}, ${c[1]*255}, ${c[2]*255}, 1)`;
        
        const text = document.createElement('span');
        text.innerText = lid == 5 ? "FET" : config.name;
        
        div.appendChild(s);
        div.appendChild(text);

        div.onclick = () => {
            viewer.setSoloLayer(viewer.soloLayerId === lid ? null : lid);
            rebuildLayerUI(viewer);
        };

        if (viewer.soloLayerId !== null) {
            const isActive = (lid == 5);
            const isSoloActive = (viewer.soloLayerId == 3 || viewer.soloLayerId == 4 || viewer.soloLayerId == 5);
            const match = (isActive && isSoloActive) || (String(lid) === String(viewer.soloLayerId));
            div.style.opacity = match ? '1.0' : '0.3';
            if (match) div.classList.remove('hidden-layer');
        }

        layerList.appendChild(div);
    });

    viewer.requestFrame();
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

// MARK: - Simulation Monitor

function buildCircuitMonitorUI(viewer) {
    const monitor = document.getElementById('circuitMonitorContent');
    if (!monitor) return;
    
    if (viewer.wireNames.length === 0) {
        monitor.innerHTML = '<div style="padding:10px; color:#666;">No circuit data</div>';
        return;
    }

    // Filter and group signals (Only done once on load)
    const groupPattern = /^(.+)\[(\d+)\]$/;
    const groupsMap = new Map();
    const singleBits = [];

    viewer.wireNames.forEach(w => {
        const nameUpper = w.name.toUpperCase();
        if (nameUpper === 'VDD' || nameUpper === 'VSS') return;
        if (w.id === 0 || w.id === 1) return;

        const m = w.name.match(groupPattern);
        if (m) {
            const groupName = m[1];
            const idx = parseInt(m[2]);
            if (!groupsMap.has(groupName)) groupsMap.set(groupName, []);
            groupsMap.get(groupName)[idx] = w;
        } else {
            singleBits.push(w);
        }
    });

    singleBits.sort((a, b) => a.name.localeCompare(b.name, undefined, {sensitivity: 'base', numeric: true}));

    monitor.innerHTML = '';
    
    const controlsUI = document.getElementById('circuitControls');
    if (controlsUI) controlsUI.style.display = 'block';

    // Store a map of how to fast-update groups on the DOM parent
    viewer._uiGroupNodes = [];
    viewer._uiButtonNodes = []; // list of {id, element}

    // 1. Single Bits Row
    if (singleBits.length > 0) {
        let specialRow = document.createElement('div');
        specialRow.className = 'monitor-special-row';
        specialRow.style.flexWrap = 'wrap';
        monitor.appendChild(specialRow);
        
        singleBits.forEach(wire => {
            let btn = document.createElement('div');
            btn.className = 'monitor-special-btn';
            btn.innerText = wire.name;
            btn.onclick = () => {
                viewer.toggleWire(wire.id);
            };
            specialRow.appendChild(btn);
            viewer._uiButtonNodes.push({ wireId: wire.id, el: btn, isGroup: false });
        });
    }

    // 2. Groups
    const sortedGroupNames = Array.from(groupsMap.keys()).sort();
    sortedGroupNames.forEach(groupName => {
        const groupWires = groupsMap.get(groupName);
        
        let label = document.createElement('div');
        label.className = 'monitor-group-label';
        monitor.appendChild(label);

        let grid = document.createElement('div');
        grid.className = 'monitor-grid';
        monitor.appendChild(grid);
        
        const isTTPort = ['ui_in', 'uo_out', 'uio_in', 'uio_out'].includes(groupName);
        const count = Math.max(isTTPort ? 8 : 0, groupWires.length);

        const groupDefinition = {
            name: groupName,
            labelEl: label,
            wires: [] // store wire IDs matching indices
        };

        for (let i = 0; i < count; i++) {
            const wire = groupWires[i];
            const btn = document.createElement('div');
            btn.className = 'monitor-btn';
            btn.innerText = i;
            
            if (wire) {
                btn.onclick = () => {
                    viewer.toggleWire(wire.id);
                };
                viewer._uiButtonNodes.push({ wireId: wire.id, el: btn, isGroup: true });
                groupDefinition.wires.push(wire.id);
            } else {
                btn.style.opacity = '0.15';
                btn.style.cursor = 'default';
                btn.style.color = '#333';
                groupDefinition.wires.push(null);
            }
            grid.appendChild(btn);
        }
        
        viewer._uiGroupNodes.push(groupDefinition);
    });
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
        const content = `${group.name} <span style="font-family:monospace; white-space: pre; color:#aaa; margin-left:8px; text-transform:none; font-size:1.5em;">0x${hex} (${dec})</span>`;
        if (group.labelEl.innerHTML !== content) {
            group.labelEl.innerHTML = content;
        }
    }
}

// MARK: - CRT / VGA View
export function updateVgaMonitor(viewer, buffer, width, height, rayX, rayY) {
    const canvas = document.getElementById('vgaCanvas');
    const placeholder = document.getElementById('vgaPlaceholder');
    if (!canvas || !placeholder) return;

    if (!buffer) return;

    if (canvas.width !== width || canvas.height !== height || placeholder.style.display !== 'none') {
        // Only trigger layout updates if the native internal buffer dimensions change or if we haven't shown the canvas yet
        canvas.width = width;
        canvas.height = height;
        
        // Respect dynamic scale logic visually
        const scale = canvas._vgaScale || 0.5;
        canvas.style.width = (width * scale) + 'px';
        canvas.style.height = (height * scale) + 'px';
        
        canvas.style.display = 'block';
        placeholder.style.display = 'none';
    }

    const ctx = canvas.getContext('2d');
    const imageData = ctx.createImageData(width, height);
    const data = imageData.data;

    // PPM (RGB) to Canvas (RGBA)
    for (let i = 0, j = 0; i < data.length; i += 4, j += 3) {
        data[i] = buffer[j];
        data[i+1] = buffer[j+1];
        data[i+2] = buffer[j+2];
        data[i+3] = 255;
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
