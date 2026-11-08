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
    darkScene: [0, 100],
    sharpen: [0, 100], noiseReduction: [0, 100], edgeEnhancement: [0, 100],
    clarity: [0, 100], qualityEnhancement: [0, 100],
  },
  hsl: {
    hueMaster: [-180, 180], hueChannel: [-30, 30],
    sat: [0, 200], satBase: 100,
    light: [-30, 30],
  },
};
const BASE_KEYS = ['brightness', 'contrast', 'saturation', 'highlights', 'shadows', 'temperature', 'darkScene'];
const ADVANCED_KEYS = ['sharpen', 'noiseReduction', 'edgeEnhancement', 'clarity', 'qualityEnhancement'];
const BASE_LABELS = {
  brightness: '亮度 Brightness', contrast: '对比度 Contrast', saturation: '鲜艳度 Saturation',
  highlights: '高光 Highlights', shadows: '阴影 Shadows', temperature: '色温（冷 ← → 暖）',
  darkScene: '暗场景优化 Dark Scene（暗部提亮·保色相）',
};
const ADVANCED_LABELS = { sharpen: '锐化 Sharpen', noiseReduction: '降噪 Noise Reduction', edgeEnhancement: '边缘增强 Edge Enhancement', clarity: '清晰度 Clarity', qualityEnhancement: '画质增强 Quality Enhancement' };
// 参数默认值的本地兜底（独立打开 index.html、尚未收到宿主 init 时使用）。
const NEUTRAL_BASE = {
  brightness: 0, contrast: 100, saturation: 100, highlights: 0, shadows: 0,
  temperature: 0, darkScene: 0, sharpen: 0, noiseReduction: 0,
  edgeEnhancement: 0, clarity: 0, qualityEnhancement: 0,
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
    base: { brightness: 0, contrast: 100, saturation: 100, highlights: 0, shadows: 0, temperature: 0, darkScene: 0, sharpen: 0, noiseReduction: 0, edgeEnhancement: 0, clarity: 0, qualityEnhancement: 0 },
    hsl: { master: { h: 0, s: 100, l: 0 }, channels: CHANNELS.slice(1).map(c => ({ name: c.name, h: 0, s: 100, l: 0 })) },
  },
  hslField: 0,
  hslChannel: 0,
  displays: [], displayIndex: 0,
  master: false, lut: false, vsync: false, perapp: false,
  vsyncForced: true,      // 滤镜层强制垂直同步（宿主下发；界面开关显示为开且锁住）
  lutActive: false,
  hdrColor: false,        // 显示器是否处于 HDR（仅用于解释「为什么 LUT 被锁成开」）
  lutForced: false,
  showFps: false, fps: -1,
  autostart: false, autotray: true,
  globalHotkey: '', engineText: '',
  profiles: [], activeProfile: -1, bindings: [],
  selectedProfile: -1, selectedBinding: -1,
  dirty: false, theme: 'auto',
  capturing: null,        // {scope, profile}
  perAppStatus: '○ 按应用切换未启用',
  paramsSig: '',          // 宿主下发的参数表签名（变化时才重建滑块）
  paramDefaults: {},      // 宿主下发的每个基础参数默认值
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
  buildAdvancedSliders();
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
    case 'fps': state.fps = (typeof msg.fps === 'number') ? msg.fps : -1; updateFpsBadge(); break;
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
    master: false, lut: true, vsync: false, perapp: false,
    showFps: false,
    autostart: false, autotray: true, globalHotkey: '',
    engine: '滤镜引擎：LUT 逐像素引擎',
    profiles: [], activeProfile: -1, bindings: [],
    settings: {
      base: { brightness: 0, contrast: 100, saturation: 100, highlights: 0, shadows: 0, temperature: 0, darkScene: 0, sharpen: 0, noiseReduction: 0, edgeEnhancement: 0, clarity: 0, qualityEnhancement: 0 },
      hsl: { master: { h: 0, s: 100, l: 0 }, channels: CHANNELS.slice(1).map(c => ({ name: c.name, h: 0, s: 100, l: 0 })) },
    },
  };
}

/* 宿主下发的参数表是唯一事实来源：范围/默认值/分组/标签都在 C++ models.cpp 定义。
 * 旧实现网页与宿主各自维护一份范围表，扩大范围时只改了一边就会「界面能拖、
 * 宿主静默截断」或「宿主接受了、网页又按旧范围显示」。 */
