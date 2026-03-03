/**
 * ESP32-RAKFlasher WebUI - Core JavaScript
 * Common functions and utilities
 */

// Global state
const App = {
    ws: null,
    wsReconnectAttempts: 0,
    maxReconnectAttempts: 10,
    systemStatus: null,
    activeOps: []
};

/**
 * Initialize application
 */
function initApp() {
    // Connect WebSocket
    connectWebSocket();

    // Load initial system status
    updateSystemStatus();

    // Check for active operations (navigation persistence)
    checkActiveOperations();

    // Set active nav link
    setActiveNav();

    // Setup mobile menu toggle
    setupMobileMenu();

    // Inject global operation banner container
    injectGlobalOpsBanner();
}

/**
 * Setup mobile menu toggle
 */
function setupMobileMenu() {
    const toggle = document.querySelector('.menu-toggle');
    const nav = document.querySelector('nav');

    if (toggle && nav) {
        toggle.addEventListener('click', () => {
            nav.classList.toggle('active');
        });
    }
}

/**
 * Set active navigation link
 */
function setActiveNav() {
    const currentPage = window.location.pathname.split('/').pop() || 'index.html';
    const links = document.querySelectorAll('nav a');

    links.forEach(link => {
        if (link.getAttribute('href') === currentPage) {
            link.classList.add('active');
        } else {
            link.classList.remove('active');
        }
    });
}

/**
 * Connect to WebSocket
 */
function connectWebSocket() {
    const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
    const wsUrl = `${protocol}//${window.location.host}/ws`;

    try {
        App.ws = new WebSocket(wsUrl);

        App.ws.onopen = () => {
            console.log('[WS] Connected');
            App.wsReconnectAttempts = 0;
            updateConnectionStatus(true);
        };

        App.ws.onclose = () => {
            console.log('[WS] Disconnected');
            updateConnectionStatus(false);

            // Attempt reconnect
            if (App.wsReconnectAttempts < App.maxReconnectAttempts) {
                App.wsReconnectAttempts++;
                console.log(`[WS] Reconnecting (attempt ${App.wsReconnectAttempts})...`);
                setTimeout(connectWebSocket, 2000 * App.wsReconnectAttempts);
            }
        };

        App.ws.onerror = (error) => {
            console.error('[WS] Error:', error);
        };

        App.ws.onmessage = (event) => {
            try {
                const data = JSON.parse(event.data);
                handleWebSocketMessage(data);
            } catch (e) {
                console.error('[WS] Failed to parse message:', e);
            }
        };
    } catch (e) {
        console.error('[WS] Failed to connect:', e);
        updateConnectionStatus(false);
    }
}

/**
 * Handle incoming WebSocket messages
 */
function handleWebSocketMessage(data) {
    console.log('[WS] Message:', data);

    switch (data.type) {
        case 'progress':
            handleProgress(data);
            break;
        case 'serial':
            handleSerialData(data.data);
            break;
        case 'status':
            App.systemStatus = data.data;
            break;
        default:
            console.log('[WS] Unknown message type:', data.type);
    }
}

/**
 * Handle progress updates
 */
function handleProgress(data) {
    // Dispatch custom event for page-specific handling
    const event = new CustomEvent('progress', { detail: data });
    window.dispatchEvent(event);

    // Update global progress if element exists
    const progressBar = document.getElementById('global-progress-bar');
    const progressText = document.getElementById('global-progress-text');

    if (progressBar) {
        progressBar.style.width = `${data.progress}%`;
        progressBar.textContent = `${data.progress}%`;
    }

    if (progressText) {
        progressText.textContent = data.message;
    }
}

/**
 * Handle serial data
 */
function handleSerialData(data) {
    // Dispatch custom event for serial monitor page
    const event = new CustomEvent('serialData', { detail: data });
    window.dispatchEvent(event);
}

/**
 * Update connection status indicator
 */
function updateConnectionStatus(connected) {
    const indicator = document.getElementById('connection-status');
    if (indicator) {
        indicator.className = `status ${connected ? 'status-connected' : 'status-disconnected'}`;
        indicator.textContent = connected ? 'Connected' : 'Disconnected';
    }
}

/**
 * Update system status
 */
async function updateSystemStatus() {
    try {
        const response = await fetch('/api/status');
        if (response.ok) {
            App.systemStatus = await response.json();
            updateStatusDisplay();
        }
    } catch (error) {
        console.error('[API] Failed to fetch status:', error);
    }
}

/**
 * Update status display elements
 */
