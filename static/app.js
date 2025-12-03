const state = {
  telegrams: new Map(),
  selectedId: null,
  ws: null,
};

const telegramTableBody = document.querySelector('#telegram-table tbody');
const fieldsTableBody = document.querySelector('#fields-table tbody');
const detailTitle = document.querySelector('#detail-title');
const detailMeta = document.querySelector('#detail-meta');
const statusEl = document.querySelector('#status');
const refreshBtn = document.querySelector('#refresh-btn');
const saveBtn = document.querySelector('#save-fields');
const sendBtn = document.querySelector('#send-telegram');
const actionGroup = document.querySelector('#detail-actions');

refreshBtn.addEventListener('click', () => loadTelegrams());
saveBtn.addEventListener('click', () => persistFields());
sendBtn.addEventListener('click', () => sendTelegram());

function showStatus(message, kind = 'info') {
  statusEl.textContent = message;
  statusEl.className = `status ${kind}`;
}

function formatDirection(dir) {
  return dir === 'Tx' ? 'Tx' : 'Rx';
}

function upsertTelegram(meta) {
  const existing = state.telegrams.get(meta.comId) || { fields: {} };
  state.telegrams.set(meta.comId, { ...existing, ...meta, fields: existing.fields || {} });
}

function updateFields(comId, fields) {
  const current = state.telegrams.get(comId) || { fields: {} };
  state.telegrams.set(comId, { ...current, fields: { ...current.fields, ...fields } });
  if (state.selectedId === comId) {
    renderFields();
  }
}

async function loadTelegrams() {
  try {
    showStatus('Loading telegrams...');
    const resp = await fetch('/api/config/telegrams');
    if (!resp.ok) throw new Error('Failed to load telegrams');
    const data = await resp.json();
    data.forEach((tg) => upsertTelegram(tg));
    renderTelegramTable();
    showStatus(`Loaded ${data.length} telegrams`, 'success');
  } catch (err) {
    console.error(err);
    showStatus('Failed to load telegrams', 'error');
  }
}

async function loadTelegramDetails(comId) {
  try {
    const resp = await fetch(`/api/telegrams/${comId}`);
    if (!resp.ok) throw new Error('Failed to fetch telegram');
    const tg = await resp.json();
    upsertTelegram(tg);
    updateFields(comId, tg.fields || {});
    state.selectedId = comId;
    renderTelegramTable();
    renderFields();
    showStatus(`Loaded telegram ${comId}`, 'success');
  } catch (err) {
    console.error(err);
    showStatus('Failed to load telegram details', 'error');
  }
}

function renderTelegramTable() {
  telegramTableBody.innerHTML = '';
  const rows = Array.from(state.telegrams.values()).sort((a, b) => a.comId - b.comId);
  rows.forEach((tg) => {
    const tr = document.createElement('tr');
    tr.dataset.comId = tg.comId;
    if (state.selectedId === tg.comId) tr.classList.add('active');
    tr.innerHTML = `
      <td>${tg.comId}</td>
      <td>${tg.name || ''}</td>
      <td><span class="value-pill">${formatDirection(tg.direction)}</span></td>
      <td>${tg.type || ''}</td>
      <td>${tg.dataset || ''}</td>
    `;
    tr.addEventListener('click', () => loadTelegramDetails(tg.comId));
    telegramTableBody.appendChild(tr);
  });
}

function valueToInput(fieldName, value, editable) {
  const container = document.createElement('div');
  if (Array.isArray(value)) {
    const area = document.createElement('textarea');
    area.value = value.join(', ');
    area.dataset.fieldName = fieldName;
    area.disabled = !editable;
    container.appendChild(area);
    return container;
  }
  const input = document.createElement('input');
  input.type = 'text';
  input.value = value === null || value === undefined ? '' : value;
  input.dataset.fieldName = fieldName;
  input.disabled = !editable;
  container.appendChild(input);
  return container;
}