function adoptParamDefs(defs) {
  if (!Array.isArray(defs) || !defs.length) return false;
  const sig = JSON.stringify(defs);
  if (sig === state.paramsSig) return false;
  state.paramsSig = sig;
  const baseKeys = [], advKeys = [];
  defs.forEach(d => {
    if (!d || typeof d.key !== 'string') return;
    const min = (typeof d.min === 'number') ? d.min : 0;
    const max = (typeof d.max === 'number') ? d.max : 100;
    const def = (typeof d.default === 'number') ? d.default : 0;
    RANGES.base[d.key] = [min, max];
    state.paramDefaults[d.key] = def;
    if (!(d.key in state.settings.base)) state.settings.base[d.key] = def;
    if (d.advanced) {
      if (!ADVANCED_LABELS[d.key] && d.label) ADVANCED_LABELS[d.key] = d.label;
      advKeys.push(d.key);
    } else {
      if (d.label) BASE_LABELS[d.key] = d.label;
      baseKeys.push(d.key);
    }
  });
  if (baseKeys.length) { BASE_KEYS.length = 0; BASE_KEYS.push(...baseKeys); }
  if (advKeys.length) { ADVANCED_KEYS.length = 0; ADVANCED_KEYS.push(...advKeys); }
  return true;
}

function applyState(p) {
  const schemaChanged = adoptParamDefs(p.params);
  if (p.settings && p.settings.base) state.settings.base = Object.assign(state.settings.base, p.settings.base);
  if (p.settings && p.settings.hsl) {
    if (p.settings.hsl.master) state.settings.hsl.master = p.settings.hsl.master;
    if (p.settings.hsl.channels) state.settings.hsl.channels = p.settings.hsl.channels;
  }
  state.displays = p.displays || state.displays;
  state.displayIndex = (typeof p.displayIndex === 'number') ? p.displayIndex : 0;
  state.master = !!p.master; state.lut = !!p.lut;
  state.lutActive = !!p.lutActive;
  state.lutForced = !!p.lutForced;
  if (typeof p.showFps === 'boolean') state.showFps = p.showFps;
  state.vsync = !!p.vsync;
  if (typeof p.vsyncForced === 'boolean') state.vsyncForced = p.vsyncForced;
  state.hdrColor = !!p.hdr;   // 显示器是否处于 HDR（只影响是否强制 LUT 引擎）
  state.perapp = !!p.perapp;
  state.autostart = !!p.autostart; state.autotray = !!p.autotray;
  state.globalHotkey = p.globalHotkey || '';
  state.engineText = p.engine || state.engineText;
  // 界面主题由宿主持久化（profiles.json 的 Theme）；旧配置的 "default" 等同 auto。
  if (typeof p.theme === 'string') state.theme = p.theme;
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
  if (schemaChanged) {
    buildBaseSliders();
    buildAdvancedSliders();
  } else {
    refreshBaseSliders();
    refreshAdvancedSliders();
  }
  buildHslSliders();
  updateEngineStatus();
  updateHslEnabled();
  updatePerAppEnabled();
  syncThemeUI();
  state.initialized = true;
}

