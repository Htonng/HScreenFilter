/* HScreenFilter demo2 — WebView2 桥接版交互逻辑（完整功能）
 * 数据源：C++ 宿主（profiles.json）。范围/预设/行为与原项目对齐。
 */
'use strict';

const $  = (sel, root = document) => root.querySelector(sel);
const $$ = (sel, root = document) => Array.from(root.querySelectorAll(sel));

/* ── 精确范围（与原项目一致；宿主 init 再下发覆盖）── */
const RANGES = {
  base: {
    brightness: [-100, 100], contrast: [0, 200], saturation: [0, 200],
    highlights: [-100, 100], shadows: [-100, 100], temperature: [-100, 100],
  },
  hsl: {
    hueMaster: [-180, 180], hueChannel: [-30, 30],
    sat: [0, 200], satBase: 100,
    light: [-30, 30],
  },
};
const BASE_KEYS = ['brightness', 'contrast', 'saturation', 'highlights', 'shadows', 'temperature'];
const BASE_LABELS = {
  brightness: '亮度 Brightness', contrast: '对比度 Contrast', saturation: '鲜艳度 Saturation',
  highlights: '亮部 Highlights', shadows: '暗部 Shadows', temperature: '色温（冷 ← → 暖）',
};
const CHANNELS = [
  { name: '全部（主）', color: '#8899AA', hue: 210, master: true },
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
const FIELD_NAMES = ['色相', '饱和度', '明亮度'];

const state = {
  initialized: false,
  settings: {
    base: { brightness: 0, contrast: 100, saturation: 100, highlights: 0, shadows: 0, temperature: 0 },
    hsl: { master: { h: 0, s: 100, l: 0 }, channels: CHANNELS.slice(1).map(c => ({ name: c.name, h: 0, s: 100, l: 0 })) },
  },
  hslField: 0,
  displays: [], displayIndex: 0,
  master: false, lut: false, perapp: false,
  autostart: false, autotray: true,
  globalHotkey: '', engineText: '',
  profiles: [], activeProfile: -1, bindings: [],
  selectedProfile: -1, selectedBinding: -1,
  dirty: false, theme: 'auto',
  capturing: null,        // {scope, profile}
  perAppStatus: '○ 按应用切换未启用',
};

/* ── 消息桥 ── */
let standalone = true;

function send(msg) {
  try {
    if (window.chrome && window.chrome.webview && window.chrome.webview.postMessage) {
      window.chrome.webview.postMessage(msg);
      return true;
    }
  } catch (e) { }
  return false;
}
if (window.chrome && window.chrome.webview) {
  window.chrome.webview.addEventListener('message', ev => handleHost(ev.data));
}
window.addEventListener('message', ev => { if (ev.data) handleHost(ev.data); });
window.addEventListener('load', () => {
  if (window.__hsfInitField !== undefined) state.hslField = window.__hsfInitField;
  buildBaseSliders();
  buildHslTabsAndChips();
  buildHslSliders();
  initTheme();
  wireStaticControls();
  if (!send({ type: 'ready' })) applyState(defaultState());
});

function handleHost(msg) {
  if (typeof msg === 'string') { try { msg = JSON.parse(msg); } catch (e) { return; } }
  if (!msg || typeof msg !== 'object') return;
  switch (msg.type) {
    case 'init':
      standalone = false;
      applyState(msg);
      send({ type: 'applied', displays: state.displays.length, profiles: state.profiles.length, master: state.master, lut: state.lut });
      break;
    case 'sync':
      standalone = false;
      applyState(msg);
      break;
    case 'engine': state.engineText = msg.text || ''; updateEngineStatus(); break;
    case 'saved': setDirty(false); break;
    case 'dirty':
      if (msg.dirty) {
        const t = msg.target || '当前设置';
        $('#saveBar').hidden = false;
        const st = $('#saveBar').querySelector('.savebar-text');
        if (st) st.textContent = '配置发生改变，是否保存至\n' + t;
      } else {
        $('#saveBar').hidden = true;
      }
      break;
    case 'status':
      if (msg.id === 'perapp') { state.perAppStatus = msg.text || ''; $('#perAppStatus').textContent = state.perAppStatus; }
      if (msg.id === 'hint') $('#hotkeyHint').textContent = msg.text || '';
      break;
    case 'capturing':
      state.capturing = { scope: msg.scope, profile: msg.profile };
      const hint = '请按下要绑定的按键（可带也可不带 Ctrl/Alt/Shift/Win），Esc 取消…';
      $('#hotkeyHint').textContent = hint;
      $('#hotkeyHint').classList.add('hotkey-capturing');
      break;
    case 'picked':
      openBindingModal(null, msg.process, msg.title);
      break;
  }
}

function defaultState() {
  return {
    type: 'init',
    displays: [{ index: 0, label: '主显示器（1920 × 1080）', enabled: false }],
    displayIndex: 0,
    master: false, lut: true, perapp: false,
    autostart: false, autotray: true, globalHotkey: '',
    engine: '滤镜引擎：LUT 逐像素引擎（3D LUT，支持 HSL 调色）',
    profiles: [], activeProfile: -1, bindings: [],
    settings: {
      base: { brightness: 0, contrast: 100, saturation: 100, highlights: 0, shadows: 0, temperature: 0 },
      hsl: { master: { h: 0, s: 100, l: 0 }, channels: CHANNELS.slice(1).map(c => ({ name: c.name, h: 0, s: 100, l: 0 })) },
    },
  };
}

function applyState(p) {
  if (p.settings && p.settings.base) state.settings.base = Object.assign(state.settings.base, p.settings.base);
  if (p.settings && p.settings.hsl) {
    if (p.settings.hsl.master) state.settings.hsl.master = p.settings.hsl.master;
    if (p.settings.hsl.channels) state.settings.hsl.channels = p.settings.hsl.channels;
  }
  state.displays = p.displays || state.displays;
  state.displayIndex = (typeof p.displayIndex === 'number') ? p.displayIndex : 0;
  state.master = !!p.master; state.lut = !!p.lut;
  state.perapp = !!p.perapp;
  state.autostart = !!p.autostart; state.autotray = !!p.autotray;
  state.globalHotkey = p.globalHotkey || '';
  state.engineText = p.engine || state.engineText;
  state.profiles = p.profiles || [];
  state.activeProfile = (typeof p.activeProfile === 'number') ? p.activeProfile : -1;
  state.bindings = p.bindings || [];
  if (state.selectedProfile >= state.profiles.length) state.selectedProfile = -1;
  if (state.selectedBinding >= state.bindings.length) state.selectedBinding = -1;
  state.capturing = null;
  $('#hotkeyHint').textContent = '';
  $('#hotkeyHint').classList.remove('hotkey-capturing');
  $('#perAppStatus').textContent = state.perAppStatus;

  renderDisplays();
  renderProfiles();
  renderBindings();
  applyControls();
  refreshBaseSliders();
  buildHslSliders();
  updateEngineStatus();
  updateHslEnabled();
  updatePerAppEnabled();
  state.initialized = true;
}

function applyControls() {
  $('#masterEnable').checked = state.master;
  $('#displayEnable').checked = state.displays[state.displayIndex] ? state.displays[state.displayIndex].enabled : false;
  $('#displayEnableText').textContent = $('#displayEnable').checked ? '启用（已开）' : '启用（已关）';
  $('#lutSwitch').checked = state.lut;
  $('#perAppSwitch').checked = state.perapp;
  $('#autoStartSwitch').checked = state.autostart;
  $('#autoTraySwitch').checked = state.autotray;
  $('#globalHotkeyBadge').textContent = state.globalHotkey || '未设置';
  const selP = state.selectedProfile >= 0 ? state.profiles[state.selectedProfile] : null;
  $('#profileHotkeyBadge').textContent = selP && selP.hotkey ? selP.hotkey : '—';
}

/* ── 渐变轨道 ── */
const RAINBOW = 'linear-gradient(to right,' +
  [0, 60, 120, 180, 240, 300, 360].map(h => 'hsl(' + h + ',100%,50%)').join(',') + ')';
function hueBandGradient(hue) { return 'linear-gradient(to right, hsl(' + (hue - 30) + ',100%,50%), hsl(' + (hue + 30) + ',100%,50%))'; }
function satGradient(hue) { return 'linear-gradient(to right, hsl(' + hue + ',10%,62%), hsl(' + hue + ',100%,55%))'; }
function lightGradient(hue) { return 'linear-gradient(to right, #000000, hsl(' + hue + ',100%,50%), #FFFFFF)'; }
function paintFill(input, min, max, value, gradient) {
  const pct = (value - min) / (max - min) * 100;
  if (gradient && gradient.startsWith('linear-gradient')) { input.style.background = gradient; return; }
  const css = getComputedStyle(document.body);
  const accent = css.getPropertyValue('--accent').trim() || '#3B82F6';
  const border = css.getPropertyValue('--border').trim() || '#E1E7EF';
  input.style.background = 'linear-gradient(to right,' + accent + ' 0%,' + accent + ' ' + pct + '%,' + border + ' ' + pct + '%,' + border + ' 100%)';
}

/* ── 基础滑块 ── */
function buildBaseSliders() {
  const group = $('#baseSliders');
  group.innerHTML = '';
  BASE_KEYS.forEach(key => {
    const [min, max] = RANGES.base[key];
    const row = document.createElement('div');
    row.className = 'slider-row';
    row.dataset.key = key;
    const label = document.createElement('div');
    label.className = 'slider-label';
    const span = document.createElement('span');
    span.textContent = BASE_LABELS[key];
    const num = document.createElement('input');
    num.type = 'number'; num.className = 'num'; num.value = state.settings.base[key];
    label.append(span, num);
    const input = document.createElement('input');
    input.type = 'range'; input.className = 'flat-range';
    input.min = min; input.max = max; input.value = state.settings.base[key];
    input.dataset.key = key;
    input.dataset.gradient = (key === 'temperature') ? 'linear-gradient(to right,#4A90D9,#FFFFFF 50%,#E8862C)' : '';
    input.addEventListener('input', () => {
      state.settings.base[key] = +input.value;
      paintFill(input, min, max, +input.value, input.dataset.gradient || '');
      num.value = input.value;
      markDirty();
      send({ type: 'base', key, value: +input.value });
    });
    num.addEventListener('change', () => {
      let v = +num.value; if (isNaN(v)) v = +input.value;
      v = Math.max(min, Math.min(max, v));
      input.value = v; state.settings.base[key] = v;
      paintFill(input, min, max, v, input.dataset.gradient || '');
      num.value = v;
      markDirty();
      send({ type: 'base', key, value: v });
    });
    row.append(label, input);
    paintFill(input, min, max, +input.value, input.dataset.gradient || '');
    group.appendChild(row);
  });
}
function refreshBaseSliders() {
  $$('#baseSliders .flat-range').forEach(input => {
    const key = input.dataset.key;
    const [min, max] = RANGES.base[key];
    input.value = state.settings.base[key];
    const num = input.closest('.slider-row').querySelector('.num');
    num.value = state.settings.base[key];
    paintFill(input, min, max, +input.value, input.dataset.gradient || '');
  });
}

/* ── HSL 选项卡 + 色系 ── */
function buildHslTabsAndChips() {
  $('#hslTabs').innerHTML = '';
  ['色相', '饱和度', '明亮度'].forEach((name, i) => {
    const b = document.createElement('button');
    b.className = 'tab' + (i === 0 ? ' active' : '');
    b.dataset.field = i;
    b.textContent = name;
    b.addEventListener('click', () => {
      $$('#hslTabs .tab').forEach(t => t.classList.toggle('active', t === b));
      state.hslField = i;
      buildHslSliders();
    });
    $('#hslTabs').appendChild(b);
  });
  $('#channelChips').innerHTML = '';
  CHANNELS.forEach((c, i) => {
    const b = document.createElement('button');
    b.className = 'chip' + (i === 0 ? ' active' : '');
    b.dataset.channel = i;
    b.innerHTML = '<i style="--c:' + c.color + '"></i>' + c.name;
    b.addEventListener('click', () => {
      $$('.chip').forEach(x => x.classList.toggle('active', x === b));
      $$('#hslSliders .slider-row').forEach(r => r.style.opacity = '1');
      if (i > 0) $$('#hslSliders .slider-row').forEach(r => {
        if (+r.dataset.channel !== i && +r.dataset.channel !== 0) r.style.opacity = '.35';
      });
    });
    $('#channelChips').appendChild(b);
  });
}
function hslVal(channel, field) {
  const key = FIELD_KEYS[field];
  return channel === 0 ? state.settings.hsl.master[key] : state.settings.hsl.channels[channel - 1][key];
}
function setHslVal(channel, field, value) {
  const key = FIELD_KEYS[field];
  if (channel === 0) state.settings.hsl.master[key] = value;
  else state.settings.hsl.channels[channel - 1][key] = value;
}
function hslRange(channel, field) {
  if (field === 0) return channel === 0 ? RANGES.hsl.hueMaster : RANGES.hsl.hueChannel;
  if (field === 1) return RANGES.hsl.sat;
  return RANGES.hsl.light;
}
function hslGradient(channel, field) {
  const hue = CHANNELS[channel].hue;
  if (field === 0) return channel === 0 ? RAINBOW : hueBandGradient(hue);
  if (field === 1) return satGradient(hue);
  return lightGradient(hue);
}
function fmtHsl(value, field) {
  return Math.round(value);          // 饱和度显示原值 0..200（默认 100）
}
function parseHslNum(text, field) {
  const v = parseFloat(text);
  return isNaN(v) ? null : v;
}
function buildHslSliders() {
  const list = $('#hslSliders');
  list.innerHTML = '';
  const field = state.hslField;
  for (let c = 0; c < 9; c++) {
    const [min, max] = hslRange(c, field);
    const val = hslVal(c, field);
    const row = document.createElement('div');
    row.className = 'slider-row';
    row.dataset.channel = c;
    const label = document.createElement('div');
    label.className = 'slider-label';
    const span = document.createElement('span');
    span.innerHTML = '<i style="display:inline-block;width:8px;height:8px;border-radius:50%;background:' + CHANNELS[c].color + ';margin-right:6px"></i>' + CHANNELS[c].name;
    const num = document.createElement('input');
    num.type = 'number'; num.className = 'num'; num.value = fmtHsl(val, field);
    label.append(span, num);
    const input = document.createElement('input');
    input.type = 'range'; input.className = 'flat-range';
    input.min = min; input.max = max; input.value = val;
    input.dataset.channel = c; input.dataset.field = field;
    input.addEventListener('input', () => {
      setHslVal(c, field, +input.value);
      input.style.background = hslGradient(c, field);
      num.value = fmtHsl(+input.value, field);
      markDirty();
      send({ type: 'hsl', channel: c, field: FIELD_KEYS[field], value: +input.value });
    });
    num.addEventListener('change', () => {
      let v = parseHslNum(num.value, field);
      if (v === null) v = +input.value;
      v = Math.max(min, Math.min(max, v));
      setHslVal(c, field, v);
      input.value = v; input.style.background = hslGradient(c, field);
      num.value = fmtHsl(v, field);
      markDirty();
      send({ type: 'hsl', channel: c, field: FIELD_KEYS[field], value: v });
    });
    row.append(label, input);
    input.style.background = hslGradient(c, field);
    list.appendChild(row);
  }
  updateHslEnabled();
}

/* ── 显示器 / 配置 / 绑定 渲染 ── */
function renderDisplays() {
  const sel = $('#displaySelect');
  sel.innerHTML = '';
  state.displays.forEach((d, i) => {
    const o = document.createElement('option');
    o.value = i; o.textContent = d.label;
    sel.appendChild(o);
  });
  sel.selectedIndex = state.displayIndex;
}
function renderProfiles() {
  const wrap = $('#profileRows');
  wrap.innerHTML = '';
  if (!state.profiles.length) { $('#profileEmpty').hidden = false; return; }
  $('#profileEmpty').hidden = true;
  state.profiles.forEach((p, i) => {
    const on = i === state.activeProfile;
    const sel = i === state.selectedProfile;
    const row = document.createElement('div');
    row.className = 'table-row' + (sel ? ' selected' : '');
    row.dataset.profile = i;
    const engine = p.engine === 'LUT' ? '<span class="tag tag-blue">LUT</span>' : '<span class="tag">放大镜</span>';
    row.innerHTML =
      '<span class="col-name"><i class="radio-dot' + (on ? ' on' : '') + '"></i><span class="pname"></span></span>' +
      '<span class="col-engine">' + engine + '</span>' +
      '<span class="col-hotkey"><span class="hotkey-key">' + (p.hotkey || '—') + '</span></span>' +
      '<span class="col-check"><button class="btn btn-enable' + (on ? ' active' : '') + '" data-profile="' + i + '">' + (on ? '已启用' : '启用') + '</button></span>';
    row.querySelector('.pname').textContent = p.name;
    row.addEventListener('click', () => { state.selectedProfile = i; renderProfiles(); });
    wrap.appendChild(row);
  });
  $$('#profileRows .btn-enable').forEach(b => b.addEventListener('click', ev => {
    ev.stopPropagation();
    const i = +b.dataset.profile;
    state.activeProfile = i;
    state.selectedProfile = i;
    renderProfiles();
    send({ type: 'profile-activate', index: i });
  }));
  applyControls();
}
function renderBindings() {
  const wrap = $('#bindingRows');
  wrap.innerHTML = '';
  if (!state.bindings.length) { $('#bindingEmpty').hidden = false; return; }
  $('#bindingEmpty').hidden = true;
  state.bindings.forEach((b, i) => {
    const sel = i === state.selectedBinding;
    const row = document.createElement('div');
    row.className = 'table-row' + (sel ? ' selected' : '');
    row.dataset.binding = i;
    row.innerHTML = '<span class="col-app"><i class="proc-ico"></i><span class="bname"></span></span>' +
                    '<span class="col-cfg"><span class="tag tag-green">' + esc(b.target) + '</span></span>';
    row.querySelector('.bname').textContent = b.name;
    row.addEventListener('click', () => { state.selectedBinding = i; renderBindings(); });
    wrap.appendChild(row);
  });
}
function esc(s) { return String(s).replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c])); }

