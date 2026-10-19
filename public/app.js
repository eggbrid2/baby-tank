const state = { csrf: '', data: null };
const $ = (selector) => document.querySelector(selector);
const $$ = (selector) => [...document.querySelectorAll(selector)];

async function api(path, options = {}) {
  const headers = new Headers(options.headers || {});
  if (options.body && typeof options.body === 'string') headers.set('content-type', 'application/json');
  if (state.csrf && options.method && options.method !== 'GET') headers.set('x-csrf-token', state.csrf);
  const response = await fetch(path, { ...options, headers });
  const body = response.headers.get('content-type')?.includes('json') ? await response.json() : null;
  if (!response.ok) throw new Error(body?.error || `HTTP ${response.status}`);
  return body;
}

function escapeHtml(value) {
  return String(value ?? '').replace(/[&<>'"]/g, (char) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', "'": '&#39;', '"': '&quot;' })[char]);
}

function ago(value) {
  if (!value) return '从未在线';
  const seconds = Math.max(0, Math.round((Date.now() - new Date(value).getTime()) / 1000));
  if (seconds < 60) return `${seconds} 秒前`;
  if (seconds < 3600) return `${Math.floor(seconds / 60)} 分钟前`;
  if (seconds < 86400) return `${Math.floor(seconds / 3600)} 小时前`;
  return new Date(value).toLocaleString('zh-CN');
}

const pageTitles = { overview: '系统概览', providers: 'AI 服务', firmware: '固件发布', devices: '设备管理', account: '账户安全' };

function requestedPage() {
  return location.pathname.replace(/\/+$/, '') === '/account' ? 'account' : 'overview';
}

function showPage(name, updateHistory = true) {
  const pageName = pageTitles[name] ? name : 'overview';
  $$('.nav-item').forEach((item) => item.classList.toggle('active', item.dataset.page === pageName));
  $$('.page').forEach((page) => page.classList.add('hidden'));
  $(`#page-${pageName}`).classList.remove('hidden');
  $('#page-title').textContent = pageTitles[pageName];
  const path = pageName === 'account' ? '/account' : '/';
  if (updateHistory && location.pathname !== path) history.pushState({ page: pageName }, '', path);
}

async function refresh() {
  const data = await api('/api/admin/overview');
  state.data = data;
  state.csrf = data.csrfToken;
  $('#device-count').textContent = data.devices.length;
  $('#online-count').textContent = data.devices.filter((d) => d.last_seen_at && Date.now() - new Date(d.last_seen_at) < 300000).length;
  $('#firmware-count').textContent = data.firmware.length;
  $('#provider-count').textContent = data.providers.filter((p) => p.enabled).length;
  renderDevices(data.devices);
  renderFirmware(data.firmware);
  renderProviders(data.providers);
}

function renderDevices(devices) {
  const rows = devices.map((d) => `<div class="list-row"><div><strong>${escapeHtml(d.name)}</strong><small>${escapeHtml(d.device_id)} · ${escapeHtml(d.board)}</small></div><div class="row-meta"><strong>${escapeHtml(d.firmware_version || '未上报')}</strong><small>${d.revoked_at ? '已吊销' : ago(d.last_seen_at)}</small></div></div>`).join('');
  $('#device-list').innerHTML = rows || '<div class="empty">暂无设备</div>';
  $('#recent-devices').innerHTML = rows || '<div class="empty">暂无设备</div>';
}

function renderFirmware(items) {
  $('#firmware-list').innerHTML = items.map((f) => `<div class="list-row"><div><strong>${escapeHtml(f.version)} <small>${escapeHtml(f.channel)}</small></strong><small>${escapeHtml(f.board)} · ${(f.byte_size / 1048576).toFixed(2)} MB · secure v${f.secure_version}</small></div><div class="row-actions">${f.published ? '<span class="provider-status on">已发布</span>' : `<button data-publish="${f.id}">发布</button>`}</div></div>`).join('') || '<div class="empty">暂无固件</div>';
  $$('[data-publish]').forEach((button) => button.addEventListener('click', async () => {
    await api(`/api/admin/firmware/${button.dataset.publish}/publish`, { method: 'POST', body: '{}' });
    await refresh();
  }));
}

