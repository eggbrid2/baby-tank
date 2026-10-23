const reduceMotion = window.matchMedia('(prefers-reduced-motion: reduce)').matches;

class Face {
  constructor(canvas, { mood = 'calm', interactive = false } = {}) {
    this.canvas = canvas;
    this.ctx = canvas.getContext('2d');
    this.mood = mood;
    this.interactive = interactive;
    this.look = { x: 0, y: 0 };
    this.target = { x: 0, y: 0 };
    this.blink = 0;
    this.nextBlink = performance.now() + 1800 + Math.random() * 2200;
    this.started = performance.now();
    this.resize = this.resize.bind(this);
    new ResizeObserver(this.resize).observe(canvas);
    this.resize();
    if (!reduceMotion) requestAnimationFrame((time) => this.draw(time));
    else this.draw(performance.now());
  }

  resize() {
    const rect = this.canvas.getBoundingClientRect();
    const dpr = Math.min(window.devicePixelRatio || 1, 2);
    this.canvas.width = Math.max(1, Math.round(rect.width * dpr));
    this.canvas.height = Math.max(1, Math.round(rect.height * dpr));
    this.ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    this.width = rect.width;
    this.height = rect.height;
    if (reduceMotion) this.draw(performance.now());
  }

  setMood(mood) {
    this.mood = mood;
    this.started = performance.now();
    if (reduceMotion) this.draw(performance.now());
  }

  setTarget(x, y) {
    this.target.x = Math.max(-1, Math.min(1, x));
    this.target.y = Math.max(-1, Math.min(1, y));
  }

  eyePath(cx, cy, side, time) {
    const ctx = this.ctx;
    const unit = Math.min(this.width / 260, this.height / 195);
    const bob = 0;
    const x = cx + this.look.x * 10 * unit;
    const y = cy + this.look.y * 7 * unit + bob;
    const blinkScale = Math.max(.03, 1 - this.blink);
    ctx.save();
    ctx.translate(x, y);
    ctx.scale(1, blinkScale);
    ctx.lineCap = 'round';
    ctx.lineJoin = 'round';
    ctx.strokeStyle = '#20e5d5';
    ctx.fillStyle = '#20e5d5';
    ctx.shadowColor = 'rgba(32,229,213,.28)';
    ctx.shadowBlur = 4 * unit;
    ctx.lineWidth = 10 * unit;

    if (this.mood === 'happy') {
      ctx.beginPath(); ctx.arc(0, 3 * unit, 25 * unit, Math.PI * 1.12, Math.PI * 1.88); ctx.stroke();
    } else if (this.mood === 'curious') {
      const tilt = side < 0 ? -.16 : .16;
      ctx.rotate(tilt);
      ctx.beginPath(); ctx.roundRect(-18 * unit, -27 * unit, 36 * unit, 55 * unit, 18 * unit); ctx.fill();
    } else if (this.mood === 'thinking') {
      ctx.rotate(side < 0 ? -.12 : .08);
      ctx.beginPath(); ctx.roundRect(-22 * unit, -8 * unit, 44 * unit, 18 * unit, 9 * unit); ctx.fill();
    } else if (this.mood === 'sleepy') {
      ctx.shadowBlur = 9 * unit;
      ctx.beginPath(); ctx.moveTo(-24 * unit, 2 * unit); ctx.quadraticCurveTo(0, 15 * unit, 24 * unit, 0); ctx.stroke();
    } else {
      ctx.beginPath(); ctx.roundRect(-19 * unit, -30 * unit, 38 * unit, 60 * unit, 20 * unit); ctx.fill();
    }
    ctx.restore();
  }

  drawMouth(time) {
    const ctx = this.ctx;
    const unit = Math.min(this.width / 260, this.height / 195);
    const x = this.width / 2;
    const y = this.height * .72;
    ctx.save();
    ctx.translate(x, y);
    ctx.strokeStyle = '#ffe9b3';
    ctx.fillStyle = '#ffe9b3';
    ctx.shadowColor = 'rgba(255,233,179,.22)';
    ctx.shadowBlur = 3 * unit;
    ctx.lineWidth = 7 * unit;
    ctx.lineCap = 'round';
    if (this.mood === 'happy') {
      ctx.beginPath(); ctx.arc(0, -5 * unit, 21 * unit, .18 * Math.PI, .82 * Math.PI); ctx.stroke();
    } else if (this.mood === 'curious') {
      ctx.beginPath(); ctx.arc(0, 0, 9 * unit, 0, Math.PI * 2); ctx.stroke();
    } else if (this.mood === 'thinking') {
      ctx.rotate(-.09); ctx.beginPath(); ctx.moveTo(-16 * unit, 2 * unit); ctx.quadraticCurveTo(0, -5 * unit, 17 * unit, 1 * unit); ctx.stroke();
    } else if (this.mood === 'sleepy') {
      ctx.beginPath(); ctx.ellipse(0, 1 * unit, 13 * unit, 10 * unit, 0, 0, Math.PI * 2); ctx.fill();
    } else {
      ctx.beginPath(); ctx.roundRect(-19 * unit, -4 * unit, 38 * unit, 8 * unit, 5 * unit); ctx.fill();
    }
    ctx.restore();
  }