/* ── 模态框 ── */
function openModal(title, fieldsHtml, onOk, okText) {
  $('#modalTitle').textContent = title;
  $('#modalBody').innerHTML = fieldsHtml;
  $('#modalOk').textContent = okText || '确定';
  $('#modalOverlay').classList.add('show');
  $('#modalOk').onclick = () => { $('#modalOverlay').classList.remove('show'); onOk(); };
  $('#modalCancel').onclick = () => $('#modalOverlay').classList.remove('show');
  const first = $('#modalBody input, #modalBody select');
  if (first) first.focus();
  if (first && first.select) first.select();
}
function openNameModal(title, initial, onOk) {
  openModal(title,
    '<div class="modal-field"><label>配置名称</label><input id="modalInput" type="text" value="' + esc(initial) + '"></div>',
    () => onOk($('#modalInput').value.trim()), '确定');
}
function openBindingModal(binding, presetProcess, presetTitle) {
  const b = binding || { process: presetProcess || '', title: presetTitle || '', profile: 0 };
  const opts = state.profiles.map((p, i) => '<option value="' + i + '">' + esc(p.name) + '</option>').join('');
  const profileIdx = Math.min(b.profile, Math.max(0, state.profiles.length - 1));
  openModal('添加检测应用',
    '<div class="modal-field"><label>进程名（如 chrome.exe）</label><input id="mProcess" type="text" value="' + esc(b.process) + '"></div>' +
    '<div class="modal-field"><label>窗口标题包含（可选）</label><input id="mTitle" type="text" value="' + esc(b.title) + '"></div>' +
    '<div class="modal-field"><label>绑定配置</label><select id="mProfile">' + opts + '</select></div>',
    () => onOk({
      process: $('#mProcess').value.trim(),
      title: $('#mTitle').value.trim(),
      profile: +$('#mProfile').value,
    }), '添加');
  const sel = $('#mProfile');
  if (sel) sel.selectedIndex = profileIdx;
}
function confirmModal(text, onOk) {
  openModal('确认', '<div class="modal-hint">' + esc(text) + '</div>', onOk, '删除');
}

