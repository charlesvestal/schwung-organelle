// Organelle slot UI — patch browser + Pd screen passthrough.
// Skeleton; implementation tracked in docs/plans/2026-05-12-organelle-port-plan.md.

import {
    drawMenuHeader,
    drawMenuList,
    menuLayoutDefaults,
} from '../../shared/menu_layout.mjs';

let currentPatchPath = '';
let patchList = [];
let selectedIndex = 0;

function init() {
    clear();
    print(20, 28, 'Organelle', 16);
    flip();
}

function tick() {
    // TODO (Task 8+): refresh patch list; render State A or State B.
}

function onMidiMessageInternal(_data) {
    // TODO (Task 10+): jog, knob touches, knob CCs.
}

function onMidiMessageExternal(_data) {
    // TODO (Task 10+): full MIDI bridge in State B.
}

globalThis.chain_ui = {
    init,
    tick,
    onMidiMessageInternal,
    onMidiMessageExternal,
};
