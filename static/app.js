const state = {
  telegrams: new Map(),
  selectedId: null,
  ws: null,
  fieldDrafts: new Map(),
  mdSessions: new Map(),
  mdByComId: new Map(),
};

const telegramTableBody = document.querySelector('#telegram-table tbody');
const fieldsTableBody = document.querySelector('#fields-table tbody');
const detailTitle = document.querySelector('#detail-title');
const detailMeta = document.querySelector('#detail-meta');
const statusEl = document.querySelector('#status');
const refreshBtn = document.querySelector('#refresh-btn');
const saveBtn = document.querySelector('#save-fields');
const sendBtn = document.querySelector('#send-telegram');
const stopBtn = document.querySelector('#stop-telegram');
const clearBtn = document.querySelector('#clear-fields');
const actionGroup = document.querySelector('#detail-actions');
const mdModal = document.querySelector('#md-modal');
const mdSendBtn = document.querySelector('#md-send');
const mdCancelBtn = document.querySelector('#md-cancel');
const mdCloseBtn = document.querySelector('#md-close');
const mdSimReply = document.querySelector('#md-sim-reply');
const mdSimConfirm = document.querySelector('#md-sim-confirm');
const mdSimError = document.querySelector('#md-sim-error');
const mdTimeline = document.querySelector('#md-timeline');
const mdExpected = document.querySelector('#md-expected');
const mdReplyTimeout = document.querySelector('#md-reply-timeout');
const mdConfirmTimeout = document.querySelector('#md-confirm-timeout');
const mdDestIp = document.querySelector('#md-dest-ip');
const mdDestPort = document.querySelector('#md-dest-port');
const mdProtocol = document.querySelector('#md-protocol');
const mdPayloadSize = document.querySelector('#md-payload-size');
const mdCallerThrottle = document.querySelector('#md-caller-throttle');
const mdReplierThrottle = document.querySelector('#md-replier-throttle');
const mdToggleReplyConfirm = document.querySelector('#md-toggle-reply-confirm');
const mdMulticastReplies = document.querySelector('#md-multicast-replies');

refreshBtn.addEventListener('click', () => loadTelegrams());
saveBtn.addEventListener('click', () => persistFields());
sendBtn.addEventListener('click', () => {
  const tg = state.telegrams.get(state.selectedId);
  if (tg && tg.type === 'MD') {
    openMdModal();
  } else {
    sendTelegram();
  }
});
stopBtn.addEventListener('click', () => stopTelegram());
clearBtn.addEventListener('click', () => clearFields());
mdSendBtn.addEventListener('click', () => sendMd());
mdCancelBtn.addEventListener('click', () => closeMdModal());
mdCloseBtn.addEventListener('click', () => closeMdModal());
mdSimReply.addEventListener('click', () => simulateMd('reply'));
mdSimConfirm.addEventListener('click', () => simulateMd('confirm'));
mdSimError.addEventListener('click', () => simulateMd('error'));
mdProtocol.addEventListener('change', () => {
  if (mdProtocol.value === 'udp-multicast') {
    mdMulticastReplies.checked = true;
    if (!mdExpected.value || Number(mdExpected.value) === 0) {
      mdExpected.value = '2';
    }
  }
});

function showStatus(message, kind = 'info') {
  statusEl.textContent = message;
  statusEl.className = `status ${kind}`;
}

function isEditingSelectedTelegram(comId = state.selectedId) {
  const active = document.activeElement;
  if (!active) return false;
  const isInput = active.tagName === 'INPUT' || active.tagName === 'TEXTAREA';
  return isInput && active.closest('#fields-table') && state.selectedId === comId;
}

function formatDisplayValue(value) {
  if (Array.isArray(value)) return value.join(', ');
  if (typeof value === 'boolean') return value ? 'true' : 'false';
  if (value === null || value === undefined) return '';
  return value;
}

function formatPayloadSize(bytes) {
  if (!bytes) return '';
  if (bytes >= 1024 && bytes % 1024 === 0) {
    return `${bytes / 1024} kB`;
  }
  if (bytes >= 1024) {
    return `${(bytes / 1024).toFixed(1)} kB`;
  }
  return `${bytes} B`;
}

function formatFieldsForDisplay(fields) {
  const draft = {};
  Object.entries(fields || {}).forEach(([name, value]) => {
    draft[name] = formatDisplayValue(value);
  });
  return draft;
}

