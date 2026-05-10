const statusEl = document.getElementById('status');
const stateEl = document.getElementById('state');

const labels = {
  uptime_ms: 'Uptime',
  own_mac: 'Own STA MAC',
  peer_mac: 'Peer MAC',
  channel: 'WiFi / ESP-NOW channel',
  rx_received_packets: 'ESP-NOW RX packets',
  rx_received_bytes: 'ESP-NOW RX bytes',
  rx_dropped_bytes: 'ESP-NOW RX dropped bytes',
  rx_truncated_bytes: 'ESP-NOW RX truncated bytes',
  send_success_count: 'ESP-NOW send success count',
  send_fail_count: 'ESP-NOW send fail count',
  tx_dropped_packets: 'UART-to-ESP-NOW TX dropped packets',
  tx_dropped_bytes: 'UART-to-ESP-NOW TX dropped bytes',
};

function formatValue(key, value) {
  if (key === 'uptime_ms') {
    const seconds = Math.floor(value / 1000);
    return `${Math.floor(seconds / 3600)}h ${Math.floor((seconds % 3600) / 60)}m ${seconds % 60}s`;
  }
  return value;
}

async function poll() {
  try {
    const response = await fetch('/api/status', { cache: 'no-store' });
    if (!response.ok) {
      throw new Error(response.status);
    }

    const status = await response.json();
    statusEl.innerHTML = Object.keys(labels)
      .map((key) => `<dt>${labels[key]}</dt><dd>${formatValue(key, status[key])}</dd>`)
      .join('');
    stateEl.textContent = `Updated ${new Date().toLocaleTimeString()}`;
    stateEl.className = '';
  } catch (error) {
    stateEl.textContent = `Update failed: ${error}`;
    stateEl.className = 'bad';
  }
}

poll();
setInterval(poll, 2000);
