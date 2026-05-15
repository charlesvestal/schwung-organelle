/*
 * Organelle slot UI (ui_chain.js).
 *
 * Two states:
 *   - 'browser': scan /data/UserData/schwung/organelle-patches/ and let
 *                the user pick one with the jog wheel.
 *   - 'running': forward the Pd patch's [s oled] draw commands to the
 *                Move display, and route knobs/jog/touches into Pd.
 */

import { shouldFilterMessage, decodeDelta } from '/data/UserData/schwung/shared/input_filter.mjs';
import { printSmall } from '/data/UserData/schwung/modules/sound_generators/organelle/font_organelle.mjs';

const SCREEN_W = 128;
const SCREEN_H = 64;

/* ----- state ----- */
let state = 'browser';                // 'browser' | 'running'
let patches = [];                     // [{name, path}]
let selectedIndex = 0;
let scrollOffset = 0;
let needsBrowserRedraw = true;
let midiOutDrainCounter = 0;

// Accumulated 0-1023 value per physical knob (1-indexed; we use 1..4).
// Move's knobs send relative deltas (1-63=CW, 64-127=CCW), not absolute,
// so we have to integrate. 0-1023 matches Organelle's native [r knobN] range.
const knobValues = [0, 512, 512, 512, 512];   // indexes 1..4 used; 0 unused
const KNOB_STEP = 8;                          // 128 ticks → full range

// Default screen state — Organelle OS-style. Top 8px info bar shows the
// patch name; below that, five 11px-tall text rows mirror what the
// patch publishes via [s screenLine1..5].
//
// Pixel layout matches OledScreen::calcxpos(n) = (n-1)*11 + 9 from
// Organelle_OS/OledScreen.cpp — line N starts at y = 9 + (n-1)*11.
let currentPatchName = '';
let screenLines = ['', '', '', '', ''];
let patchHasDrawn = false;          // set true on first [s oled ...] op
let defaultDirty = true;

/* ----- helpers ----- */

function fetchPatchList() {
    const json = host_module_get_param('patch_list');
    try {
        const list = JSON.parse(json || '[]');
        if (Array.isArray(list)) return list;
    } catch (_e) {}
    return [];
}

function fetchInitialPatchPath() {
    const p = host_module_get_param('patch_path');
    return p || '';
}

function drawBrowser() {
    clear_screen();

    // Header
    fill_rect(0, 0, SCREEN_W, 11, 1);
    print(2, 2, 'Organelle', 0);

    if (!patches.length) {
        print(8, 28, 'No patches found.', 1);
        print(2, 44, '/organelle-patches/', 1);
        host_flush_display();
        needsBrowserRedraw = false;
        return;
    }

    const listTop = 13;
    const listBottom = SCREEN_H - 1;
    const lineH = 10;
    const visibleRows = Math.floor((listBottom - listTop) / lineH);

    if (selectedIndex < scrollOffset) scrollOffset = selectedIndex;
    if (selectedIndex >= scrollOffset + visibleRows) scrollOffset = selectedIndex - visibleRows + 1;

    for (let i = 0; i < visibleRows; i++) {
        const idx = scrollOffset + i;
        if (idx >= patches.length) break;
        const y = listTop + i * lineH;
        const sel = idx === selectedIndex;
        if (sel) fill_rect(0, y - 1, SCREEN_W, lineH, 1);
        print(2, y, patches[idx].name, sel ? 0 : 1);
    }

    host_flush_display();
    needsBrowserRedraw = false;
}

function patchNameFromPath(p) {
    const i = p ? p.lastIndexOf('/') : -1;
    return i >= 0 ? p.substring(i + 1) : (p || '');
}

function pushKnobValuesToPatch() {
    for (let i = 1; i <= 4; i++) {
        host_module_set_param(`knob${i}`, String(knobValues[i]));
    }
}

function enterRunning(patch) {
    host_module_set_param('patch_path', patch.path);
    pushKnobValuesToPatch();        // patches read r knob1..4 on loadbang
    state = 'running';
    currentPatchName = patch.name;
    screenLines = ['', '', '', '', ''];
    patchHasDrawn = false;
    defaultDirty = true;
    clear_screen();
    host_flush_display();
}

function exitToBrowser() {
    host_module_set_param('patch_path', '');
    state = 'browser';
    currentPatchName = '';
    screenLines = ['', '', '', '', ''];
    patchHasDrawn = false;
    patches = fetchPatchList();
    if (selectedIndex >= patches.length) selectedIndex = Math.max(0, patches.length - 1);
    needsBrowserRedraw = true;
    drawBrowser();
}