function renderFields() {
  fieldsTableBody.innerHTML = '';
  const tg = state.telegrams.get(state.selectedId);
  if (!tg) {
    detailTitle.textContent = 'Telegram Details';
    detailMeta.textContent = 'Select a telegram to view details.';
    actionGroup.hidden = true;
    return;
  }

  detailTitle.textContent = `${tg.name || 'Telegram'} (${tg.comId})`;
  detailMeta.textContent = `${tg.direction} • ${tg.type} • Dataset: ${tg.dataset}`;
  const editable = tg.direction === 'Tx';
  actionGroup.hidden = !editable;

  const fieldEntries = Object.entries(tg.fields || {}).sort(([a], [b]) => a.localeCompare(b));
  fieldEntries.forEach(([name, value]) => {
    const tr = document.createElement('tr');
    const labelCell = document.createElement('td');
    labelCell.textContent = name;
    const valueCell = document.createElement('td');
    const inputEl = valueToInput(name, value, editable);
    valueCell.appendChild(inputEl);
    tr.appendChild(labelCell);
    tr.appendChild(valueCell);
    fieldsTableBody.appendChild(tr);
  });

  if (!fieldEntries.length) {
    const tr = document.createElement('tr');
    const td = document.createElement('td');
    td.colSpan = 2;
    td.className = 'muted';
    td.textContent = 'No fields available for this telegram yet.';
    tr.appendChild(td);
    fieldsTableBody.appendChild(tr);
  }
}

function parseInputValue(original, rawValue) {
  if (Array.isArray(original)) {
    if (!rawValue.trim()) return [];
    return rawValue
      .split(',')
      .map((v) => v.trim())
      .filter((v) => v !== '')
      .map((v) => Number.isNaN(Number(v)) ? v : Number(v));
  }
  if (typeof original === 'boolean') {
    return rawValue.trim().toLowerCase() === 'true';
  }
  if (typeof original === 'number') {
    const num = Number(rawValue);
    return Number.isNaN(num) ? original : num;
  }
  return rawValue;
}

function collectFieldPayload() {
  const tg = state.telegrams.get(state.selectedId);
  if (!tg) return {};
  const payload = {};
  fieldsTableBody.querySelectorAll('input, textarea').forEach((el) => {
    const name = el.dataset.fieldName;
    const original = tg.fields ? tg.fields[name] : undefined;
    payload[name] = parseInputValue(original, el.value);
  });
  return payload;
}

async function persistFields() {
  if (!state.selectedId) return;
  const payload = collectFieldPayload();
  try {
    showStatus('Saving fields...');
    const resp = await fetch(`/api/telegrams/${state.selectedId}/fields`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload),
    });
    if (!resp.ok) throw new Error('Failed to save fields');
    const data = await resp.json();
    updateFields(state.selectedId, data);
    showStatus('Fields saved', 'success');
  } catch (err) {
    console.error(err);
    showStatus('Failed to save fields', 'error');
  }
}

async function sendTelegram() {
  if (!state.selectedId) return;
  const payload = collectFieldPayload();
  try {
    showStatus('Sending telegram...');
    const resp = await fetch(`/api/telegrams/${state.selectedId}/send`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload),
    });
    if (!resp.ok) throw new Error('Failed to send telegram');
    showStatus('Telegram dispatched', 'success');
  } catch (err) {
    console.error(err);
    showStatus('Failed to send telegram', 'error');
  }
}

function handleSnapshot(data) {
  if (!Array.isArray(data.telegrams)) return;
  data.telegrams.forEach((tg) => {
    upsertTelegram(tg);
    if (tg.fields) updateFields(tg.comId, tg.fields);
  });
  renderTelegramTable();
  renderFields();
}

function handleUpdate(message) {
  if (!message || !message.comId) return;
  if (message.fields) {
    updateFields(message.comId, message.fields);
  }
}

function connectWebSocket() {
  const protocol = location.protocol === 'https:' ? 'wss' : 'ws';
  const wsUrl = `${protocol}://${location.host}/ws/telegrams`;
  const ws = new WebSocket(wsUrl);
  state.ws = ws;
  ws.onopen = () => showStatus('WebSocket connected', 'success');
  ws.onclose = () => {
    showStatus('WebSocket disconnected. Retrying...', 'error');
    setTimeout(connectWebSocket, 1500);
  };
  ws.onerror = (err) => {
    console.error('WebSocket error', err);
    showStatus('WebSocket error', 'error');
  };
  ws.onmessage = (event) => {
    try {
      const payload = JSON.parse(event.data);
      if (payload.type === 'snapshot') {
        handleSnapshot(payload);
      } else if (payload.type === 'rx' || payload.type === 'tx') {
        handleUpdate(payload);
      }
    } catch (err) {
      console.error('Failed to parse message', err);
    }
  };
}

loadTelegrams();
connectWebSocket();
renderFields();