  draw(time) {
    if (!this.width || !this.height) return;
    const ctx = this.ctx;
    ctx.clearRect(0, 0, this.width, this.height);
    if (this.interactive) {
      this.look.x += (this.target.x - this.look.x) * .07;
      this.look.y += (this.target.y - this.look.y) * .07;
    }
    if (!reduceMotion && time > this.nextBlink && this.mood !== 'sleepy') {
      const phase = (time - this.nextBlink) / 190;
      this.blink = phase < .5 ? phase * 2 : Math.max(0, (1 - phase) * 2);
      if (phase >= 1) {
        this.blink = 0;
        this.nextBlink = time + 2100 + Math.random() * 3300;
      }
    }
    const eyeY = this.height * .44;
    this.eyePath(this.width * .31, eyeY, -1, time);
    this.eyePath(this.width * .69, eyeY, 1, time);
    this.drawMouth(time);
    if (!reduceMotion) requestAnimationFrame((next) => this.draw(next));
  }
}

const heroCanvas = document.querySelector('[data-face]');
const heroFace = heroCanvas ? new Face(heroCanvas, { interactive: true }) : null;
const heroStage = document.querySelector('.hero-stage');
let pointerInsideHero = false;
let restingMood = heroFace?.mood || 'calm';
if (heroStage && heroFace && !reduceMotion) {
  heroStage.addEventListener('pointerenter', () => {
    pointerInsideHero = true;
    restingMood = heroFace.mood;
    heroFace.setMood('calm');
  });
  heroStage.addEventListener('pointermove', (event) => {
    const rect = heroStage.getBoundingClientRect();
    const x = (event.clientX - rect.left) / rect.width * 2 - 1;
    const y = (event.clientY - rect.top) / rect.height * 2 - 1;
    heroFace.setTarget(x, y);
  });
  heroStage.addEventListener('pointerleave', () => {
    pointerInsideHero = false;
    heroFace.setTarget(0, 0);
    heroFace.setMood(restingMood);
  });
}

if (heroFace && !reduceMotion) {
  const moods = ['curious', 'happy', 'calm', 'thinking', 'happy', 'sleepy', 'calm'];
  let moodIndex = 0;
  const changeMood = () => {
    if (!pointerInsideHero) {
      heroFace.setTarget(0, 0);
      restingMood = moods[moodIndex % moods.length];
      heroFace.setMood(restingMood);
      moodIndex += 1;
    }
    window.setTimeout(changeMood, 4200 + Math.random() * 2600);
  };
  window.setTimeout(changeMood, 3200);
}

const moodMeta = {
  calm: ['现在很平静', '心情 68'], curious: ['发现了新东西', '好奇 84'],
  happy: ['今天很开心', '愉悦 92'], thinking: ['正在认真想', '专注 76'],
  sleepy: ['有一点困了', '能量 18'],
};
const demoCanvas = document.querySelector('[data-face-demo]');
if (demoCanvas) {
  const demoFace = new Face(demoCanvas, { mood: 'calm', interactive: false });
  document.querySelectorAll('[data-mood]').forEach((button) => button.addEventListener('click', () => {
    document.querySelectorAll('[data-mood]').forEach((item) => item.classList.toggle('active', item === button));
    const mood = button.dataset.mood;
    demoFace.setMood(mood);
    document.querySelector('[data-mood-label]').textContent = moodMeta[mood][0];
    document.querySelector('[data-mood-value]').textContent = moodMeta[mood][1];
    document.querySelector('.thinking-dots').classList.toggle('show', mood === 'thinking');
  }));
}

const observer = new IntersectionObserver((entries) => {
  entries.forEach((entry) => { if (entry.isIntersecting) entry.target.classList.add('visible'); });
}, { threshold: .12 });
document.querySelectorAll('.reveal').forEach((element, index) => {
  element.style.transitionDelay = `${Math.min(index % 4, 2) * 70}ms`;
  observer.observe(element);
});

function formatBytes(bytes) {
  if (!Number.isFinite(bytes)) return '—';
  return `${(bytes / 1024 / 1024).toFixed(2)} MB`;
}

async function loadRelease() {
  const panel = document.querySelector('[data-release-panel]');
  try {
    const response = await fetch('/api/public/v1/releases');
    if (!response.ok) throw new Error('release_request_failed');
    const { releases } = await response.json();
    const release = releases[0];
    if (!release) {
      panel.innerHTML = '<div class="release-empty">稳定版正在准备中，源码可先在 GitHub 查看。</div>';
      return;
    }
    const safe = (value) => String(value ?? '').replace(/[&<>'"]/g, (char) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', "'": '&#39;', '"': '&quot;' })[char]);
    panel.innerHTML = `<div class="release-top"><div><p class="release-kicker">LATEST STABLE</p><div class="release-version">v${safe(release.version)}</div></div><span class="release-badge">STABLE</span></div>
      <p class="release-notes">${safe(release.notes || '稳定版头部应用固件。')}</p>
      <div class="release-facts"><div><span>目标硬件</span><b>${safe(release.board)} / ${safe(release.chip)}</b></div><div><span>文件大小</span><b>${formatBytes(release.byteSize)}</b></div><div><span>文件</span><b>${safe(release.fileName)}</b></div><div><span>SHA-256</span><b>${safe(release.sha256)}</b></div></div>
      <div class="release-actions"><a class="button" href="${safe(release.downloadUrl)}" download>下载稳定版 <span aria-hidden="true">↓</span></a><button class="copy-button" type="button" data-copy-sha>复制校验值</button></div>`;
    panel.querySelector('[data-copy-sha]').addEventListener('click', async (event) => {
      await navigator.clipboard.writeText(release.sha256);
      event.currentTarget.textContent = '已复制';
      setTimeout(() => { event.currentTarget.textContent = '复制校验值'; }, 1600);
    });
  } catch {
    panel.innerHTML = '<div class="release-empty">暂时无法读取版本，请稍后再试。</div>';
  }
}
loadRelease();