/* ----- default screen (Organelle home-screen port) ----- */
// Mirrors Organelle_OS/OledScreen.cpp::setLine: a top info bar (y=0-8) with
// the patch name + 5 text rows below, each at y = 9 + (n-1)*11. Patches
// set the rows via [s screenLineN <text>].

function drawDefaultScreen() {
    clear_screen();
    // Info bar (y=0-8): patch name, rendered with the ported Organelle
    // 5x8 small font for pixel-accurate parity with the real device.
    if (currentPatchName) printSmall(currentPatchName, 2, 0, 1);
    // 5 text lines: y = calcxpos(n) + 1 = 9 + (n-1)*11 + 1.
    for (let n = 1; n <= 5; n++) {
        const txt = screenLines[n - 1];
        if (!txt) continue;
        const y = 9 + (n - 1) * 11 + 1;
        printSmall(txt, 2, y, 1);
    }
    host_flush_display();
    defaultDirty = false;
}

function updateDefaultIfNeeded() {
    if (state !== 'running' || patchHasDrawn) return;
    if (defaultDirty) drawDefaultScreen();
}

/* ----- screen op execution (running state) ----- */

function executeScreenOps(ops) {
    let didFlip = false;
    let drawnFromPatch = false;
    for (const o of ops) {
        switch (o.op) {
            case 'set_line': {
                // Default screen text — patch published [s screenLineN text].
                const n = (o.n | 0);
                if (n >= 1 && n <= 5) {
                    screenLines[n - 1] = String(o.text || '');
                    defaultDirty = true;
                }
                break;
            }
            case 'clear':
                clear_screen();
                drawnFromPatch = true;
                break;
            case 'fill':
                fill_rect(o.x | 0, o.y | 0, o.w | 0, o.h | 0, o.c ? 1 : 0);
                drawnFromPatch = true;
                break;
            case 'line':
                draw_line(o.x1 | 0, o.y1 | 0, o.x2 | 0, o.y2 | 0, o.c ? 1 : 0);
                drawnFromPatch = true;
                break;
            case 'box': {
                const x = o.x | 0, y = o.y | 0, w = o.w | 0, h = o.h | 0, c = o.c ? 1 : 0;
                draw_line(x, y, x + w - 1, y, c);
                draw_line(x, y + h - 1, x + w - 1, y + h - 1, c);
                draw_line(x, y, x, y + h - 1, c);
                draw_line(x + w - 1, y, x + w - 1, y + h - 1, c);
                drawnFromPatch = true;
                break;
            }
            case 'invert':
                // Approximation: fill white. Schwung's display API exposes no
                // XOR invert. C&G patches use invert mainly for menu hilight,
                // which still reads OK if text is drawn in color 0 on top.
                fill_rect(o.x | 0, o.y | 0, o.w | 0, o.h | 0, 1);
                drawnFromPatch = true;
                break;
            case 'pixel':
                set_pixel(o.x | 0, o.y | 0, o.c ? 1 : 0);
                drawnFromPatch = true;
                break;
            case 'print':
                print(o.x | 0, o.y | 0, String(o.text || ''), o.c ? 1 : 0);
                drawnFromPatch = true;
                break;
            case 'flip':
                didFlip = true;
                break;
        }
    }
    if (drawnFromPatch) {
        patchHasDrawn = true;
        if (didFlip) host_flush_display();
    }
    return drawnFromPatch;
}

/* ----- MIDI-out drain (running state) ----- */
// Pd's [noteout]/[ctlout] hooks fill a ring in the DSP plugin; drain it as
// hex triplets via get_param("midi_out_queue") and forward via
// host_module_send_midi (or external as appropriate).

function drainMidiOut() {
    const hex = host_module_get_param('midi_out_queue');
    if (!hex || !hex.length) return;
    for (const part of hex.split(',')) {
        if (part.length < 6) continue;
        const msg = new Uint8Array(3);
        msg[0] = parseInt(part.substr(0, 2), 16);
        msg[1] = parseInt(part.substr(2, 2), 16);
        msg[2] = parseInt(part.substr(4, 2), 16);
        // Source 0 = internal (Move's MIDI out). Modules emitting to external
        // USB MIDI would use source 2 — leave that for later.
        host_module_send_midi(msg, 0);
    }
}

/* ----- lifecycle ----- */

