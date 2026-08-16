/* HScreenFilter · Flat Design 原型 — 交互逻辑（WebView2 桥接版） */
'use strict';

const $  = (sel, root = document) => root.querySelector(sel);
const $$ = (sel, root = document) => Array.from(root.querySelectorAll(sel));

/* ── 状态（与宿主同步的唯一数据源）── */
const state = {
  base:   { brightness: 0, contrast: 100, saturation: 100, highlights: 0, shadows: 0, temperature: 0 },
  hsl:    { field: 0, channel: 0, values: Array.from({ length: 9 }, () => ({ h: 0, s: 0, l: 0 })) },
  lut:    true,          // LUT 引擎开关
  vsync:  false,         // 垂直同步
  perApp: false,         // 按应用切换
  display: { index: 0, enabled: true },
  master: true,          // 滤镜总开关
  profileActive: 0,      // 当前启用配置（n选1）
  theme:  'auto',        // auto | light | dark（默认跟随系统）
  hotkey: 'Ctrl + Shift + F',
  dirty:  false,
};
let booting = true;   // 初始化阶段不弹保存条

const CHANNELS = [
  { name: '全部（主）', color: '#8899AA', hue: 210 },
  { name: '红',   color: '#FF0000', hue: 0 },
  { name: '橙',   color: '#FF8000', hue: 30 },
  { name: '黄',   color: '#FFD400', hue: 52 },
  { name: '绿',   color: '#22C55E', hue: 140 },
  { name: '青',   color: '#06B6D4', hue: 188 },
  { name: '蓝',   color: '#3B82F6', hue: 218 },
  { name: '紫',   color: '#8B5CF6', hue: 262 },
  { name: '品红', color: '#EC4899', hue: 330 },
];
const FIELD_KEYS = ['h', 's', 'l'];
const FIELD_RANGES = { h: [-180, 180], s: [-100, 100], l: [-100, 100] };
const FIELD_NAMES = ['色相', '饱和度', '明亮度'];

/* ── HSL 渐变轨道（Photoshop 风格）── */
const RAINBOW = 'linear-gradient(to right,' +
  [0, 60, 120, 180, 240, 300, 360].map(h => 'hsl(' + h + ',100%,50%)').join(',') + ')';
function hueBandGradient(hue) {
  // 分色系：色相滑块的渐变仅覆盖该色系 ±30° 的范围
  return 'linear-gradient(to right, hsl(' + (hue - 30) + ',100%,50%), hsl(' + (hue + 30) + ',100%,50%))';
}
function satGradient(hue) { return 'linear-gradient(to right, hsl(' + hue + ',12%,62%), hsl(' + hue + ',100%,62%))'; }
function lightGradient(hue) { return 'linear-gradient(to right, #000000, hsl(' + hue + ',100%,50%), #FFFFFF)'; }

/* ── 导航 ── */
$$('.nav-item').forEach(btn => btn.addEventListener('click', () => {
  $$('.nav-item').forEach(b => b.classList.toggle('active', b === btn));
  $$('.page').forEach(p => p.classList.toggle('active', p.id === 'page-' + btn.dataset.page));
  syncHost('page', { page: btn.dataset.page });
}));

/* ── 通用滑块渲染：fill 渐变 + 数值框联动 ── */
function paintRange(input, min, max, value, gradient) {
  const pct = (value - min) / (max - min) * 100;
  if (gradient && gradient.startsWith('linear-gradient')) {
    input.style.background = gradient;
  } else {
    const css = getComputedStyle(document.body);
    const accent = css.getPropertyValue('--accent').trim() || '#3B82F6';
    const border = css.getPropertyValue('--border').trim() || '#E1E7EF';
    input.style.background =
      'linear-gradient(to right,' + accent + ' 0%,' + accent + ' ' + pct + '%,' + border + ' ' + pct + '%,' + border + ' 100%)';
  }
}
function initRange(input) {
  const min = +input.min, max = +input.max, grad = input.dataset.gradient || '';
  const sync = () => {
    const val = +input.value;
    paintRange(input, min, max, val, grad);
    const num = input.closest('.slider-row').querySelector('.num');
    if (num) num.value = val;
    markDirty(); syncHost('slider', { key: input.dataset.key, value: val });
  };
  input.addEventListener('input', sync);
  input.addEventListener('change', sync);
  const num = input.closest('.slider-row').querySelector('.num');
  if (num) {
    num.addEventListener('change', () => {
      let v = +num.value;
      if (isNaN(v)) v = +input.value;
      v = Math.max(min, Math.min(max, v));
      input.value = v;
      sync();
    });
  }
  sync();
}

