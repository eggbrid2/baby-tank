let csrfToken = '';
const authShell = document.querySelector('[data-auth-shell]');
const accountShell = document.querySelector('[data-account-shell]');

function showAccount(account, csrf = '') {
  csrfToken = csrf;
  authShell.hidden = true;
  accountShell.hidden = false;
  accountShell.querySelector('[data-account-name]').textContent = account.displayName;
  accountShell.querySelector('[data-account-email]').textContent = account.email;
}

function showAuth() {
  csrfToken = '';
  accountShell.hidden = true;
  authShell.hidden = false;
}

document.querySelectorAll('[data-tab]').forEach((button) => button.addEventListener('click', () => {
  document.querySelectorAll('[data-tab]').forEach((item) => item.classList.toggle('active', item === button));
  document.querySelectorAll('[data-form]').forEach((form) => { form.hidden = form.dataset.form !== button.dataset.tab; });
}));

const messages = { invalid_credentials: '邮箱或密码不正确。', account_exists: '这个邮箱已经注册。',
  invalid_password_length: '密码至少需要 12 个字符。', consent_required: '请确认监护人与条款选项。',
  invalid_email: '请输入有效邮箱。', invalid_display_name: '请输入称呼。', too_many_attempts: '尝试过多，请稍后再试。' };

document.querySelectorAll('[data-form]').forEach((form) => form.addEventListener('submit', async (event) => {
  event.preventDefault();
  if (!form.reportValidity()) return;
  const data = new FormData(form);
  const status = form.querySelector('[data-status]');
  const button = form.querySelector('button[type=submit]');
  button.disabled = true; status.className = 'status'; status.textContent = form.dataset.form === 'login' ? '正在登录…' : '正在创建…';
  const payload = Object.fromEntries(data.entries());
  payload.guardianConfirmed = data.get('guardianConfirmed') === 'on';
  payload.termsAccepted = data.get('termsAccepted') === 'on';
  try {
    const response = await fetch(`/api/cloud/v1/${form.dataset.form}`, { method: 'POST',
      headers: { 'content-type': 'application/json' }, body: JSON.stringify(payload) });
    const result = await response.json();
    if (!response.ok) throw new Error(result.error || 'request_failed');
    if (form.dataset.form === 'register') {
      status.textContent = '账号已创建，现在可以登录。';
      document.querySelector('[data-tab="login"]').click();
      document.querySelector('[data-form="login"] input[name="email"]').value = payload.email;
    } else showAccount(result.account, result.csrfToken);
  } catch (error) { status.className = 'status error'; status.textContent = messages[error.message] || '暂时无法完成，请稍后再试。'; }
  finally { button.disabled = false; }
}));

document.querySelector('[data-logout]').addEventListener('click', async () => {
  const response = await fetch('/api/cloud/v1/logout', { method: 'POST', headers: { 'x-csrf-token': csrfToken } });
  if (response.ok) showAuth();
});

fetch('/api/cloud/v1/me').then(async (response) => {
  if (response.ok) { const result = await response.json(); showAccount(result.account, result.csrfToken); }
}).catch(() => {});