/* ── 引擎状态 / 依赖开关 ── */
function updateEngineStatus() {
  const on = state.master;
  const el = $('#engineStatus');
  el.querySelector('.dot').style.background = on ? 'var(--green)' : 'var(--orange)';
  const base = state.engineText || '滤镜引擎';
  el.lastChild.textContent = ' ' + (on ? base : '滤镜引擎：已关闭');
}
function updateHslEnabled() {
  const on = state.lut;
  $$('#hslSliders .flat-range, #hslSliders .num, #hslTabs .tab, .chip').forEach(el => {
    el.disabled = !on; el.style.opacity = on ? '' : '.45';
  });
  $('#hslReset').disabled = !on;
  $('#hslHint').textContent = on
    ? '已启用 LUT 引擎：8 个色系可分别精确调整、互不干扰（64³ 3D LUT 逐像素着色器）。'
    : '提示：关闭 LUT 引擎后使用放大镜引擎，暂不支持分色系 HSL。';
}
function updatePerAppEnabled() {
  const on = state.perapp;
  $$('#bindingList .table-row, #bindingList .table-head').forEach(el => el.style.opacity = on ? '' : '.45');
  ['#btnBindingAdd', '#btnBindingPick', '#btnBindingEdit', '#btnBindingDel'].forEach(id => { $(id).disabled = !on; });
  $('#perAppStatus').textContent = on ? state.perAppStatus : '○ 按应用切换未启用';
}