function applyControls() {
  $('#masterEnable').checked = state.master;
  $('#displayEnable').checked = state.displays[state.displayIndex] ? state.displays[state.displayIndex].enabled : false;
  $('#displayEnableText').textContent = $('#displayEnable').checked ? '启用（已开）' : '启用（已关）';
  $('#lutSwitch').checked = state.lut;
  // HDR 下 LUT 强制开启：显示为开且不允许关闭（放大镜/伽马对 HDR 内容无效）
  $('#lutSwitch').disabled = state.lutForced;
  const lutRow = $('#lutSwitch').closest('.toggle-row');
  if (lutRow) lutRow.title = state.lutForced ? 'HDR 下自动启用 LUT 引擎，无法关闭（放大镜/伽马对 HDR 内容无效）' : '';
  const lutLabel = lutRow ? lutRow.querySelector('.toggle-label') : null;
  if (lutLabel) lutLabel.textContent = state.lutForced ? 'LUT 引擎开关（HDR 下自动启用，不可关闭）' : 'LUT 引擎开关';
  // 滤镜层强制垂直同步：显示为「开」并锁住
  //（窗口模式 Present(1)：由 DWM 把每一帧对齐到下一个 vblank，不用帧延迟等待对象）
  $('#vsyncSwitch').checked = state.vsyncForced || state.vsync;
  $('#vsyncSwitch').disabled = state.vsyncForced;
  {
    const row = $('#vsyncSwitch').closest('.toggle-row');
    if (row) row.title = state.vsyncForced ? '滤镜层固定与显示器刷新对齐（强制垂直同步），不可关闭' : '';
    const label = row ? row.querySelector('.toggle-label') : null;
    if (label) label.textContent = state.vsyncForced ? '垂直同步 V-Sync（强制开启）' : '垂直同步 V-Sync';
  }
  const badge = $('#colorModeBadge');
  // 色彩管道固定：DXGI 桌面复制 + SDR（B8G8R8A8）输入输出，不做 HDR 色彩转换。
  badge.textContent = 'DXGI 桌面复制 · SDR';
  badge.title = '滤镜用 DXGI 桌面复制取 SDR 画面（B8G8R8A8），LUT 后原样输出；'
              + 'HDR 显示器上由 Windows 按「SDR 内容亮度」把覆盖层映射到 HDR 信号。';
  badge.classList.toggle('hdr-on', state.hdrColor);
  $('#perAppSwitch').checked = state.perapp;
  $('#autoStartSwitch').checked = state.autostart;
  $('#autoTraySwitch').checked = state.autotray;
  $('#fpsSwitch').checked = state.showFps;
  updateFpsBadge();
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
    const [min, max] = RANGES.base[key] || [0, 100];
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
    const [min, max] = RANGES.base[key] || [0, 100];
    input.value = state.settings.base[key];
    const num = input.closest('.slider-row').querySelector('.num');
    num.value = state.settings.base[key];
    paintFill(input, min, max, +input.value, input.dataset.gradient || '');
  });
}