function renderProviders(items) {
  for (const kind of ['llm', 'asr', 'tts']) {
    const entries = items.filter((item) => item.kind === kind);
    $(`#${kind}-list`).innerHTML = entries.map((item) => {
      const thinking = kind === 'llm' ? ` · 思考${({ on: '开启', off: '关闭', default: '默认' })[item.options?.thinkingMode] || '默认'}${item.options?.reasoningEffort ? `/${item.options.reasoningEffort}` : ''}` : '';
      return `<div class="provider-item"><span class="provider-status ${item.enabled ? 'on' : ''}">${item.enabled ? '启用' : '停用'}</span><strong>${escapeHtml(item.label)}</strong><small>${escapeHtml(item.provider)} · ${escapeHtml(item.model || '未指定模型')}${escapeHtml(thinking)} · ${item.has_secret ? '密钥已保存' : '无密钥'}</small><button type="button" class="text-button" data-edit-provider="${escapeHtml(item.provider)}" data-provider-kind="${kind}">编辑</button></div>`;
    }).join('') || '<div class="empty">尚未配置</div>';
  }
  $$('[data-edit-provider]').forEach((button) => button.addEventListener('click', () => {
    openProviderDialog(button.dataset.providerKind, button.dataset.editProvider);
  }));
}

$('#login-form').addEventListener('submit', async (event) => {
  event.preventDefault();
  const data = Object.fromEntries(new FormData(event.currentTarget));
  try {
    const result = await api('/api/auth/login', { method: 'POST', body: JSON.stringify(data) });
    state.csrf = result.csrfToken;
    await refresh();
    $('#login').classList.add('hidden');
    $('#app').classList.remove('hidden');
    showPage(requestedPage(), false);
  } catch (error) { $('#login-error').textContent = error.message === 'invalid_credentials' ? '用户名或密码错误' : error.message; }
});

$$('.nav-item').forEach((button) => button.addEventListener('click', (event) => {
  event.preventDefault();
  showPage(button.dataset.page);
}));
window.addEventListener('popstate', () => showPage(requestedPage(), false));

$('#logout').addEventListener('click', async () => {
  await api('/api/auth/logout', { method: 'POST', body: '{}' });
  location.reload();
});

$('#password-form').addEventListener('submit', async (event) => {
  event.preventDefault();
  const form = new FormData(event.currentTarget);
  const currentPassword = String(form.get('currentPassword') || '');
  const newPassword = String(form.get('newPassword') || '');
  const confirmPassword = String(form.get('confirmPassword') || '');
  const result = $('#password-result');
  if (newPassword !== confirmPassword) {
    result.textContent = '两次输入的新密码不一致。';
    return;
  }
  try {
    await api('/api/auth/change-password', { method: 'POST', body: JSON.stringify({ currentPassword, newPassword }) });
    result.textContent = '密码已修改，正在返回登录页……';
    setTimeout(() => { location.href = '/'; }, 800);
  } catch (error) {
    result.textContent = ({
      current_password_incorrect: '当前密码不正确。',
      new_password_length_invalid: '新密码必须为 12 至 128 位。',
      new_password_must_differ: '新密码不能与当前密码相同。',
    })[error.message] || error.message;
  }
});

$('#device-form').addEventListener('submit', async (event) => {
  event.preventDefault();
  const body = Object.fromEntries(new FormData(event.currentTarget));
  const result = await api('/api/admin/devices', { method: 'POST', body: JSON.stringify(body) });
  $('#device-token').textContent = `设备 ID: ${result.deviceId}\n设备令牌: ${result.token}`;
  $('#device-token').classList.remove('hidden');
  await refresh();
});

$('#firmware-form').addEventListener('submit', async (event) => {
  event.preventDefault();
  const form = new FormData(event.currentTarget);
  const file = form.get('file');
  const query = new URLSearchParams({ version: form.get('version'), board: form.get('board'), channel: form.get('channel'),
    fileName: file.name, secureVersion: form.get('secureVersion'), rolloutPercent: form.get('rolloutPercent'),
    chip: form.get('chip'), notes: form.get('notes'), secureBootRequired: form.get('secureBootRequired') ? 'true' : 'false' });
  try {
    const result = await api(`/api/admin/firmware?${query}`, { method: 'PUT', body: file, headers: { 'content-type': 'application/octet-stream' } });
    $('#firmware-result').textContent = `已上传 ${result.version}，检查后可发布。`;
    await refresh();
  } catch (error) { $('#firmware-result').textContent = error.message; }
});