/* ── 主题（纯 UI）── */
const media = window.matchMedia('(prefers-color-scheme: dark)');
function initTheme() {
  state.theme = (document.body.dataset.theme === 'light' || document.body.dataset.theme === 'dark')
    ? document.body.dataset.theme : 'auto';
  $('#themeSelect').value = state.theme;
  applyTheme();
}
function applyTheme() {
  const eff = state.theme === 'auto' ? (media.matches ? 'dark' : 'light') : state.theme;
  document.body.dataset.theme = eff;
  $$('#baseSliders .flat-range').forEach(input => paintFill(input, +input.min, +input.max, +input.value, input.dataset.gradient || ''));
  $$('#hslSliders .flat-range').forEach(input => { input.style.background = hslGradient(+input.dataset.channel, +input.dataset.field); });
}

/* ── 静态控件绑定 ── */
function wireStaticControls() {
  $$('.nav-item').forEach(b => b.addEventListener('click', () => {
    $$('.nav-item').forEach(x => x.classList.toggle('active', x === b));
    $$('.page').forEach(p => p.classList.toggle('active', p.id === 'page-' + b.dataset.page));
  }));

  const SWITCH_IDS = {
    masterEnable: 'master', lutSwitch: 'lut',
    perAppSwitch: 'perapp',
    autoStartSwitch: 'autostart', autoTraySwitch: 'autotray', displayEnable: 'display',
  };
  const wire = (id, key) => {
    const el = $('#' + id);
    if (!el) { console.error('[demo2] missing element: ' + id); return; }
    el.addEventListener('change', () => {
      const v = el.checked;
      if (id === 'displayEnable') { state.displays[state.displayIndex].enabled = v; $('#displayEnableText').textContent = v ? '启用（已开）' : '启用（已关）'; }
      else state[key] = v;
      if (id === 'lutSwitch') updateHslEnabled();
      if (id === 'perAppSwitch') updatePerAppEnabled();
      if (id === 'masterEnable') updateEngineStatus();
      markDirty();
      send({ type: 'switch', id: SWITCH_IDS[id] || id, value: v });
    });
  };
  wire('masterEnable', 'master'); wire('lutSwitch', 'lut');
  wire('perAppSwitch', 'perapp');
  wire('autoStartSwitch', 'autostart'); wire('autoTraySwitch', 'autotray');
  wire('displayEnable', 'displayEnabled');

  $('#displaySelect').addEventListener('change', () => {
    state.displayIndex = +$('#displaySelect').value;
    send({ type: 'display', index: state.displayIndex });
  });
  $('#themeSelect').addEventListener('change', e => { state.theme = e.target.value; applyTheme(); });
  media.addEventListener('change', () => { if (state.theme === 'auto') applyTheme(); });

  // 快捷预设（数值与原项目一致）
  const PRESETS = {
    default: { brightness: 0, contrast: 100, saturation: 100, highlights: 0, shadows: 0, temperature: 0 },
    eye:     { brightness: -5, contrast: 95, saturation: 95, highlights: 0, shadows: 0, temperature: 25 },
    night:   { brightness: -40, contrast: 100, saturation: 90, highlights: 0, shadows: 0, temperature: 45 },
    vivid:   { brightness: 0, contrast: 110, saturation: 150, highlights: 0, shadows: 0, temperature: 0 },
  };
  $$('.preset').forEach(b => b.addEventListener('click', () => {
    const p = PRESETS[b.dataset.preset];
    if (!p) return;
    state.settings.base = Object.assign({}, p);
    refreshBaseSliders();
    markDirty();
    send({ type: 'preset', name: b.dataset.preset });
  }));

  $('#hslReset').addEventListener('click', () => {
    state.settings.hsl.master = { h: 0, s: 100, l: 0 };
    state.settings.hsl.channels = CHANNELS.slice(1).map(c => ({ name: c.name, h: 0, s: 100, l: 0 }));
    buildHslSliders();
    markDirty();
    send({ type: 'hsl-reset' });
  });

  // 保存 / 取消
  $('#btnSaveYes').addEventListener('click', () => send({ type: 'save' }));
  $('#btnSaveNo').addEventListener('click', () => send({ type: 'cancel' }));

  // ── 配置管理 ──
  $('#btnProfileNew').addEventListener('click', () => {
    openNameModal('新建配置', '配置 ' + (state.profiles.length + 1), name => {
      if (name) send({ type: 'profile', action: 'new', name });
    });
  });
  $('#btnProfileRename').addEventListener('click', () => {
    const i = state.selectedProfile;
    if (i < 0) { $('#hotkeyHint').textContent = '请先在列表中选择一个配置'; return; }
    openNameModal('重命名配置', state.profiles[i].name, name => {
      if (name) send({ type: 'profile', action: 'rename', index: i, name });
    });
  });
  $('#btnProfileDel').addEventListener('click', () => {
    const i = state.selectedProfile;
    if (i < 0) { $('#hotkeyHint').textContent = '请先在列表中选择一个配置'; return; }
    confirmModal('确定删除配置「' + state.profiles[i].name + '」？', () => send({ type: 'profile', action: 'delete', index: i }));
  });
  $('#btnProfileUp').addEventListener('click', () => {
    const i = state.selectedProfile;
    if (i > 0) send({ type: 'profile', action: 'move', index: i, delta: -1 });
  });
  $('#btnProfileDown').addEventListener('click', () => {
    const i = state.selectedProfile;
    if (i >= 0 && i < state.profiles.length - 1) send({ type: 'profile', action: 'move', index: i, delta: 1 });
  });
  $('#btnProfileImport').addEventListener('click', () => send({ type: 'profile', action: 'import' }));
  $('#btnProfileExport').addEventListener('click', () => {
    const i = state.selectedProfile;
    if (i < 0) { $('#hotkeyHint').textContent = '请先在列表中选择一个配置'; return; }
    send({ type: 'profile', action: 'export', index: i });
  });

  // ── 快捷键 ──
  $('#btnHotkeySet').addEventListener('click', () => {
    const i = state.selectedProfile;
    if (i < 0) { $('#hotkeyHint').textContent = '请先在列表中选择一个配置'; return; }
    send({ type: 'hotkey', action: 'capture', scope: 'profile', profile: i });
  });
  $('#btnHotkeyClear').addEventListener('click', () => {
    const i = state.selectedProfile;
    if (i < 0) { $('#hotkeyHint').textContent = '请先在列表中选择一个配置'; return; }
    send({ type: 'hotkey', action: 'clear', scope: 'profile', profile: i });
  });
  $('#btnGlobalHotkey').addEventListener('click', () => send({ type: 'hotkey', action: 'capture', scope: 'global' }));
  $('#btnGlobalHotkeyClear').addEventListener('click', () => send({ type: 'hotkey', action: 'clear', scope: 'global' }));

  // ── 按应用切换 ──
  $('#btnBindingAdd').addEventListener('click', () => openBindingModal(null));
  $('#btnBindingPick').addEventListener('click', () => {
    const banner = document.createElement('div');
    banner.className = 'countdown-banner';
    banner.innerHTML = '<div class="cd-num">3</div><div class="cd-text">秒后捕获当前前台应用…</div>';
    $('#app').appendChild(banner);
    let n = 3;
    const timer = setInterval(() => {
      n--;
      if (n > 0) banner.querySelector('.cd-num').textContent = n;
      else { clearInterval(timer); banner.remove(); send({ type: 'binding', action: 'pick' }); }
    }, 1000);
  });
  $('#btnBindingEdit').addEventListener('click', () => {
    const i = state.selectedBinding;
    if (i < 0) { $('#perAppStatus').textContent = '请先在列表中选择要编辑的应用'; return; }
    openBindingModal({ process: state.bindings[i].process, title: state.bindings[i].title, profile: state.bindings[i].profile });
  });
  $('#btnBindingDel').addEventListener('click', () => {
    const i = state.selectedBinding;
    if (i < 0) { $('#perAppStatus').textContent = '请先在列表中选择要删除的应用'; return; }
    confirmModal('确定删除该检测应用？', () => send({ type: 'binding', action: 'delete', index: i }));
  });

  // 调试日志
  $('#btnDebug').addEventListener('click', () => send({ type: 'log' }));

  // 其余占位按钮闪烁
  $$('.btn').forEach(b => b.addEventListener('click', () => {
    if (b.classList.contains('preset') || b.classList.contains('btn-enable')) return;
    if (['btnSaveYes','btnSaveNo','btnDebug','btnGlobalHotkey','btnHotkeySet','btnHotkeyClear',
          'btnProfileNew','btnProfileRename','btnProfileDel','btnProfileUp','btnProfileDown',
          'btnProfileImport','btnProfileExport','btnBindingAdd','btnBindingPick','btnBindingEdit','btnBindingDel','btnGlobalHotkeyClear',
          'modalOk','modalCancel'].includes(b.id)) return;
    flash(b);
  }));
}

function markDirty() { /* 保存弹窗改由宿主判定（对比保存快照） */ }
function setDirty(on) { state.dirty = on; $('#saveBar').hidden = !on; }
function flash(btn) {
  if (!btn) return;
  btn.style.transition = 'none'; btn.style.background = 'var(--accent)'; btn.style.color = '#fff';
  setTimeout(() => { btn.style.transition = ''; btn.style.background = ''; btn.style.color = ''; }, 160);
}