function openMdModal() {
  mdModal.classList.remove('hidden');
  renderMdTimeline();
}

function closeMdModal() {
  mdModal.classList.add('hidden');
}

function persistCurrentDraft() {
  if (!state.selectedId) return;
  const drafts = {};
  fieldsTableBody.querySelectorAll('input, textarea').forEach((el) => {
    drafts[el.dataset.fieldName] = el.value;
  });
  if (Object.keys(drafts).length) {
    state.fieldDrafts.set(state.selectedId, drafts);
  }
}

function setDraftValue(comId, name, value) {
  const current = state.fieldDrafts.get(comId) || {};
  state.fieldDrafts.set(comId, { ...current, [name]: value });
}

function getDraftValue(comId, name) {
  const current = state.fieldDrafts.get(comId) || {};
  return current[name];
}

function rememberFocus() {
  const active = document.activeElement;
  if (!active || !active.dataset.fieldName) return null;
  return {
    fieldName: active.dataset.fieldName,
    selectionStart: active.selectionStart,
    selectionEnd: active.selectionEnd,
  };
}

function restoreFocus(snapshot) {
  if (!snapshot) return;
  const target = fieldsTableBody.querySelector(`[data-field-name="${snapshot.fieldName}"]`);
  if (target) {
    target.focus();
    if (typeof snapshot.selectionStart === 'number' && typeof snapshot.selectionEnd === 'number') {
      target.setSelectionRange(snapshot.selectionStart, snapshot.selectionEnd);
    }
  }
}

function formatDirection(dir) {
  return dir === 'Tx' ? 'Tx' : 'Rx';
}

function upsertTelegram(meta) {
  const existing = state.telegrams.get(meta.comId) || { fields: {} };
  state.telegrams.set(meta.comId, { ...existing, ...meta, fields: existing.fields || {} });
}

function setTxActive(comId, active) {
  const tg = state.telegrams.get(comId);
  if (!tg) return;
  state.telegrams.set(comId, { ...tg, txActive: Boolean(active) });
  if (state.selectedId === comId) {
    renderFields();
  }
}

