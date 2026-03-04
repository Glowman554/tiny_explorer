const API_BASE = 'https://index.tinytapeout.com';

// State
let allShuttles = [];
let currentProjects = [];
let filteredProjects = [];
let selectedShuttle = null;

// Elements
const shuttleSelection = document.getElementById('shuttle-selection');
const shuttleBody = document.getElementById('shuttle-body');
const projectSection = document.getElementById('project-section');
const projectBody = document.getElementById('project-body');
const selectedShuttleName = document.getElementById('selected-shuttle-name');
const projectCountBadge = document.getElementById('project-count');
const searchInput = document.getElementById('project-search');
const backButton = document.getElementById('back-button');
const noResults = document.getElementById('no-results');
const modalOverlay = document.getElementById('modal-overlay');
const modalClose = document.getElementById('modal-close');
const jsonMetadata = document.getElementById('json-metadata');


// Initialize
async function init() {
    try {
        const response = await fetch(`${API_BASE}/index.json`);
        const data = await response.json();
        allShuttles = data.shuttles;
        renderShuttles();
        handleHashChange(); // Check hash after data is loaded
    } catch (error) {

        console.error('Error fetching shuttles:', error);
        shuttleBody.innerHTML = `<tr><td colspan="3"><div class="loader-container"><p>Error loading shuttles. Please try again later.</p></div></td></tr>`;
    }


    // Event Listeners
    searchInput.addEventListener('input', handleSearch);
    backButton.addEventListener('click', showShuttleSelection);
    window.addEventListener('hashchange', handleHashChange);
    modalClose.addEventListener('click', closeModal);
    modalOverlay.addEventListener('click', (e) => {
        if (e.target === modalOverlay) closeModal();
    });

    document.addEventListener('keydown', (e) => {
        if (e.key === 'Escape' && !modalOverlay.classList.contains('hidden')) {
            closeModal();
        }
    });

    // Initial check for hash
    handleHashChange();
}

// Handle Hash Change for Deep Linking
function handleHashChange() {
    const hash = window.location.hash.replace('#', '');
    if (hash && allShuttles.length > 0) {
        const shuttle = allShuttles.find(s => s.id === hash);
        if (shuttle) {
            selectShuttle(shuttle, false); // false = don't update hash again
            return;
        }
    }
    
    if (!hash && selectedShuttle) {
        showShuttleSelection(false);
    }
}


// Render Shuttles
function renderShuttles() {
    shuttleBody.innerHTML = '';
    
    // Sort shuttles by ID descending (usually newer first)
    const sortedShuttles = [...allShuttles].reverse();
    const currentYear = new Date().getFullYear();

    sortedShuttles.forEach(shuttle => {
        const row = document.createElement('tr');
        row.className = 'selectable-row';
        row.innerHTML = `
            <td>
                <div class="project-title">${shuttle.name}</div>
                <div class="meta" style="font-size: 0.8rem; color: var(--text-secondary)">${shuttle.pdk || 'Unknown PDK'}</div>
            </td>
            <td>
                <span class="badge" style="background: ${shuttle.status === 'open' ? 'var(--accent-cyan)' : 'rgba(255,255,255,0.1)'}; color: ${shuttle.status === 'open' ? '#050a15' : 'inherit'}">
                    ${shuttle.status || 'closed'}
                </span>
            </td>
            <td style="font-weight: 600; color: var(--accent-cyan)">
                ${shuttle.projects || 0} Projects
            </td>
        `;
        row.onclick = () => selectShuttle(shuttle);
        shuttleBody.appendChild(row);
    });
}


// Select Shuttle
async function selectShuttle(shuttle, updateHash = true) {
    selectedShuttle = shuttle;
    selectedShuttleName.textContent = shuttle.name;

    if (updateHash) {
        window.location.hash = shuttle.id;
    }

    
    // Transition UI
    shuttleSelection.classList.add('hidden');
    projectSection.classList.remove('hidden');
    projectBody.innerHTML = `
        <tr>
            <td colspan="5">
                <div class="loader-container">
                    <div class="spinner"></div>
                    <p>Loading projects for ${shuttle.name}...</p>
                </div>
            </td>
        </tr>
    `;

    
    try {
        const response = await fetch(`${API_BASE}/${shuttle.id}.json`);

        const data = await response.json();
        currentProjects = data.projects || [];
        filteredProjects = [...currentProjects];
        
        projectCountBadge.textContent = `${currentProjects.length} projects`;
        renderProjects(filteredProjects);
        //window.scrollTo({ top: 0, behavior: 'smooth' });
    } catch (error) {
        console.error('Error fetching projects:', error);
        projectBody.innerHTML = `<tr><td colspan="5"><div class="loader-container"><p>Error loading projects.</p></div></td></tr>`;
    }
}


// Helper to highlight search query
function highlightText(text, query) {
    if (!query) return text || '';
    const escapedQuery = query.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
    const regex = new RegExp(`(${escapedQuery})`, 'gi');
    return (text || '').replace(regex, '<mark>$1</mark>');
}