function init() {
    patches = fetchPatchList();
    const cur = fetchInitialPatchPath();
    if (cur) {
        state = 'running';
        currentPatchName = patchNameFromPath(cur);
        // Recover the home-screen text from the DSP-side cache (the screen-
        // ops ring is one-shot per draw, so without this the screen would
        // stay blank until the patch re-emits its lines — e.g. on a knob
        // turn or pad press).
        const stateJson = host_module_get_param('screen_state');
        if (stateJson) {
            try {
                const lines = JSON.parse(stateJson);
                if (Array.isArray(lines)) {
                    for (let i = 0; i < 5; i++) screenLines[i] = lines[i] || '';
                }
            } catch (_e) { screenLines = ['', '', '', '', '']; }
        } else {
            screenLines = ['', '', '', '', ''];
        }
        patchHasDrawn = false;
        defaultDirty = true;
        clear_screen();
        host_flush_display();
    } else {
        state = 'browser';
        needsBrowserRedraw = true;
        drawBrowser();
    }
}

function tick() {
    if (state === 'browser') {
        if (needsBrowserRedraw) drawBrowser();
        return;
    }
    // Running state.
    const json = host_module_get_param('screen_ops');
    if (json && json.length > 2) {  // ignore empty "[]"
        let ops;
        try { ops = JSON.parse(json); } catch (_e) { ops = null; }
        if (Array.isArray(ops) && ops.length) executeScreenOps(ops);
    }
    updateDefaultIfNeeded();
    // Drain MIDI out every 2 ticks (~22 Hz, plenty for note timing).
    if ((++midiOutDrainCounter & 1) === 0) drainMidiOut();
}

/* ----- input ----- */

function onMidiMessageInternal(data) {
    if (!data || data.length < 2) return;
    const status = data[0];
    const d1 = data[1];
    const d2 = data.length > 2 ? data[2] : 0;
    const hi = status & 0xF0;

    // --- Knob capacitive touches: notes 0-7 (knob N → note N-1).
    // shouldFilterMessage() drops these, so handle BEFORE the filter.
    if ((hi === 0x90 || hi === 0x80) && d1 >= 0 && d1 <= 7) {
        const on = hi === 0x90 && d2 > 0;
        // Knob 7 touch (note 6) = Organelle Aux = open patch list.
        // Patches don't see auxKey by default; matches Organelle's native UX
        // where Aux is the system key, not a patch button.
        if (d1 === 6 && on) {
            if (state === 'running') exitToBrowser();
            return;
        }
        // Knob 8 touch (note 7) = foot switch → r fs to the patch.
        if (d1 === 7) { host_module_set_param('fs', on ? '1' : '0'); return; }
        return;  // other knob touches ignored
    }

    // Drop noise + capacitive touches we don't care about.
    if (shouldFilterMessage(data)) return;

    // --- CC traffic
    if (hi !== 0xB0) {
        // Pad/keyboard notes (>= 10) are NOT re-dispatched here — they
        // arrive in the DSP plugin's on_midi via the chain mixer route.
        return;
    }

    // Knobs 1-4 (CC 71-74). Move sends RELATIVE deltas, not absolute — integrate.
    if (d1 >= 71 && d1 <= 74) {
        const knob = d1 - 71 + 1;
        const delta = decodeDelta(d2);
        if (!delta) return;
        let v = knobValues[knob] + delta * KNOB_STEP;
        if (v < 0) v = 0;
        if (v > 1023) v = 1023;
        knobValues[knob] = v;
        // Send raw 0-1023 to Pd; DSP plugin no longer rescales.
        host_module_set_param(`knob${knob}`, String(v));
        return;
    }

    // Jog turn (CC 14, signed delta encoded as 0..127)
    if (d1 === 14) {
        const delta = d2 < 64 ? d2 : d2 - 128;
        if (!delta) return;
        if (state === 'browser') {
            if (delta > 0 && selectedIndex < patches.length - 1) selectedIndex++;
            if (delta < 0 && selectedIndex > 0) selectedIndex--;
            needsBrowserRedraw = true;
        } else {
            host_module_set_param('encoderInput', String(delta));
        }
        return;
    }

    // Jog click (CC 3)
    if (d1 === 3) {
        if (state === 'browser') {
            if (d2 > 0 && patches.length) enterRunning(patches[selectedIndex]);
        } else {
            host_module_set_param('encoderButton', d2 > 0 ? '1' : '0');
        }
        return;
    }

    // Back button (CC 51): in running state, return to browser.
    if (d1 === 51 && d2 > 0 && state === 'running') {
        exitToBrowser();
        return;
    }
}

function onMidiMessageExternal(_data) {
    // External USB MIDI arrives in the chain mixer and is delivered to the
    // DSP plugin's on_midi automatically per the slot's "full bridge"
    // routing. No JS-side action needed.
}

globalThis.chain_ui = {
    init,
    tick,
    onMidiMessageInternal,
    onMidiMessageExternal,
};
