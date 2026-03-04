import { CircuitViewer } from './circuit_viewer.js';
import { initViewerUI } from './viewer_ui.js';

// Entry point
window.addEventListener('DOMContentLoaded', () => {
    const viewer = new CircuitViewer('glcanvas');
    window.viewer = viewer; // for debugging/global access
    
    initViewerUI(viewer);

    const params = new URLSearchParams(window.location.search);
    const isLocal = params.get('local') === '1';
    const gds = params.get('file') || params.get('gds');
    const pdk = params.get('pdk') || '';
    const metadataUrl = params.get('metadata');
    
    if (metadataUrl) {
        fetch(metadataUrl)
            .then(res => res.json())
            .then(data => {
                const allStrings = [];
                const searchStrings = (obj) => {
                    if (!obj) return;
                    if (typeof obj === 'string') {
                        allStrings.push(obj.toLowerCase());
                    } else if (Array.isArray(obj)) {
                        obj.forEach(searchStrings);
                    } else if (typeof obj === 'object') {
                        Object.values(obj).forEach(searchStrings);
                    }
                };
                searchStrings(data.project?.pins || data.pins || data.pinout || data.yaml?.pinout);

                const hasVGA = allStrings.some(p => p.includes('hsync') || p.includes('vsync') || p.includes('vga'));
                if (hasVGA) {
                    const vgaBtn = document.getElementById('toggleVgaBtn');
                    if (vgaBtn && !vgaBtn.classList.contains('btn-active')) {
                        vgaBtn.click();
                    }
                }
            })
            .catch(err => console.warn("Failed to fetch project metadata:", err));
    }

    // Create a hidden file input for local uploads
    const localInput = document.createElement('input');
    localInput.type = 'file';
    localInput.accept = '.gds,.gds.br,.gds.gz,.oas';
    localInput.style.display = 'none';
    document.body.appendChild(localInput);

    localInput.onchange = (e) => {
        const file = e.target.files[0];
        if (!file) return;
        const url = URL.createObjectURL(file);
        
        document.title = `Tiny Explorer - ${file.name}`;
        
        const loading = document.getElementById('loading');
        if (loading) {
            loading.style.pointerEvents = 'none'; // reset
        }
        
        const btnDownload = document.getElementById('btnDownload');
        if (btnDownload) btnDownload.style.display = 'none';
        
        viewer.loadGDS(url, pdk, file.name);
    };
    
    if (isLocal || !gds) {
        const loading = document.getElementById('loading');
        if (loading) {
            loading.innerHTML = `<button id="btnLocalSelect" style="font-size: 16px; padding: 12px 24px; border-radius: 8px; background: #0f0; color: #000; font-weight: bold; cursor: pointer; border: none; box-shadow: 0 4px 15px rgba(0,255,0,0.4); text-transform: uppercase;">Select Local GDS/OAS File</button>`;
            loading.style.pointerEvents = 'auto'; // allow mouse click
            document.getElementById('btnLocalSelect').onclick = () => localInput.click();
        }
        // Attempt programmatic invocation, but don't rely on it due to browser pop-up policies
        localInput.click();
    } else {
        viewer.loadGDS(gds, pdk);
    }
});