function updateFields(comId, fields, { replaceDraft = false } = {}) {
  const current = state.telegrams.get(comId) || { fields: {} };
  state.telegrams.set(comId, { ...current, fields: { ...current.fields, ...fields } });

  const shouldUpdateDraft = replaceDraft || !isEditingSelectedTelegram(comId);
  if (shouldUpdateDraft) {
    const updated = state.telegrams.get(comId);
    state.fieldDrafts.set(comId, formatFieldsForDisplay(updated.fields || {}));
  }

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
    persistCurrentDraft();
    const resp = await fetch(`/api/telegrams/${comId}`);
    if (!resp.ok) throw new Error('Failed to fetch telegram');
    const tg = await resp.json();
    upsertTelegram(tg);
    updateFields(comId, tg.fields || {}, { replaceDraft: true });
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

function valueToInput(fieldName, value, displayValue, editable) {
  const container = document.createElement('div');
  if (Array.isArray(value)) {
    const area = document.createElement('textarea');
    area.value = displayValue;
    area.dataset.fieldName = fieldName;
    area.disabled = !editable;
    container.appendChild(area);
    return container;
  }
  const input = document.createElement('input');
  input.type = 'text';
  input.value = displayValue;
  input.dataset.fieldName = fieldName;
  input.disabled = !editable;
  container.appendChild(input);
  return container;
}

function renderFields() {
  const focusSnapshot = rememberFocus();
  persistCurrentDraft();
  fieldsTableBody.innerHTML = '';
  const tg = state.telegrams.get(state.selectedId);
  if (!tg) {
    detailTitle.textContent = 'Telegram Details';
    detailMeta.textContent = 'Select a telegram to view details.';
    actionGroup.hidden = true;
    sendBtn.disabled = true;
    stopBtn.disabled = true;
    return;
  }

  detailTitle.textContent = `${tg.name || 'Telegram'} (${tg.comId})`;
  detailMeta.textContent = `${tg.direction} • ${tg.type} • Dataset: ${tg.dataset}`;
  const editable = tg.direction === 'Tx';
  actionGroup.hidden = !editable;
  const isTxPd = tg.direction === 'Tx' && tg.type === 'PD';
  const publishing = Boolean(tg.txActive);
  sendBtn.disabled = !editable || (isTxPd && publishing);
  stopBtn.disabled = !editable || !isTxPd || !publishing;

  const fieldEntries = Object.entries(tg.fields || {}).sort(([a], [b]) => a.localeCompare(b));
  fieldEntries.forEach(([name, value]) => {
    const tr = document.createElement('tr');
    const labelCell = document.createElement('td');
    labelCell.textContent = name;
    const valueCell = document.createElement('td');
    const draftValue = getDraftValue(state.selectedId, name);
    const displayValue = draftValue !== undefined ? draftValue : formatDisplayValue(value);
    const inputEl = valueToInput(name, value, displayValue, editable);
    const input = inputEl.querySelector('input, textarea');
    if (input && editable) {
      input.addEventListener('input', () => setDraftValue(state.selectedId, name, input.value));
    }
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

  restoreFocus(focusSnapshot);
  renderMdTimeline();
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

function collectMdOptions() {
  const selectedMode = document.querySelector('input[name="md-mode"]:checked');
  const mode = selectedMode ? selectedMode.value : 'Mn';
  return {
    mdMode: mode,
    expectedReplies: Number(mdExpected.value || 0),
    replyTimeoutMs: Number(mdReplyTimeout.value || 0),
    confirmTimeoutMs: Number(mdConfirmTimeout.value || 0),
    destIp: mdDestIp.value ? Number(mdDestIp.value) : undefined,
    destPort: Number(mdDestPort.value || 0) || undefined,
    protocol: mdProtocol.value,
    payloadBytes: Number(mdPayloadSize.value || 0),
    callerThrottle: mdCallerThrottle.checked,
    replierThrottle: mdReplierThrottle.checked,
    toggleReplyConfirm: mdToggleReplyConfirm.checked,
    multicastReplies: mdMulticastReplies.checked,
  };
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
    updateFields(state.selectedId, data, { replaceDraft: true });
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
    const data = await resp.json();
    if (data && 'txActive' in data) {
      setTxActive(state.selectedId, data.txActive);
    }
    showStatus('Telegram dispatched', 'success');
  } catch (err) {
    console.error(err);
    showStatus('Failed to send telegram', 'error');
  }
}

async function sendMd() {
  if (!state.selectedId) return;
  const payload = { ...collectFieldPayload(), ...collectMdOptions() };
  try {
    showStatus('Sending MD telegram...');
    const resp = await fetch(`/api/telegrams/${state.selectedId}/send`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload),
    });
    if (!resp.ok) throw new Error('Failed to send telegram');
    closeMdModal();
    showStatus('MD telegram dispatched', 'success');
  } catch (err) {
    console.error(err);
    showStatus('Failed to send MD telegram', 'error');
  }
}

async function stopTelegram() {
  if (!state.selectedId) return;
  try {
    showStatus('Stopping telegram...');
    const resp = await fetch(`/api/telegrams/${state.selectedId}/stop`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
    });
    if (!resp.ok) throw new Error('Failed to stop telegram');
    const data = await resp.json();
    if (data && 'txActive' in data) {
      setTxActive(state.selectedId, data.txActive);
    }
    if (data && data.ok) {
      showStatus('Telegram stopped', 'success');
    } else {
      showStatus('Failed to stop telegram', 'error');
    }
  } catch (err) {
    console.error(err);
    showStatus('Failed to stop telegram', 'error');
  }
}

async function clearFields() {
  if (!state.selectedId) return;
  const tg = state.telegrams.get(state.selectedId);
  if (!tg || !tg.fields) return;

  const payload = {};
  Object.keys(tg.fields).forEach((name) => {
    payload[name] = null;
  });

  try {
    showStatus('Clearing fields...');
    const resp = await fetch(`/api/telegrams/${state.selectedId}/fields`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload),
    });
    if (!resp.ok) throw new Error('Failed to clear fields');
    const data = await resp.json();
    state.fieldDrafts.delete(state.selectedId);
    updateFields(state.selectedId, data, { replaceDraft: true });
    showStatus('Fields cleared', 'success');
  } catch (err) {
    console.error(err);
    showStatus('Failed to clear fields', 'error');
  }
}