/* ── 基础调节 ── */
const BASE_KEY = ['brightness', 'contrast', 'saturation', 'highlights', 'shadows', 'temperature'];
$$('#baseSliders .flat-range').forEach((input, i) => {
  input.dataset.key = BASE_KEY[i];
  input.addEventListener('input', () => { state.base[input.dataset.key] = +input.value; });
  initRange(input);
});
$$('#baseSliders .num').forEach((num, i) => {
  num.dataset.key = BASE_KEY[i];
  num.addEventListener('input', () => { state.base[num.dataset.key] = +num.value; });
});

/* ── 快捷预设 ── */
const PRESETS = {
  default: { brightness: 0, contrast: 100, saturation: 100, highlights: 0, shadows: 0, temperature: 0 },
  eye:     { brightness: -8, contrast: 108, saturation: 96, highlights: -12, shadows: 8, temperature: 18 },
  night:   { brightness: -32, contrast: 115, saturation: 88, highlights: -25, shadows: 14, temperature: 46 },
  vivid:   { brightness: 4, contrast: 125, saturation: 155, highlights: 6, shadows: -6, temperature: -6 },
};
$$('.preset').forEach(btn => btn.addEventListener('click', () => {
  const p = PRESETS[btn.dataset.preset];
  if (!p) return;
  $$('#baseSliders .flat-range').forEach((input, i) => {
    input.value = p[BASE_KEY[i]];
    input.dispatchEvent(new Event('input'));
  });
  flashButton(btn);
  syncHost('preset', { name: btn.dataset.preset });
}));

/* ── HSL 页 ── */
const hslList = $('#hslSliders');
function buildHslSliders() {
  hslList.innerHTML = '';
  for (let c = 0; c < 9; c++) {
    const ch = CHANNELS[c];
    const field = state.hsl.field;
    const key = FIELD_KEYS[field];
    const [min, max] = FIELD_RANGES[key];
    const row = document.createElement('div');
    row.className = 'slider-row';
    row.dataset.channel = c;
    const label = document.createElement('div');
    label.className = 'slider-label';
    const name = document.createElement('span');
    name.innerHTML = '<i style="display:inline-block;width:8px;height:8px;border-radius:50%;background:' + ch.color + ';margin-right:6px"></i>' + ch.name;
    const num = document.createElement('input');
    num.type = 'number'; num.className = 'num'; num.value = state.hsl.values[c][key];
    num.addEventListener('change', () => {
      let v = +num.value; if (isNaN(v)) v = 0;
      v = Math.max(min, Math.min(max, v));
      state.hsl.values[c][key] = v;
      input.value = v; paintHslRange(input, c, field, v);
      markDirty(); syncHost('hsl', { channel: c, field: FIELD_NAMES[field], value: v });
    });
    label.append(name, num);
    const input = document.createElement('input');
    input.type = 'range'; input.className = 'flat-range';
    input.min = min; input.max = max; input.value = state.hsl.values[c][key];
    input.dataset.channel = c;
    input.addEventListener('input', () => {
      state.hsl.values[c][key] = +input.value;
      paintHslRange(input, c, field, +input.value);
      const n = input.closest('.slider-row').querySelector('.num');
      if (n) n.value = input.value;
      markDirty(); syncHost('hsl', { channel: c, field: FIELD_NAMES[field], value: +input.value });
    });
    row.append(label, input);
    paintHslRange(input, c, field, +input.value);
    hslList.appendChild(row);
  }
  updateHslEnabled();
}
function paintHslRange(input, channel, field, value) {
  const hue = CHANNELS[channel].hue;
  let grad;
  if (field === 0)
    grad = channel === 0 ? RAINBOW : hueBandGradient(hue);       // 色相：全部=全彩虹，分色系=±30°
  else if (field === 1) grad = satGradient(hue);                 // 饱和度：灰 → 纯色
  else                  grad = lightGradient(hue);               // 明亮度：黑 → 纯色 → 白
  input.style.background = grad;
}
$$('#hslTabs .tab').forEach(tab => tab.addEventListener('click', () => {
  $$('#hslTabs .tab').forEach(t => t.classList.toggle('active', t === tab));
  state.hsl.field = +tab.dataset.field;
  buildHslSliders();
}));
$$('.chip').forEach(chip => chip.addEventListener('click', () => {
  $$('.chip').forEach(c => c.classList.toggle('active', c === chip));
  state.hsl.channel = +chip.dataset.channel;
  $$('#hslSliders .slider-row').forEach(r => r.style.opacity = '1');
  if (state.hsl.channel > 0) {
    $$('#hslSliders .slider-row').forEach(r => {
      if (+r.dataset.channel !== state.hsl.channel && +r.dataset.channel !== 0) r.style.opacity = '.35';
    });
  }
}));
$('#hslReset').addEventListener('click', () => {
  state.hsl.values = Array.from({ length: 9 }, () => ({ h: 0, s: 0, l: 0 }));
  buildHslSliders();
  markDirty(); syncHost('hsl-reset');
  flashButton($('#hslReset'));
});

