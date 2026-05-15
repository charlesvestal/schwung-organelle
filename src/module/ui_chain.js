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

// Default screen state — drawn for patches that don't emit [s oled]
// themselves (most C&G stock patches relied on the Organelle OS for this).
let currentPatchName = '';
let knobLabels = ['', '', '', ''];
let patchHasDrawn = false;
let lastDrawnKnobs = [-1, -1, -1, -1];

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

function loadKnobLabels(patchPath) {
    knobLabels = ['', '', '', ''];
    if (!patchPath) return;
    const meta = host_read_file(`${patchPath}/metadata.json`);
    if (!meta) return;
    try {
        const json = JSON.parse(meta);
        if (Array.isArray(json.knob_labels)) {
            for (let i = 0; i < 4; i++) knobLabels[i] = json.knob_labels[i] || '';
        }
    } catch (_e) {}
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
    loadKnobLabels(patch.path);
    patchHasDrawn = false;
    lastDrawnKnobs = [-1, -1, -1, -1];
    clear_screen();
    host_flush_display();
}

function exitToBrowser() {
    host_module_set_param('patch_path', '');
    state = 'browser';
    currentPatchName = '';
    patchHasDrawn = false;
    patches = fetchPatchList();
    if (selectedIndex >= patches.length) selectedIndex = Math.max(0, patches.length - 1);
    needsBrowserRedraw = true;
    drawBrowser();
}

/* ----- default screen (Organelle "Mother" style) ----- */
// Drawn when state=running AND the patch hasn't emitted any [s oled] yet.
// Shows patch name + 4 knob value bars, mirroring what the Organelle OS
// renders by default for patches without custom UI.

function drawDefaultScreen() {
    clear_screen();
    const SCREEN_W = 128;
    print(2, 0, currentPatchName || 'Patch', 1);

    const startY = 18;
    const lineH  = 11;
    const labelW = 50;
    const valueW = 22;
    const barX   = labelW + 2;
    const barW   = SCREEN_W - barX - valueW;

    for (let i = 0; i < 4; i++) {
        const v = knobValues[i + 1] / 1023;
        const y = startY + i * lineH;
        const lab = (knobLabels[i] || `K${i + 1}`).substring(0, 8);
        print(2, y, `${lab}:`, 1);

        // Bar outline (1px border, 5px tall fill)
        const bx = barX, by = y, bw = barW, bh = 6;
        draw_line(bx,        by,        bx + bw - 1, by,        1);
        draw_line(bx,        by + bh - 1, bx + bw - 1, by + bh - 1, 1);
        draw_line(bx,        by,        bx,        by + bh - 1, 1);
        draw_line(bx + bw - 1, by,        bx + bw - 1, by + bh - 1, 1);
        const fillW = Math.max(0, Math.min(bw - 2, Math.round(v * (bw - 2))));
        if (fillW > 0) fill_rect(bx + 1, by + 1, fillW, bh - 2, 1);

        // Percent on the right
        const pct = Math.round(v * 100);
        print(bx + bw + 2, y, `${pct}`, 1);
    }
    host_flush_display();
}

function updateDefaultIfNeeded() {
    if (state !== 'running' || patchHasDrawn) return;
    let changed = lastDrawnKnobs[0] < 0;   // first call after entering running
    for (let i = 0; i < 4; i++) {
        if (knobValues[i + 1] !== lastDrawnKnobs[i]) {
            changed = true;
            lastDrawnKnobs[i] = knobValues[i + 1];
        }
    }
    if (changed) drawDefaultScreen();
}

/* ----- screen op execution (running state) ----- */

function executeScreenOps(ops) {
    let didFlip = false;
    for (const o of ops) {
        switch (o.op) {
            case 'clear':
                clear_screen();
                break;
            case 'fill':
                fill_rect(o.x | 0, o.y | 0, o.w | 0, o.h | 0, o.c ? 1 : 0);
                break;
            case 'line':
                draw_line(o.x1 | 0, o.y1 | 0, o.x2 | 0, o.y2 | 0, o.c ? 1 : 0);
                break;
            case 'box': {
                // Outlined rect: 4 single-pixel lines.
                const x = o.x | 0, y = o.y | 0, w = o.w | 0, h = o.h | 0, c = o.c ? 1 : 0;
                draw_line(x, y, x + w - 1, y, c);
                draw_line(x, y + h - 1, x + w - 1, y + h - 1, c);
                draw_line(x, y, x, y + h - 1, c);
                draw_line(x + w - 1, y, x + w - 1, y + h - 1, c);
                break;
            }
            case 'invert':
                // Approximation: fill white. Real XOR invert isn't exposed by
                // Schwung's display API; most Organelle patches use invert
                // for selection highlighting, which this still reads correctly
                // if the patch draws text in color 0 over the highlight.
                fill_rect(o.x | 0, o.y | 0, o.w | 0, o.h | 0, 1);
                break;
            case 'pixel':
                set_pixel(o.x | 0, o.y | 0, o.c ? 1 : 0);
                break;
            case 'print':
                print(o.x | 0, o.y | 0, String(o.text || ''), o.c ? 1 : 0);
                break;
            case 'flip':
                didFlip = true;
                break;
        }
    }
    if (didFlip) host_flush_display();
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
        loadKnobLabels(cur);
        patchHasDrawn = false;
        lastDrawnKnobs = [-1, -1, -1, -1];
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
    let drewFromPatch = false;
    if (json && json.length > 2) {  // ignore empty "[]"
        let ops;
        try { ops = JSON.parse(json); } catch (_e) { ops = null; }
        if (Array.isArray(ops) && ops.length) {
            executeScreenOps(ops);
            patchHasDrawn = true;
            drewFromPatch = true;
        }
    }
    if (!drewFromPatch) updateDefaultIfNeeded();
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
