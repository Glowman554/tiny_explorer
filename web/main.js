import { CircuitViewer } from './circuit_viewer.js';
import { initViewerUI } from './viewer_ui.js';

// Entry point
window.addEventListener('DOMContentLoaded', () => {
    const viewer = new CircuitViewer('glcanvas');
    window.viewer = viewer; // for debugging/global access
    
    initViewerUI(viewer);

    const params = new URLSearchParams(window.location.search);
    const gds = params.get('file') || 'gds/09/tt_um_znah_vga_ca.gds';
    const pdk = params.get('pdk') || 'sky130A';
    
    viewer.loadGDS(gds, pdk);
});