const providers = { llm: ['qwen', 'openai', 'deepseek', 'custom'], asr: ['qwen', 'volcengine', 'tencent', 'custom'], tts: ['volcengine', 'qwen', 'tencent', 'custom'] };
const modelCatalog = {
  llm: {
    qwen: [['qwen3.8-flash', 'Qwen 3.8 Flash（快速）'], ['qwen3.8-max', 'Qwen 3.8 Max（质量）'], ['qwen3.7-plus', 'Qwen 3.7 Plus'], ['qwen-plus', 'Qwen Plus（兼容）']],
    openai: [['gpt-6-luna', 'GPT-6 Luna（快速）'], ['gpt-6-sol', 'GPT-6 Sol（均衡）'], ['gpt-6-astra', 'GPT-6 Astra（质量）']],
    deepseek: [['deepseek-v4-flash', 'DeepSeek V4 Flash（快速）'], ['deepseek-v4-pro', 'DeepSeek V4 Pro（质量）']],
    custom: [],
  },
  asr: { qwen: [['paraformer-realtime-v2', 'Paraformer Realtime V2']], volcengine: [], tencent: [], custom: [] },
  tts: { volcengine: [['seed-tts-2.0', '豆包语音合成 2.0']], qwen: [], tencent: [], custom: [] },
};

function updateCustomModelVisibility() {
  const form = $('#provider-form');
  const custom = form.elements.modelChoice.value === '__custom';
  $('#custom-model-field').classList.toggle('hidden', !custom);
  form.elements.customModel.required = custom;
}

function populateModels(kind, provider) {
  const form = $('#provider-form');
  const models = modelCatalog[kind]?.[provider] || [];
  form.elements.modelChoice.innerHTML = [
    ...models.map(([value, label]) => `<option value="${escapeHtml(value)}">${escapeHtml(label)}</option>`),
    '<option value="__custom">自定义模型 ID…</option>',
  ].join('');
  if (!models.length) form.elements.modelChoice.value = '__custom';
  updateCustomModelVisibility();
}

function savedProvider(kind, provider) {
  return state.data?.providers?.find((item) => item.kind === kind && item.provider === provider) || null;
}

function setModelValue(model) {
  const form = $('#provider-form');
  const value = String(model || '').trim();
  const known = [...form.elements.modelChoice.options].some((option) => option.value === value);
  form.elements.modelChoice.value = known ? value : '__custom';
  form.elements.customModel.value = known ? '' : value;
  updateCustomModelVisibility();
}

function applyProviderDefaults() {
  const form = $('#provider-form');
  const kind = form.elements.kind.value;
  const provider = form.elements.provider.value;
  const isDoubaoTts = kind === 'tts' && provider === 'volcengine';
  const isQwenAsr = kind === 'asr' && provider === 'qwen';
  const simpleApiKey = kind === 'llm' || isDoubaoTts || isQwenAsr;
  const existing = savedProvider(kind, provider);

  for (const name of ['label', 'baseUrl', 'customModel', 'apiKey', 'appId', 'resourceId', 'secretId', 'secretKey', 'accessToken',
    'workspaceId', 'speaker', 'audioFormat', 'sampleRate', 'speechRate', 'loudnessRate', 'temperature']) {
    if (form.elements[name]) form.elements[name].value = '';
  }
  form.elements.enabled.checked = false;
  form.elements.thinkingMode.value = 'default';
  form.elements.reasoningEffort.value = '';
  form.elements.maxTokens.value = '2048';
  populateModels(kind, provider);
  form.elements.label.value = isDoubaoTts ? '豆包语音 TTS 2.0' : (isQwenAsr ? '阿里百炼 Paraformer ASR' : provider);
  $('#app-id-field').classList.toggle('hidden', simpleApiKey);
  $('#access-token-field').classList.toggle('hidden', simpleApiKey);
  $('#secret-fields').classList.toggle('hidden', simpleApiKey);
  $('#resource-id-field').classList.toggle('hidden', !isDoubaoTts);
  $('#workspace-id-field').classList.toggle('hidden', !isQwenAsr);
  $('#audio-options').classList.toggle('hidden', !isDoubaoTts);
  $('#llm-options').classList.toggle('hidden', kind !== 'llm');
  $('#llm-dialect-field').classList.toggle('hidden', kind !== 'llm' || provider !== 'custom');
  if (kind === 'llm') form.elements.llmDialect.value = provider === 'custom' ? 'openai' : provider;
  $('#provider-guide').classList.toggle('hidden', !isDoubaoTts && !isQwenAsr);
  $('#provider-guide').innerHTML = isDoubaoTts
    ? '新版豆包语音只需 API Key。先在<a href="https://console.volcengine.com/speech/new/setting/activate?projectName=default" target="_blank" rel="noreferrer">开通管理</a>启用语音合成 2.0，再到<a href="https://console.volcengine.com/speech/new/setting/apikeys?projectName=default" target="_blank" rel="noreferrer">API 管理</a>复制 Key，并从<a href="https://console.volcengine.com/speech/new/voices?ResourceID=volc.seedtts.default&projectName=default" target="_blank" rel="noreferrer">音色库</a>复制 Speaker ID。'
    : (isQwenAsr
      ? 'ASR 使用阿里云百炼 Paraformer，地域必须选择华北 2（北京）。到<a href="https://bailian.console.aliyun.com/cn-beijing/model/settings/api-key" target="_blank" rel="noreferrer">API Key 页面</a>创建并复制 Key；Workspace ID 在百炼控制台右上角的业务空间信息中复制。'
      : '');
  if (kind === 'asr' && provider === 'qwen') {
    form.elements.audioFormat.value = 'pcm';
    form.elements.sampleRate.value = '16000';
  } else if (kind === 'tts' && provider === 'volcengine') {
    // Credentials are intentionally not returned by the admin API.  Never put
    // a credential default into an existing provider form: submitting it would
    // overwrite the encrypted value (notably seed-icl-2.0 for cloned voices).
    if (!existing) form.elements.resourceId.value = 'seed-tts-2.0';
    form.elements.speaker.value = 'zh_female_vv_uranus_bigtts';
    form.elements.audioFormat.value = 'mp3';
    form.elements.sampleRate.value = '24000';
  }

  if (existing) {
    form.elements.label.value = existing.label || provider;
    form.elements.baseUrl.value = existing.base_url || '';
    form.elements.enabled.checked = Boolean(existing.enabled);
    setModelValue(existing.model);
    for (const [name, value] of Object.entries(existing.options || {})) {
      if (form.elements[name] && value !== null && value !== undefined) form.elements[name].value = String(value);
    }
  }
  const credentialMessage = existing?.has_secret ? '凭据已加密保存。为安全不回显原文，留空即保留；仅在需要更换时重新填写。' : '只填写当前供应商要求的字段；留空会保留已保存的值。';
  $('#credential-help').textContent = credentialMessage;
  for (const name of ['apiKey', 'appId', 'resourceId', 'secretId', 'secretKey', 'accessToken', 'workspaceId']) {
    form.elements[name].placeholder = existing?.has_secret ? '已保存，留空保留' : '';
  }
}