function updateStatusDisplay() {
    if (!App.systemStatus) return;

    // Update uptime
    const uptimeEl = document.getElementById('uptime');
    if (uptimeEl) {
        uptimeEl.textContent = formatUptime(App.systemStatus.uptime);
    }

    // Update connection status
    const connectionEl = document.getElementById('connection-status');
    if (connectionEl) {
        connectionEl.className = `status ${App.systemStatus.connected ? 'status-connected' : 'status-disconnected'}`;
        connectionEl.textContent = App.systemStatus.connected ? 'RAK Connected' : 'RAK Disconnected';
    }

    // Update client count
    const clientsEl = document.getElementById('clients');
    if (clientsEl) {
        clientsEl.textContent = App.systemStatus.clients;
    }

    // Update free heap
    const heapEl = document.getElementById('heap');
    if (heapEl) {
        heapEl.textContent = formatBytes(App.systemStatus.freeHeap);
    }

    // Update STA WiFi status on dashboard
    const staEl = document.getElementById('sta-wifi-status');
    if (staEl && App.systemStatus.sta) {
        const sta = App.systemStatus.sta;
        if (sta.connected && sta.ip) {
            staEl.className = 'status status-connected';
            staEl.textContent = `${sta.ssid} (${sta.ip})`;
        } else if (sta.state === 1) {
            staEl.className = 'status status-warning';
            staEl.textContent = 'Connecting...';
        } else if (sta.state === 3 || sta.state === 4) {
            staEl.className = 'status status-error';
            staEl.textContent = sta.ssid ? `Disconnected (${sta.ssid})` : 'Disconnected';
        } else {
            staEl.className = 'status status-disconnected';
            staEl.textContent = 'Not configured';
        }
    }

    // Update Serial Bridge status on dashboard
    const bridgeEl = document.getElementById('bridge-status');
    if (bridgeEl && App.systemStatus.bridge) {
        const br = App.systemStatus.bridge;
        if (br.active && br.mode === 'TCP') {
            bridgeEl.className = 'status status-connected';
            bridgeEl.textContent = 'TCP Client Connected';
        } else if (br.active && br.mode === 'HTTP') {
            bridgeEl.className = 'status status-warning';
            bridgeEl.textContent = 'HTTP API Active';
        } else {
            bridgeEl.className = 'status status-disconnected';
            bridgeEl.textContent = 'Idle';
        }
    }
}

/**
 * Format uptime in human-readable form
 */
function formatUptime(seconds) {
    const days = Math.floor(seconds / 86400);
    const hours = Math.floor((seconds % 86400) / 3600);
    const minutes = Math.floor((seconds % 3600) / 60);
    const secs = seconds % 60;

    if (days > 0) {
        return `${days}d ${hours}h ${minutes}m`;
    } else if (hours > 0) {
        return `${hours}h ${minutes}m ${secs}s`;
    } else if (minutes > 0) {
        return `${minutes}m ${secs}s`;
    } else {
        return `${secs}s`;
    }
}

/**
 * Format bytes in human-readable form
 */
function formatBytes(bytes) {
    if (bytes === 0) return '0 B';

    const k = 1024;
    const sizes = ['B', 'KB', 'MB', 'GB'];
    const i = Math.floor(Math.log(bytes) / Math.log(k));

    return parseFloat((bytes / Math.pow(k, i)).toFixed(2)) + ' ' + sizes[i];
}

/**
 * Show alert message (with stacking limits and dismiss button)
 */
function showAlert(message, type = 'info') {
    const alertContainer = document.getElementById('alert-container');
    if (!alertContainer) return;

    // Limit max visible alerts — remove oldest if over limit
    const maxAlerts = 3;
    while (alertContainer.children.length >= maxAlerts) {
        alertContainer.removeChild(alertContainer.firstChild);
    }

    const alert = document.createElement('div');
    alert.className = `alert alert-${type}`;
    alert.style.cursor = 'pointer';
    alert.title = 'Click to dismiss';
    alert.textContent = message;

    // Click to dismiss
    alert.addEventListener('click', () => alert.remove());

    alertContainer.appendChild(alert);

    // Auto-dismiss after 5 seconds
    setTimeout(() => {
        if (alert.parentNode) alert.remove();
    }, 5000);
}

/**
 * Show loading spinner
 */
function showLoading(elementId) {
    const element = document.getElementById(elementId);
    if (element) {
        element.innerHTML = '<div class="spinner"></div>';
    }
}

/**
 * Hide loading spinner
 */
function hideLoading(elementId) {
    const element = document.getElementById(elementId);
    if (element) {
        element.innerHTML = '';
    }
}

/**
 * API request helper
 * @param {string} url - API endpoint
 * @param {object} options - fetch options, plus optional `silent: true` to suppress auto-alert
 */
