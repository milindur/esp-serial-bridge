const HEALTH_WINDOW_MS = 10000;

const successKeys = ['send_success_count', 'rx_received_packets'];
const failureKeys = ['send_fail_count', 'rx_dropped_bytes', 'rx_truncated_bytes', 'tx_dropped_packets', 'tx_dropped_bytes'];
const history = [];

const groups = {
  radio: [
    ['own_mac', 'Own STA MAC'],
    ['peer_mac', 'Peer MAC'],
  ],
  traffic: [
    ['rx_received_packets', 'RX packets'],
    ['rx_received_bytes', 'RX bytes'],
    ['send_success_count', 'Send success'],
  ],
  errors: [
    ['send_fail_count', 'Send failures'],
    ['rx_dropped_bytes', 'RX dropped bytes'],
    ['rx_truncated_bytes', 'RX truncated bytes'],
    ['tx_dropped_packets', 'TX dropped packets'],
    ['tx_dropped_bytes', 'TX dropped bytes'],
  ],
};

const el = {
  uptime: document.getElementById('uptime'),
  channel: document.getElementById('channel'),
  updated: document.getElementById('updated'),
  health: document.getElementById('health'),
  radio: document.getElementById('radio'),
  traffic: document.getElementById('traffic'),
  errors: document.getElementById('errors'),
};

function formatDuration(ms) {
  const total = Math.floor(ms / 1000);
  const h = Math.floor(total / 3600);
  const m = Math.floor((total % 3600) / 60);
  const s = total % 60;
  return `${h}h ${String(m).padStart(2, '0')}m ${String(s).padStart(2, '0')}s`;
}

function formatNumber(value) {
  return typeof value === 'number' ? value.toLocaleString() : value;
}

function severityClass(key, value) {
  if (!value) return '';
  if (key.includes('fail') || key.includes('dropped') || key.includes('truncated')) return 'bad';
  return '';
}

function renderGroup(target, rows, status) {
  target.innerHTML = rows.map(([key, label]) => {
    const value = formatNumber(status[key]);
    const className = severityClass(key, status[key]);
    return `<dt>${label}</dt><dd class="${className}">${value}</dd>`;
  }).join('');
}

function sumDelta(keys, now, then) {
  return keys.reduce((total, key) => total + Math.max(0, (now[key] || 0) - (then[key] || 0)), 0);
}

function windowDelta(status) {
  const now = Date.now();
  history.push({ time: now, status });
  while (history.length > 1 && history[0].time < now - HEALTH_WINDOW_MS) history.shift();
  return history.length > 1 ? history[0].status : null;
}

function renderHealth(status) {
  const baseline = windowDelta(status);
  const detail = `${Math.round(HEALTH_WINDOW_MS / 1000)}s window`;

  if (!baseline) {
    el.health.className = 'health health--loading';
    el.health.innerHTML = `<span class="health-dot"></span>Unknown <small>${detail}</small>`;
    return;
  }

  const okCount = sumDelta(successKeys, status, baseline);
  const errorCount = sumDelta(failureKeys, status, baseline);
  let className = 'health health--loading';
  let label = 'Unknown';

  if (okCount > 0 && errorCount === 0) {
    className = 'health';
    label = 'Healthy';
  } else if (okCount === 0 && errorCount > 0) {
    className = 'health health--bad';
    label = 'Check errors';
  } else if (okCount > 0 && errorCount > 0) {
    className = 'health health--warn';
    label = 'Mixed traffic';
  }

  el.health.className = className;
  el.health.innerHTML = `<span class="health-dot"></span>${label} <small>${detail}</small>`;
}

async function poll() {
  try {
    const response = await fetch('/api/status', { cache: 'no-store' });
    if (!response.ok) throw new Error(response.status);

    const status = await response.json();
    el.uptime.textContent = formatDuration(status.uptime_ms);
    el.channel.textContent = status.channel;
    el.updated.textContent = new Date().toLocaleTimeString();
    renderGroup(el.radio, groups.radio, status);
    renderGroup(el.traffic, groups.traffic, status);
    renderGroup(el.errors, groups.errors, status);
    renderHealth(status);
  } catch (error) {
    el.updated.textContent = 'failed';
    el.health.className = 'health health--bad';
    el.health.innerHTML = `<span class="health-dot"></span>Offline`;
  }
}

poll();
setInterval(poll, 2000);