async function simulateMd(event) {
  if (!state.selectedId) return;
  const sessions = state.mdByComId.get(state.selectedId) || [];
  const session = sessions[sessions.length - 1] || '';
  try {
    await fetch(`/api/telegrams/${state.selectedId}/md/simulate`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ event, session }),
    });
  } catch (err) {
    console.error('simulateMd failed', err);
  }
}

function handleSnapshot(data) {
  if (!Array.isArray(data.telegrams)) return;
  data.telegrams.forEach((tg) => {
    upsertTelegram(tg);
    if ('txActive' in tg) {
      setTxActive(tg.comId, tg.txActive);
    }
    if (tg.fields) updateFields(tg.comId, tg.fields, { replaceDraft: true });
  });
  renderTelegramTable();
  renderFields();
}

function handleUpdate(message) {
  if (!message || !message.comId) return;
  if ('txActive' in message) {
    setTxActive(message.comId, message.txActive);
  }
  if (message.fields) {
    updateFields(message.comId, message.fields);
  }
}

function handleMdEvent(message) {
  if (!message || !message.session) return;
  const existing = state.mdSessions.get(message.session) || { comId: message.comId, events: [], mode: message.mode };
  existing.mode = message.mode || existing.mode;
  if (message.options) {
    existing.options = { ...(existing.options || {}), ...message.options };
  }
  const timestamp = new Date().toLocaleTimeString();
  existing.events.push({
    event: message.event,
    detail: message.detail,
    receivedReplies: message.receivedReplies,
    expectedReplies: message.expectedReplies,
    fields: message.fields,
    timestamp,
  });
  state.mdSessions.set(message.session, existing);
  const sessions = state.mdByComId.get(message.comId) || [];
  if (!sessions.includes(message.session)) {
    sessions.push(message.session);
    state.mdByComId.set(message.comId, sessions);
  }
  renderMdTimeline();
}

function renderMdTimeline() {
  mdTimeline.innerHTML = '';
  const sessions = state.mdByComId.get(state.selectedId) || [];
  sessions.slice().reverse().forEach((sessionId) => {
    const session = state.mdSessions.get(sessionId);
    if (!session) return;
    const header = document.createElement('li');
    header.classList.add('muted');
    const title = document.createElement('div');
    title.innerHTML = `<strong>${sessionId}</strong> <span class="muted-pill">${session.mode || ''}</span>`;
    header.appendChild(title);
    const metaParts = [];
    const options = session.options || {};
    if (options.protocol) metaParts.push(options.protocol);
    const payloadLabel = formatPayloadSize(options.payloadBytes);
    if (payloadLabel) metaParts.push(payloadLabel);
    if (options.multicastReplies) metaParts.push('multicast replies');
    if (options.callerThrottle) metaParts.push('caller throttle');
    if (options.replierThrottle) metaParts.push('replier throttle');
    if (options.toggleReplyConfirm) metaParts.push('toggle reply/confirm');
    if (options.replyTimeoutMs) metaParts.push(`R ${options.replyTimeoutMs}ms`);
    if (options.confirmTimeoutMs) metaParts.push(`C ${options.confirmTimeoutMs}ms`);
    if (metaParts.length) {
      const meta = document.createElement('div');
      meta.classList.add('muted');
      meta.textContent = metaParts.join(' • ');
      header.appendChild(meta);
    }
    mdTimeline.appendChild(header);
    session.events.slice().reverse().forEach((event) => {
      const item = document.createElement('li');
      const left = document.createElement('div');
      left.textContent = `${event.timestamp} • ${event.event}`;
      const right = document.createElement('div');
      const detailPieces = [];
      if (event.detail) detailPieces.push(event.detail);
      if (typeof event.receivedReplies === 'number') {
        const expected = typeof event.expectedReplies === 'number' ? event.expectedReplies : '0';
        detailPieces.push(`replies ${event.receivedReplies}/${expected}`);
      }
      right.textContent = detailPieces.join(' • ');
      item.appendChild(left);
      item.appendChild(right);
      mdTimeline.appendChild(item);
    });
  });
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
      } else if (payload.type === 'md') {
        handleMdEvent(payload);
      }
    } catch (err) {
      console.error('Failed to parse message', err);
    }
  };
}

loadTelegrams();
connectWebSocket();
renderFields();
