// The page glue for the PicoDeck web simulator, shared by simulator/web/shell.html
// (local builds) and picodeck.net/try/, which loads it from the release zip.
// Load it before picodeck_simulator.js: it defines Module. Needs a <canvas id="canvas">;
// #status, the #pad keys and the #textsink soft keyboard are optional.
//
// /try/?app=<id, name or folder> starts that app instead of the launcher (--launch).
var statusEl = document.getElementById('status'); // optional
var launchApp = new URLSearchParams(location.search).get('app');
var Module = {
  arguments: launchApp && /^[\w .-]{1,64}$/.test(launchApp) ? ['--launch', launchApp] : [],
  canvas: document.getElementById('canvas'),
  // The dev-command stub polls stdin; Emscripten's default is window.prompt().
  stdin: function () { return null; },
  print: function (t) { console.log(t); },
  printErr: function (t) { console.warn(t); },
  setStatus: function (t) { if (statusEl) statusEl.textContent = t; },
  monitorRunDependencies: function (left) {
    if (!left && statusEl) statusEl.textContent = '';
  },
};

// Browsers only start audio after a user gesture.
function resumeAudio() {
  var ctx = Module.SDL2 && Module.SDL2.audioContext;
  if (ctx && ctx.state === 'suspended') ctx.resume();
}
window.addEventListener('keydown', resumeAudio);
window.addEventListener('pointerdown', resumeAudio);
Module.canvas.focus({ preventScroll: true });

// ── On-screen keys ──────────────────────────────────────────────────────
// SDL reads keys from the canvas (see simulator/main.c), so the pad sends them there.
var KEYCODES = { ArrowLeft: 37, ArrowUp: 38, ArrowRight: 39, ArrowDown: 40,
                 Enter: 13, Escape: 27, Tab: 9, Backspace: 8, F10: 121, ' ': 32 };
var CODES = { ' ': 'Space' };

function codeFor(key) {
  if (CODES[key]) return CODES[key];
  if (key.length > 1) return key;                 // named keys: code == key
  if (/[a-z]/i.test(key)) return 'Key' + key.toUpperCase();
  if (/[0-9]/.test(key)) return 'Digit' + key;
  return '';
}
function keyCodeFor(key) {
  if (KEYCODES[key]) return KEYCODES[key];
  return key.length === 1 ? key.toUpperCase().charCodeAt(0) : 0;
}
function sendKey(type, key) {
  var ev = new KeyboardEvent(type, { key: key, code: codeFor(key),
                                     bubbles: true, cancelable: true });
  var kc = keyCodeFor(key);
  Object.defineProperty(ev, 'keyCode', { get: function () { return kc; } });
  Object.defineProperty(ev, 'which', { get: function () { return kc; } });
  Module.canvas.dispatchEvent(ev);
}
function tapKey(key) { sendKey('keydown', key); sendKey('keyup', key); }

// Hold-to-press: keydown on touch, keyup on release — multitouch safe.
document.querySelectorAll('#pad [data-key]').forEach(function (btn) {
  var key = btn.dataset.key;
  function release(e) {
    if (!btn.classList.contains('down')) return;
    btn.classList.remove('down');
    sendKey('keyup', key);
  }
  btn.addEventListener('pointerdown', function (e) {
    e.preventDefault();
    btn.setPointerCapture(e.pointerId);
    btn.classList.add('down');
    sendKey('keydown', key);
    if (navigator.vibrate) navigator.vibrate(8);
  });
  btn.addEventListener('pointerup', release);
  btn.addEventListener('pointercancel', release);
  btn.addEventListener('lostpointercapture', release);
});

// Soft keyboard for typing: its keystrokes must not also reach SDL directly
// (iOS sends real key events, Android sends 'Unidentified'), so everything
// goes through input/beforeinput and is re-sent once.
var sink = document.getElementById('textsink');
var kbdBtn = document.getElementById('kbd-toggle');
if (sink && kbdBtn) {
  kbdBtn.addEventListener('pointerdown', function (e) { e.preventDefault(); });
  kbdBtn.addEventListener('click', function () {
    if (document.activeElement === sink) sink.blur(); else sink.focus();
  });
  sink.addEventListener('focus', function () { kbdBtn.classList.add('on'); });
  sink.addEventListener('blur', function () { kbdBtn.classList.remove('on'); });
  ['keydown', 'keyup', 'keypress'].forEach(function (t) {
    sink.addEventListener(t, function (e) { e.stopPropagation(); });
  });
  sink.addEventListener('beforeinput', function (e) {
    if (e.inputType === 'deleteContentBackward') { e.preventDefault(); tapKey('Backspace'); }
    else if (e.inputType === 'insertLineBreak') { e.preventDefault(); tapKey('Enter'); }
  });
  // Typed text goes straight to the OS char queue (exact case and punctuation;
  // synthesised key events would come through SDL as unshifted US keys).
  sink.addEventListener('input', function (e) {
    var text = sink.value;
    sink.value = '';
    for (var i = 0; i < text.length; i++) Module._web_type_char(text.charCodeAt(i));
  });
}

// Reveal the pad on the first touch only; mouse and keyboard users never see it.
window.addEventListener('touchstart', function () {
  document.body.classList.add('touch');
}, { once: true, passive: true });