// Render Projects
function renderProjects(projects, query = '') {
    projectBody.innerHTML = '';
    
    if (projects.length === 0) {
        noResults.classList.remove('hidden');
        return;
    }
    
    noResults.classList.add('hidden');
 
    projects.forEach(project => {
        const row = document.createElement('tr');
        row.className = 'selectable-row';
        row.onclick = () => openGdsViewer(project.macro);
        row.innerHTML = `
            <td>
                <div class="project-title">${highlightText(project.title, query) || 'Untitled Project'}</div>
                <div style="margin-top: 0.4rem; display: flex; align-items: center; gap: 0.5rem;">
                    <div class="macro-badge">${highlightText(project.macro, query)}</div>
                    <span style="font-size: 0.75rem; color: var(--text-secondary); font-family: monospace;">${highlightText(project.tiles, query) || '1x1'}</span>
                </div>
            </td>

            <td class="project-author">${highlightText(project.author, query) || 'Anonymous'}</td>
            <td class="project-desc">${highlightText(project.description, query) || 'No description provided.'}</td>
            <td>
                <div class="actions">
                    <a href="https://tinytapeout.com/runs/${selectedShuttle.id}/${project.macro}/" target="_blank" onclick="event.stopPropagation()" class="action-link icon-only" title="Tiny Tapeout Project Page">
                        <svg viewBox="0 0 24 24" width="16" height="16" stroke="currentColor" stroke-width="2" fill="none" stroke-linecap="round" stroke-linejoin="round"><path d="M18 13v6a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h6"></path><polyline points="15 3 21 3 21 9"></polyline><line x1="10" y1="14" x2="21" y2="3"></line></svg>
                    </a>
                    <button class="action-link meta-btn icon-only" onclick='event.stopPropagation(); showProjectMetadata(${JSON.stringify(project).replace(/'/g, "&apos;")})' title="JSON Metadata">
                        <svg viewBox="0 0 24 24" width="16" height="16" stroke="currentColor" stroke-width="2" fill="none" stroke-linecap="round" stroke-linejoin="round"><polyline points="16 18 22 12 16 6"></polyline><polyline points="8 6 2 12 8 18"></polyline></svg>
                    </button>
                    <button class="action-link gds-btn" onclick="event.stopPropagation(); openGdsViewer('${project.macro}')" title="View GDS">
                        <svg viewBox="0 0 24 24" width="14" height="14" stroke="currentColor" stroke-width="2" fill="none"><path d="M21 16V8a2 2 0 0 0-1-1.73l-7-4a2 2 0 0 0-2 0l-7 4A2 2 0 0 0 3 8v8a2 2 0 0 0 1 1.73l7 4a2 2 0 0 0 2 0l7-4A2 2 0 0 0 21 16z"></path><polyline points="3.27 6.96 12 12.01 20.73 6.96"></polyline><line x1="12" y1="22.08" x2="12" y2="12"></line></svg>
                        View
                    </button>
                </div>
            </td>
        `;
        projectBody.appendChild(row);
    });
}


// Show Modal
function showProjectMetadata(project) {
    jsonMetadata.textContent = JSON.stringify(project, null, 4);
    modalOverlay.classList.remove('hidden');
    document.body.style.overflow = 'hidden'; // Prevent scrolling
}

// Close Modal
function closeModal() {
    modalOverlay.classList.add('hidden');
    document.body.style.overflow = '';
}



// Open GDS Viewer with template-based link and Brotli check
async function openGdsViewer(macro) {
    if (!selectedShuttle) return;
    
    let baseUrl = "";
    if (selectedShuttle.project_gds_url_template) {
        baseUrl = selectedShuttle.project_gds_url_template.replace(/{macro}/g, macro);
    } else {
        // Fallback for older data
        baseUrl = `https://raw.githubusercontent.com/TinyTapeout/tinytapeout-${selectedShuttle.id}/main/projects/${macro}/${macro}.gds`;
    }

    const brUrl = baseUrl + '.br';
    const metadataUrl = `${API_BASE}/${selectedShuttle.id}/${macro}.json`;
    const pdk = selectedShuttle.pdk || '';
    
    // Check for GDS first, then .br as fallback
    try {
        const res = await fetch(baseUrl, { method: 'HEAD' });
        if (res.ok) {
            window.open(`viewer.html?file=${encodeURIComponent(baseUrl)}&pdk=${pdk}&metadata=${encodeURIComponent(metadataUrl)}`, '_blank');
            return;
        }
    } catch (e) {
        console.warn('GDS check failed:', e);
    }

    // Try Brotli fallback
    try {
        const res = await fetch(brUrl, { method: 'HEAD' });
        if (res.ok) {
            window.open(`viewer.html?file=${encodeURIComponent(brUrl)}&pdk=${pdk}&metadata=${encodeURIComponent(metadataUrl)}`, '_blank');
            return;
        }
    } catch (e) {
        console.warn('.br check failed:', e);
    }
    
    window.open(`viewer.html?file=${encodeURIComponent(baseUrl)}&pdk=${pdk}&metadata=${encodeURIComponent(metadataUrl)}`, '_blank');
}

// Handle Search
function handleSearch(e) {
    const query = e.target.value.toLowerCase().trim();
    
    if (!query) {
        filteredProjects = [...currentProjects];
    } else {
        filteredProjects = currentProjects.filter(p => {
            const title = (p.title || '').toLowerCase();
            const author = (p.author || '').toLowerCase();
            const desc = (p.description || '').toLowerCase();
            const macro = (p.macro || '').toLowerCase();
            const tiles = (p.tiles || '').toLowerCase();
            return title.includes(query) || author.includes(query) || desc.includes(query) || macro.includes(query) || tiles.includes(query);
        });
    }
    
    renderProjects(filteredProjects, query);
}

// Back to Selection
function showShuttleSelection(updateHash = true) {
    if (updateHash) {
        window.location.hash = '';
    }
    projectSection.classList.add('hidden');
    shuttleSelection.classList.remove('hidden');
    searchInput.value = '';
    filteredProjects = [];
    selectedShuttle = null;
    shuttleSelection.scrollIntoView({ behavior: 'smooth' });
}


init();