function openProviderDialog(kind, selectedProvider) {
  const form = $('#provider-form');
  form.reset();
  form.elements.kind.value = kind;
  form.elements.provider.innerHTML = providers[kind].map((name) => `<option>${name}</option>`).join('');
  if (selectedProvider && providers[kind].includes(selectedProvider)) form.elements.provider.value = selectedProvider;
  applyProviderDefaults();
  $('#provider-result').textContent = '';
  $('#provider-dialog').showModal();
}

$$('.add-provider').forEach((button) => button.addEventListener('click', () => openProviderDialog(button.dataset.kind)));
$('#provider-form').elements.provider.addEventListener('change', applyProviderDefaults);
$('#provider-form').elements.modelChoice.addEventListener('change', updateCustomModelVisibility);
$('#provider-dialog .close').addEventListener('click', () => $('#provider-dialog').close());
$('#provider-form').addEventListener('submit', async (event) => {
  event.preventDefault();
  const form = new FormData(event.currentTarget);
  const body = Object.fromEntries(['kind', 'provider', 'label', 'baseUrl'].map((key) => [key, String(form.get(key) || '').trim()]));
  body.model = String(form.get('modelChoice') === '__custom' ? form.get('customModel') : form.get('modelChoice')).trim();
  body.enabled = form.has('enabled');
  body.credentials = Object.fromEntries(['apiKey', 'appId', 'resourceId', 'secretId', 'secretKey', 'accessToken', 'workspaceId']
    .map((key) => [key, String(form.get(key) || '').trim()]).filter(([, value]) => value));
  const optionKeys = body.kind === 'llm'
    ? ['llmDialect', 'thinkingMode', 'reasoningEffort', 'temperature', 'maxTokens']
    : ['speaker', 'audioFormat', 'sampleRate', 'speechRate', 'loudnessRate'];
  body.options = Object.fromEntries(optionKeys
    .map((key) => [key, String(form.get(key) || '').trim()]).filter(([, value]) => value !== ''));
  try {
    await api('/api/admin/providers', { method: 'PUT', body: JSON.stringify(body) });
    $('#provider-dialog').close();
    await refresh();
  } catch (error) { $('#provider-result').textContent = error.message; }
});

refresh().then(() => {
  $('#login').classList.add('hidden');
  $('#app').classList.remove('hidden');
  showPage(requestedPage(), false);
}).catch(() => {});