async function apiRequest(url, options = {}) {
    const silent = options.silent;
    delete options.silent;

    try {
        const response = await fetch(url, {
            ...options,
            headers: {
                'Content-Type': 'application/json',
                ...options.headers
            }
        });

        const data = await response.json();

        if (!response.ok) {
            throw new Error(data.error || 'Request failed');
        }

        return data;
    } catch (error) {
        console.error('[API] Request failed:', error);
        if (!silent) {
            showAlert(error.message, 'danger');
        }
        throw error;
    }
}

/**
 * API request with JSON body
 * @param {string} url - API endpoint
 * @param {object} body - JSON body to send
 * @param {object} extra - Extra options (e.g. { silent: true })
 */
async function apiPost(url, body, extra = {}) {
    return apiRequest(url, {
        method: 'POST',
        body: JSON.stringify(body),
        ...extra
    });
}

/**
 * API request with FormData
 */
async function apiUpload(url, formData) {
    try {
        const response = await fetch(url, {
            method: 'POST',
            body: formData
        });

        const data = await response.json();

        if (!response.ok) {
            throw new Error(data.error || 'Upload failed');
        }

        return data;
    } catch (error) {
        console.error('[API] Upload failed:', error);
        showAlert(error.message, 'danger');
        throw error;
    }
}

/**
 * Reboot ESP32
 */
async function rebootESP32() {
    if (confirm('Are you sure you want to reboot the ESP32?')) {
        try {
            await apiPost('/api/reboot', {});
            showAlert('Rebooting... Please wait 30 seconds before reconnecting.', 'info');
        } catch (error) {
            console.error('Reboot failed:', error);
        }
    }
}

/**
 * Enter deep sleep
 */
async function enterSleep() {
    if (confirm('Are you sure you want to enter deep sleep? The device will wake on GPIO signal.')) {
        try {
            await apiPost('/api/sleep', {});
            showAlert('Entering deep sleep...', 'info');
        } catch (error) {
            console.error('Sleep failed:', error);
        }
    }
}

// ═══════════════════════════════════════════════════════════════════
//  Global Operation Banner — persistent across page navigation
// ═══════════════════════════════════════════════════════════════════

/**
 * Inject the global operations banner into the page (below header)
 */
function injectGlobalOpsBanner() {
    if (document.getElementById('global-ops-banner')) return;
    const banner = document.createElement('div');
    banner.id = 'global-ops-banner';
    banner.style.cssText = 'display:none;background:#1a237e;color:#fff;padding:0.5rem 1rem;' +
        'font-size:0.85rem;text-align:center;cursor:pointer;position:sticky;top:0;z-index:1000;';
    banner.innerHTML = '<span id="global-ops-text"></span>';
    banner.addEventListener('click', () => {
        const page = banner.dataset.page;
        if (page) window.location.href = page;
    });
    const container = document.querySelector('.container');
    if (container) {
        container.parentNode.insertBefore(banner, container);
    }
}

/**
 * Check for any active operations on the server
 * This is the key to navigation persistence — each page load checks if
 * something is already running and can resume tracking it.
 */
async function checkActiveOperations() {
    try {
        const resp = await fetch('/api/operations/active');
        if (!resp.ok) return;
        const data = await resp.json();

        App.activeOps = data.operations || [];

        // Dispatch event for page-specific handlers to pick up
        const event = new CustomEvent('activeOperations', { detail: App.activeOps });
        window.dispatchEvent(event);

        // Update global banner for running ops NOT on current page
        updateGlobalOpsBanner();
    } catch (e) {
        // Silently fail — server may not support this endpoint yet
    }
}

/**
 * Update the global operations banner
 * Shows a banner when an operation is running on a DIFFERENT page
 */
function updateGlobalOpsBanner() {
    const banner = document.getElementById('global-ops-banner');
    if (!banner) return;

    const currentPage = window.location.pathname.split('/').pop() || 'index.html';
    const runningOps = App.activeOps.filter(op => op.running && op.page !== currentPage);

    if (runningOps.length > 0) {
        const op = runningOps[0]; // Show first running op
        const labels = {
            swd: '⚡ SWD operation in progress',
            backup: '💾 Backup in progress',
            meshtastic: '📡 Meshtastic command running',
            serial_test: '🔌 Serial test running'
        };
        const label = labels[op.type] || 'Operation in progress';
        const pct = op.progress !== undefined ? ` (${op.progress}%)` : '';
        document.getElementById('global-ops-text').textContent = label + pct + ' — click to view';
        banner.dataset.page = op.page;
        banner.style.display = 'block';
    } else {
        banner.style.display = 'none';
    }
}

// Auto-update status every 5 seconds
setInterval(updateSystemStatus, 5000);

// Check active operations every 3 seconds (for global banner updates)
setInterval(async () => {
    await checkActiveOperations();
}, 3000);

// Initialize when DOM is ready
if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', initApp);
} else {
    initApp();
}