/* LUT 开关 → HSL 可用性 */
function updateHslEnabled() {
  const on = state.lut;
  $$('#hslSliders .flat-range, #hslSliders .num').forEach(el => {
    el.disabled = !on;
    el.style.opacity = on ? '' : '.45';
  });
  $$('#hslTabs .tab, .chip').forEach(el => {
    el.disabled = !on;
    el.style.opacity = on ? '' : '.45';
  });
  $('#hslReset').disabled = !on;
  $('#hslHint').textContent = on
    ? '已启用 LUT 引擎：8 个色系可分别精确调整、互不干扰（64³ 3D LUT 逐像素着色器）。'
    : '提示：关闭 LUT 引擎后使用放大镜引擎，暂不支持分色系 HSL。';
}

/* ── 按应用切换 ── */
function updatePerAppEnabled() {
  const on = state.perApp;
  $$('#bindingList .table-row, #bindingList .table-head').forEach(el => {
    el.style.opacity = on ? '' : '.45';
  });
  ['#btnBindingAdd', '#btnBindingPick', '#btnBindingEdit', '#btnBindingDel'].forEach(id => {
    $(id).disabled = !on;
  });
  $('#perAppStatus').textContent = on
    ? '● 列表内无进程在前台，滤镜已自动关闭（共 3 个检测目标）'
    : '○ 按应用切换未启用';
}

/* ── 配置启用（n选1 按钮）── */
function renderProfileActive() {
  $$('#profileList .table-row').forEach(row => {
    const i = +row.dataset.profile;
    const btn = row.querySelector('.btn-enable');
    const dot = row.querySelector('.radio-dot');
    const on = i === state.profileActive;
    btn.classList.toggle('active', on);
    btn.textContent = on ? '已启用' : '启用';
    dot.classList.toggle('on', on);
  });
}
$$('#profileList .btn-enable').forEach(btn => btn.addEventListener('click', () => {
  state.profileActive = +btn.dataset.profile;
  renderProfileActive();
  markDirty(); syncHost('profile-active', { index: state.profileActive });
}));
$$('#profileList .table-row').forEach(row => row.addEventListener('click', () => {
  state.profileActive = +row.dataset.profile;
  renderProfileActive();
  markDirty(); syncHost('profile-active', { index: state.profileActive });
}));

/* ── 主题（跟随系统，默认）── */
const media = window.matchMedia('(prefers-color-scheme: dark)');
function applyTheme() {
  const eff = state.theme === 'auto' ? (media.matches ? 'dark' : 'light') : state.theme;
  document.body.dataset.theme = eff;
  $$('#baseSliders .flat-range').forEach((input, i) => {
    paintRange(input, +input.min, +input.max, +input.value, input.dataset.gradient || '');
  });
  $$('#hslSliders .flat-range').forEach(input => {
    paintHslRange(input, +input.dataset.channel, state.hsl.field, +input.value);
  });
  syncHost('theme', { theme: state.theme, effective: eff });
}
(function initTheme() {
  const init = document.body.dataset.theme;               // auto | light | dark
  state.theme = (init === 'light' || init === 'dark') ? init : 'auto';
  $('#themeSelect').value = state.theme;
  applyTheme();
})();
$('#themeSelect').addEventListener('change', e => {
  state.theme = e.target.value;
  applyTheme();
  markDirty();
});
media.addEventListener('change', () => { if (state.theme === 'auto') applyTheme(); });

/* ── 开关（全部绑定到 state 并同步宿主）── */
function wireToggle(id, key, onChange) {
  const el = $(id);
  if (!el) return;
  el.addEventListener('change', () => {
    state[key] = el.checked;
    if (onChange) onChange(el.checked);
    markDirty(); syncHost('switch', { id, key, value: el.checked });
  });
}
wireToggle('#masterEnable', 'master', () => updateEngineStatus());
$('#displayEnable').addEventListener('change', () => {
  state.display.enabled = $('#displayEnable').checked;
  $('#displayEnableText').textContent = state.display.enabled ? '启用（已开）' : '启用（已关）';
  markDirty(); syncHost('switch', { id: 'displayEnable', key: 'displayEnabled', value: state.display.enabled });
});
wireToggle('#lutSwitch', 'lut', on => updateHslEnabled());
wireToggle('#vsyncSwitch', 'vsync');
wireToggle('#perAppSwitch', 'perApp', on => updatePerAppEnabled());
wireToggle('#captureSwitch', 'capture');
wireToggle('#autoStartSwitch', 'autoStart');
wireToggle('#autoTraySwitch', 'autoTray');