/* ── 高级调整（LUT 后处理）── */
function buildAdvancedSliders() {
  const group = $('#advancedSliders');
  group.innerHTML = '';
  ADVANCED_KEYS.forEach(key => {
    const [min, max] = RANGES.base[key] || [0, 100];
    const row = document.createElement('div');
    row.className = 'slider-row';
    row.dataset.key = key;
    const label = document.createElement('div');
    label.className = 'slider-label';
    const span = document.createElement('span');
    span.textContent = ADVANCED_LABELS[key];
    const num = document.createElement('input');
    num.type = 'number'; num.className = 'num';
    label.append(span, num);
    const input = document.createElement('input');
    input.type = 'range'; input.className = 'flat-range';
    input.min = min; input.max = max;
    input.dataset.key = key;
    const value = state.settings.base[key] || 0;
    input.value = value; num.value = value;
    const update = v => {
      state.settings.base[key] = v; input.value = v; num.value = v;
      paintFill(input, min, max, v, '');
      markDirty();
      send({ type: 'base', key, value: v });
    };
    input.addEventListener('input', () => update(+input.value));
    num.addEventListener('change', () => {
      let v = +num.value; if (isNaN(v)) v = +input.value;
      update(Math.max(min, Math.min(max, v)));
    });
    row.append(label, input);
    paintFill(input, min, max, value, '');
    group.appendChild(row);
  });
}
function refreshAdvancedSliders() {
  $$('#advancedSliders .flat-range').forEach(input => {
    const key = input.dataset.key;
    const [min, max] = RANGES.base[key] || [0, 100];
    const value = state.settings.base[key] || 0;
    input.value = value;
    const num = input.closest('.slider-row').querySelector('.num');
    num.value = value;
    paintFill(input, min, max, value, '');
  });
  // 以「LUT 引擎真的在跑」为准（开关打开但引擎回退到放大镜/伽马时仍应禁用）
  const enabled = !!state.lutActive;
  // 注意：禁用样式挂在 .advanced-block 上（不是 #advancedSliders），之前挂错层导致看起来仍可调
  const block = $('#advancedSliders') ? $('#advancedSliders').closest('.advanced-block') : null;
  if (block) block.classList.toggle('advanced-disabled', !enabled);
  $$('#advancedSliders input').forEach(el => { el.disabled = !enabled; });
  if (block) block.style.opacity = enabled ? '' : '0.45';
  const hint = $('#advancedHint');
  if (hint) hint.textContent = enabled
    ? '高级画质处理已启用，将在 LUT 后处理阶段生效。'
    : '提示：请先启用 LUT 引擎，高级调整才能生效。当前参数仍会保存。';
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
      state.hslChannel = i;
      applyHslChipFocus();
    });
    $('#channelChips').appendChild(b);
  });
  applyHslChipFocus();
}
// 色系 chip 的“聚焦”状态要在切换字段（重建滑块）后仍然保持：
// 旧实现只在点击时改一次 opacity，切换色相/饱和度/明亮度后全部滑块又变回 100%。
function applyHslChipFocus() {
  const active = state.hslChannel | 0;
  $$('#channelChips .chip').forEach((x, idx) => x.classList.toggle('active', idx === active));
  $$('#hslSliders .slider-row').forEach(r => {
    const c = +r.dataset.channel;
    r.style.opacity = (active === 0 || c === 0 || c === active) ? '1' : '.35';
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
  applyHslChipFocus();
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
    const activate = () => {
      state.activeProfile = i;
      state.selectedProfile = i;
      renderProfiles();
      send({ type: 'profile-activate', index: i });
    };
    const prof = state.profiles[i];
    // 启用 LUT 配置同样会启动逐像素引擎：和手动拨 LUT 开关一样先提示性能损耗
    if (prof && prof.engine === 'LUT' && !state.lut) {
      confirmLutEnable(activate, '配置「' + prof.name + '」使用 LUT 引擎');
      return;
    }
    activate();
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
function openModal(title, fieldsHtml, onOk, okText, cancelText) {
  $('#modalTitle').textContent = title;
  $('#modalBody').innerHTML = fieldsHtml;
  $('#modalOk').textContent = okText || '确定';
  $('#modalCancel').textContent = cancelText || '取消';
  $('#modalOverlay').classList.add('show');
  $('#modalOk').onclick = () => { $('#modalOverlay').classList.remove('show'); onOk(); };
  $('#modalCancel').onclick = () => $('#modalOverlay').classList.remove('show');
  const first = $('#modalBody input, #modalBody select');
  if (first) {
    first.focus();
    // 只有文本/数字输入和 select 支持 select()；复选框调用会抛 InvalidStateError
    if (first.tagName === 'SELECT' || first.type === 'text' || first.type === 'number') first.select();
  }
}
function openNameModal(title, initial, onOk) {
  openModal(title,
    '<div class="modal-field"><label>配置名称</label><input id="modalInput" type="text" value="' + esc(initial) + '"></div>',
    () => onOk($('#modalInput').value.trim()), '确定');
}
function openBindingModal(binding, presetProcess, presetTitle) {
  const isEdit = !!binding;
  const b = binding || { process: presetProcess || '', title: presetTitle || '', profile: 0 };
  const opts = '<option value="-1">按当前设置</option>' +
    state.profiles.map((p, i) => '<option value="' + i + '">' + esc(p.name) + '</option>').join('');
  // 第一项是「按当前设置」(profile=-1)，配置索引在列表中整体 +1
  const profileIdx = Math.max(-1, Math.min(b.profile, state.profiles.length - 1));
  openModal(isEdit ? '编辑检测应用' : '添加检测应用',
    '<div class="modal-field"><label>进程名（如 chrome.exe）</label><input id="mProcess" type="text" value="' + esc(b.process) + '"></div>' +
    '<div class="modal-field"><label>窗口标题包含（可选）</label><input id="mTitle" type="text" value="' + esc(b.title) + '"></div>' +
    '<div class="modal-field"><label>绑定配置</label><select id="mProfile">' + opts + '</select></div>',
    () => {
      // 直接发送绑定消息（此前的实现引用了一个不存在的 onOk 参数，
      // 导致点击“添加”抛 ReferenceError、消息从未发出、列表永远为空）
      const process = $('#mProcess').value.trim();
      const title = $('#mTitle').value.trim();
      const profile = +$('#mProfile').value;
      if (isEdit) send({ type: 'binding', action: 'edit', index: state.selectedBinding, process: process, title: title, profile: profile });
      else send({ type: 'binding', action: 'add', process: process, title: title, profile: profile });
    }, '确定');
  const sel = $('#mProfile');
  if (sel) sel.selectedIndex = profileIdx + 1;
}
function confirmModal(text, onOk) {
  openModal('确认', '<div class="modal-hint">' + esc(text) + '</div>', onOk, '删除');
}

/* ── LUT 性能损耗确认 ──
 * LUT 滤镜不是普通调色：它每帧用 DXGI 桌面复制抓整个桌面，再对每个像素做
 * 64³ 3D LUT 采样（高级调整还会叠 8 次邻域采样），GPU 占用/功耗远高于放大镜。
 * 用户主动启用 LUT（开关 / LUT 配置 / LUT 预设）前必须明确询问一次；
 * 勾选「本次运行不再提示」后同一进程内不再重复弹窗。 */
let lutWarningAcknowledged = false;
const LUT_PRESETS = ['hdr', 'cinematic', 'landscape', 'portrait', 'cyber'];
function lutWarningHtml(reason) {
  return '<div class="modal-hint modal-warning">' +
    '<p><b>LUT 滤镜会持续占用 GPU 做全屏逐像素处理，性能损耗明显。</b></p>' +
    '<p>' + esc(reason || '') + '</p>' +
    '<ul>' +
    '<li>每帧通过 DXGI 桌面复制抓取整个桌面（2K/4K 下每帧几十 MB 显存流量）；</li>' +
    '<li>每个像素做 64³ 3D LUT 三线性采样，叠加锐化/降噪/清晰度时还要 8 次邻域采样；</li>' +
    '<li>高分辨率 + 高刷新率、核显/低端独显、笔记本电池模式下尤其明显，游戏帧率可能下降；</li>' +
    '<li>HDR 显示器上 LUT 会被强制启用（放大镜/伽马对 HDR 内容无效）。</li>' +
    '</ul>' +
    '<p>只是护眼/基础调色时建议改用「放大镜」引擎；需要分色系 HSL、锐化/降噪等高级效果时再启用 LUT。</p>' +
    '<label class="modal-check"><input type="checkbox" id="lutWarnDontAsk"> 本次运行不再提示</label>' +
    '</div>';
}
function confirmLutEnable(onOk, reason) {
  if (lutWarningAcknowledged) { onOk(); return; }
  openModal('启用 LUT 滤镜？', lutWarningHtml(reason), () => {
    const cb = $('#lutWarnDontAsk');
    if (cb && cb.checked) lutWarningAcknowledged = true;
    onOk();
  }, '仍然启用', '保持关闭');
}

/* ── 引擎状态 / 依赖开关 ── */
function updateEngineStatus() {
  const on = state.master;
  const el = $('#engineStatus');
  el.querySelector('.dot').style.background = on ? 'var(--green)' : 'var(--orange)';
  const base = state.engineText || '滤镜引擎';
  const txt = $('#engineText') || el.lastChild;
  if (txt) txt.textContent = on ? base : '滤镜引擎：已关闭';
}
/* 实时帧率（标题栏右上角）：开关关闭时隐藏；fps < 0 = 当前没有在跑的 LUT 引擎 */
function updateFpsBadge() {
  const el = $('#fpsBadge');
  if (!el) return;
  el.hidden = !state.showFps;
  if (!state.showFps) return;
  el.textContent = state.fps >= 0 ? state.fps.toFixed(1) + ' FPS' : '— FPS';
}
function updateHslEnabled() {
  // 不能只看开关：LUT 引擎启动失败时宿主会回退到放大镜/伽马，此时开关仍是「开」，
  // 但 HSL 根本不会生效。以宿主下发的 lutActive（真的在跑逐像素引擎）为准。
  const on = !!state.lut && !!state.lutActive;
  $$('#hslSliders .flat-range, #hslSliders .num, #hslTabs .tab, .chip').forEach(el => {
    el.disabled = !on; el.style.opacity = on ? '' : '.45';
  });
  $('#hslReset').disabled = !on;
  if (on) {
    $('#hslHint').textContent = '已启用 LUT 引擎：8 个色系可分别精确调整、互不干扰（64³ 3D LUT 逐像素着色器，性能损耗较大）。'
      + (state.lutForced ? ' HDR 显示器上由系统强制启用，不可关闭。' : '');
  } else if (state.lut) {
    $('#hslHint').textContent = 'LUT 引擎当前不可用（已回退到放大镜/伽马引擎），分色系 HSL 无法生效；请查看引擎状态或日志。';
  } else {
    $('#hslHint').textContent = '提示：关闭 LUT 引擎后使用放大镜引擎，暂不支持分色系 HSL。';
  }
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
  syncThemeUI();
}
function syncThemeUI() {
  state.theme = (state.theme === 'light' || state.theme === 'dark') ? state.theme : 'auto';
  const sel = $('#themeSelect');
  if (sel) sel.value = state.theme;
  applyTheme();
}
function applyTheme() {
  const eff = state.theme === 'auto' ? (media.matches ? 'dark' : 'light') : state.theme;
  document.body.dataset.theme = eff;
  // 把实际生效的主题告诉宿主：窗口标题栏要跟着 UI 主题走（标题栏与界面一体）
  send({ type: 'theme', dark: eff === 'dark' });
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
    masterEnable: 'master', lutSwitch: 'lut', vsyncSwitch: 'vsync',
    perAppSwitch: 'perapp',
    autoStartSwitch: 'autostart', autoTraySwitch: 'autotray', displayEnable: 'display',
    fpsSwitch: 'showfps',
  };
  const doSwitch = (id, el, v) => {
    if (id === 'displayEnable') { state.displays[state.displayIndex].enabled = v; $('#displayEnableText').textContent = v ? '启用（已开）' : '启用（已关）'; }
    else state[keyOf(id)] = v;
    if (id === 'lutSwitch') { state.lutActive = v; refreshAdvancedSliders(); updateHslEnabled(); }
    if (id === 'perAppSwitch') updatePerAppEnabled();
    if (id === 'masterEnable') updateEngineStatus();
    if (id === 'fpsSwitch') { state.fps = -1; updateFpsBadge(); }
    // 「显示滤镜实时帧率」是界面偏好（宿主会立即落盘），不代表配置被改动，不弹保存条
    if (id !== 'fpsSwitch') markDirty();
    send({ type: 'switch', id: SWITCH_IDS[id] || id, value: v });
  };
  const keyOf = id => ({ masterEnable: 'master', lutSwitch: 'lut', vsyncSwitch: 'vsync',
    perAppSwitch: 'perapp', autoStartSwitch: 'autostart', autoTraySwitch: 'autotray',
    displayEnable: 'displayEnabled', fpsSwitch: 'showFps' }[id] || id);
  // 绕过「再次确认」，直接启用 LUT（用于 LUT 预设确认之后）。
  const enableLutNow = () => {
    if (state.lut) return;
    state.lut = true;
    state.lutActive = true;
    const sw = $('#lutSwitch');
    if (sw) sw.checked = true;
    updateHslEnabled();
    refreshAdvancedSliders();
    send({ type: 'switch', id: 'lut', value: true });
  };
  const wire = (id) => {
    const el = $('#' + id);
    if (!el) { console.error('[demo2] missing element: ' + id); return; }
    el.addEventListener('change', () => {
      const v = el.checked;
      // 从「关」拨到「开」的瞬间先问一次性能损耗；取消时把开关拨回去，不发消息。
      if (id === 'lutSwitch' && v && !state.lut) {
        el.checked = false;
        confirmLutEnable(() => { el.checked = true; doSwitch(id, el, true); }, 'LUT 引擎开关');
        return;
      }
      doSwitch(id, el, v);
    });
  };
  wire('masterEnable'); wire('lutSwitch');
  wire('vsyncSwitch');
  wire('perAppSwitch');
  wire('autoStartSwitch'); wire('autoTraySwitch');
  wire('displayEnable');
  wire('fpsSwitch');

  $('#displaySelect').addEventListener('change', () => {
    state.displayIndex = +$('#displaySelect').value;
    send({ type: 'display', index: state.displayIndex });
  });
  $('#themeSelect').addEventListener('change', e => {
    state.theme = e.target.value;
    applyTheme();
    send({ type: 'theme-save', theme: state.theme });
  });
  media.addEventListener('change', () => { if (state.theme === 'auto') applyTheme(); });

  // 快捷预设（数值与原项目一致）
  const PRESETS = {
    // 基础组（只用基础滑块）
    'default':     { brightness: 0, contrast: 100, saturation: 100, highlights: 0, shadows: 0, temperature: 0, sharpen: 0 },
    eye:           { brightness: -5, contrast: 95, saturation: 95, highlights: 0, shadows: 0, temperature: 25, sharpen: 0 },
    night:         { brightness: -40, contrast: 100, saturation: 90, highlights: 0, shadows: 0, temperature: 45, sharpen: 0 },
    vivid:         { brightness: 0, contrast: 110, saturation: 150, highlights: 0, shadows: 0, temperature: 0, sharpen: 0 },
    'game-bright': { brightness: 4, contrast: 98, saturation: 105, highlights: -5, shadows: 20, temperature: 0, darkScene: 70, sharpen: 0 },
    arena:         { brightness: 0, contrast: 125, saturation: 110, highlights: 5, shadows: -10, temperature: 0, sharpen: 10, clarity: 15 },
    office:        { brightness: 3, contrast: 98, saturation: 92, highlights: 0, shadows: 0, temperature: -18, sharpen: 0 },
    // LUT 组（分色系 HSL / 后处理由宿主下发，这里只给基础部分，Host 同步后补全）
    hdr:           { brightness: 0, contrast: 108, saturation: 115, highlights: 8, shadows: -5, temperature: 0, sharpen: 8, clarity: 18 },
    cinematic:     { brightness: 0, contrast: 108, saturation: 105, highlights: 5, shadows: -6, temperature: 6, sharpen: 0, clarity: 15 },
    landscape:     { brightness: 0, contrast: 106, saturation: 110, highlights: -8, shadows: 12, temperature: 0, sharpen: 8, clarity: 25 },
    portrait:      { brightness: 3, contrast: 102, saturation: 100, highlights: 5, shadows: 8, temperature: 0, sharpen: 0, clarity: 8, noiseReduction: 25 },
    cyber:         { brightness: 0, contrast: 115, saturation: 120, highlights: 6, shadows: -12, temperature: 0, sharpen: 10, clarity: 28 },
  };
  const applyPreset = (name) => {
    const p = PRESETS[name];
    if (!p) return;
    // 宿主 ApplyPresetByName 会以一份完整 FilterSettings 替换当前设置（含 HSL/高级项）。
    // 网页本地也按同一语义重置，否则在宿主 sync 到达前会短暂显示另一套数值，看起来
    // 像「点一个预设，画面先跳到旧 HSL 再跳回来」。
    const zero = Object.assign({}, NEUTRAL_BASE);
    BASE_KEYS.concat(ADVANCED_KEYS).forEach(k => {
      if (typeof state.paramDefaults[k] === 'number') zero[k] = state.paramDefaults[k];
    });
    state.settings.base = Object.assign(zero, p);
    state.settings.hsl = {
      master: { h: 0, s: 100, l: 0 },
      channels: CHANNELS.slice(1).map(c => ({ name: c.name, h: 0, s: 100, l: 0 })),
    };
    buildHslSliders();
    refreshBaseSliders();
    refreshAdvancedSliders();
    markDirty();
    send({ type: 'preset', name: name });
  };
  $$('.preset').forEach(b => b.addEventListener('click', () => {
    const name = b.dataset.preset;
    if (!PRESETS[name]) return;
    // LUT 组预设只在 LUT 引擎下才有完整效果；顺手问一次是否启用（不自动改开关）
    if (LUT_PRESETS.includes(name) && !state.lut) {
      confirmLutEnable(() => { enableLutNow(); applyPreset(name); }, 'LUT 预设「' + b.textContent.trim() + '」');
      return;
    }
    applyPreset(name);
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
    if (i < 0) { $('#hotkeyHint').textContent = '请先在列表中选择一个配置'; return; }
    if (i === 0) { $('#hotkeyHint').textContent = '已经是第一个配置'; return; }
    state.selectedProfile = i - 1;
    send({ type: 'profile', action: 'move', index: i, delta: -1 });
  });
  $('#btnProfileDown').addEventListener('click', () => {
    const i = state.selectedProfile;
    if (i < 0) { $('#hotkeyHint').textContent = '请先在列表中选择一个配置'; return; }
    if (i >= state.profiles.length - 1) { $('#hotkeyHint').textContent = '已经是最后一个配置'; return; }
    state.selectedProfile = i + 1;
    send({ type: 'profile', action: 'move', index: i, delta: 1 });
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
