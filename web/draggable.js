let highestZIndex = 100;

export const bringToFront = (panel) => {
    highestZIndex++;
    panel.style.zIndex = highestZIndex;
};

export const makeDraggable = (panelId) => {
    const panel = typeof panelId === 'string' ? document.getElementById(panelId) : panelId;
    if (!panel) return;
    
    panel.addEventListener('mousedown', () => bringToFront(panel));

    const header = panel.querySelector('.panel-header');
    if (!header) return;

    let isDragging = false;
    let startX, startY;
    let currentX, currentY;

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
};