$('#displaySelect').addEventListener('change', e => {
  state.display.index = e.target.selectedIndex;
  markDirty(); syncHost('display-select', { index: state.display.index });
});

function updateEngineStatus() {
  const on = state.master;
  const el = $('#engineStatus');
  el.querySelector('.dot').style.background = on ? 'var(--green)' : 'var(--orange)';
  el.lastChild.textContent = on
    ? ' 滤镜引擎：LUT 逐像素引擎（3D LUT，支持 HSL 调色）'
    : ' 滤镜引擎：已关闭';
}
updateEngineStatus();

/* ── 快捷键按钮（原型：请求宿主捕获按键）── */
$('#btnGlobalHotkey').addEventListener('click', () => {
  flashButton($('#btnGlobalHotkey'));
  syncHost('request', { action: 'capture-global-hotkey' });
});
$('#btnHotkeySet').addEventListener('click', () => {
  flashButton($('#btnHotkeySet'));
  syncHost('request', { action: 'capture-profile-hotkey', profile: state.profileActive });
});
$('#btnHotkeyClear').addEventListener('click', () => {
  flashButton($('#btnHotkeyClear'));
  syncHost('request', { action: 'clear-profile-hotkey', profile: state.profileActive });
});

/* ── 保存条 ── */
let savedSnapshot = JSON.stringify({ base: state.base, hsl: state.hsl.values });
function markDirty() {
  if (booting || state.dirty) return;
  state.dirty = true;
  $('#saveBar').hidden = false;
}
$('#btnSaveYes').addEventListener('click', () => {
  savedSnapshot = JSON.stringify({ base: state.base, hsl: state.hsl.values });
  state.dirty = false;
  $('#saveBar').hidden = true;
  flashButton($('#btnSaveYes'));
  syncHost('save', { action: 'save' });
});
$('#btnSaveNo').addEventListener('click', () => {
  const snap = JSON.parse(savedSnapshot);
  state.base = snap.base;
  state.hsl.values = snap.hsl;
  $$('#baseSliders .flat-range').forEach((input, i) => {
    input.value = state.base[BASE_KEY[i]];
    input.dispatchEvent(new Event('input', { bubbles: true }));
  });
  buildHslSliders();
  state.dirty = false;
  $('#saveBar').hidden = true;
  syncHost('save', { action: 'cancel' });
});

/* ── 其余按钮（占位：闪烁反馈）── */
$$('.btn').forEach(b => b.addEventListener('click', () => {
  const id = b.id;
  if (['btnSaveYes', 'btnSaveNo', 'btnDebug', 'btnGlobalHotkey', 'btnHotkeySet', 'btnHotkeyClear'].includes(id)) return;
  if (b.classList.contains('preset') || b.classList.contains('btn-enable')) return;
  flashButton(b);
}));

function flashButton(btn) {
  if (!btn) return;
  btn.style.transition = 'none';
  btn.style.background = 'var(--accent)';
  btn.style.color = '#fff';
  setTimeout(() => {
    btn.style.transition = '';
    btn.style.background = '';
    btn.style.color = '';
  }, 160);
}

/* ── WebView2 桥：向宿主汇报全部状态 ── */
function notifyHost(msg) {
  try {
    if (window.chrome && window.chrome.webview && window.chrome.webview.postMessage) {
      window.chrome.webview.postMessage(msg);
    }
  } catch (e) { /* 浏览器直接预览时忽略 */ }
}
function syncHost(type, extra) {
  notifyHost(Object.assign({
    type: type || 'state',
    state: {
      master: state.master,
      displayEnabled: state.display.enabled,
      displayIndex: state.display.index,
      lut: state.lut,
      vsync: state.vsync,
      perApp: state.perApp,
      profileActive: state.profileActive,
      theme: state.theme,
      hotkey: state.hotkey,
      base: state.base,
      hsl: state.hsl.values,
    }
  }, extra || {}));
}
window.addEventListener('load', () => {
  if (window.__hsfInitField !== undefined) state.hsl.field = window.__hsfInitField;
  buildHslSliders();
  updatePerAppEnabled();
  syncHost('ready', { title: document.title, w: innerWidth, h: innerHeight });
  booting = false;
});

/* 暴露给调试 */
window.__hsfDemo = { state, CHANNELS, syncHost };
