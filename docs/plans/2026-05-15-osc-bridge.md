# OSC Compatibility Bridge — v0.2 Design

**Date**: 2026-05-15
**Status**: Design — not yet implemented
**Goal**: Make every well-behaved Organelle patch run unmodified, including ones using the `patchmenu` library (CZZ-Multi, Basic Poly, Orac-style, etc.).

## Problem

v0.1 wires Move controls directly to Pd receivers:
- Knob CCs → `[s knobN]`, `[s knobNRaw]` (Organelle convention)
- Knob 7/8 touch → `[s aux]` / `[s fs]`
- Jog turn/click → `[s enc]` / `[s encbut]`
- Pads → `[s notes]` list + `libpd_noteon`
- Pd `[s oled gPrintln ...]` → parsed in DSP, drawn by JS
- Pd `[s screenLineN ...]` → parsed in DSP, drawn by JS via the ported Organelle home-screen

This works for any patch that just reads inputs and produces audio (Drone-1, Dropper, FunFX, Genny-1, Partial-Party — all confirmed working).

It breaks for patches using the `patchmenu` library, because patchmenu depends on the **OS-side OSC handshake**:

1. Patch loadbang fires `[msg 0 → patchmenu inlet]` (the patchmenu's enable is set to 0)
2. Patch fires `[bang → s enableSubMenu]`
3. On real Organelle, mother.pd's `[r enableSubMenu]` → routes through `[s oscOut]` → OSC `/enablepatchsub 1` → OS sets `app.patchScreenEncoderOverride = 1`
4. From that point on, the OS routes `/encoder/turn` OSC to the patch
5. The OS *also* sends `/patchLoaded 1` and other state-trigger OSCs that wake up the patchmenu's internal state machine (likely via `[r page-ready]` feedback)
6. Patchmenu's internal spigots open and encoder events drive page navigation

We replicated step 4 unconditionally (good — patches that only need encoder still work) but skipped 3, 5, 6. patchmenu therefore stays stuck with its spigots closed.

## Architecture

Set up a proper OSC bridge between our DSP plugin and the Pd instance so patches think they're talking to the real Organelle OS:

```
Move hardware           Our DSP plugin            Pd patch (in libpd)
─────────────           ──────────────            ───────────────────
Jog turn       ──►      OSC sender                [netreceive 4001]
                        emits /encoder/turn  ──►  ┌─────────────┐
Pad press      ──►      emits /key N V       ──►  │ mother.pd   │
Knob touch 7   ──►      emits /key 0 V       ──►  │ r oscIn     │
                                                  │ routeOSC    │
                                                  │ ↓           │
                                                  │ s knob1..4  │
                                                  │ s notes     │
                                                  │ s enc       │
                                                  │ s aux       │
                                                  │ ...         │
                                                  └─────────────┘
                                                          │
                                                          │ s oscOut
                                                          ▼
DSP plugin     ◄──      [netsend localhost:4000]   patch sends:
parses OSC:                                       /oled/line/N "text"
 /oled/line/N   ──► OP_SET_LINE op                /oled/gPrintln ...
 /oled/gXxx    ──► OP_xxx op                      /enablepatchsub 1
 /enablepatchsub 1 ──► record state               /patchLoaded 1
 /patchLoaded ──► (echo back as OS would)         /gohome ...
 /gohome      ──► trigger exit_to_browser
```

The key invariant: **mother.pd we ship is the real Organelle mother.pd, verbatim** (BSD-3, fine to redistribute). All our routing happens at the OSC layer. Patches see exactly what they'd see on real hardware.

## Implementation Plan

### 1. UDP loopback inside libpd
- Use `[netreceive -u 4001]` in mother.pd (already there in upstream mother.pd's messageIO subpatch)
- Use `[netsend -u 4000]` in mother.pd for `r oscOut → patch's OSC output`
- libpd's `[netreceive]` works on real sockets even in the embedded host

### 2. DSP plugin OSC client/server
- C++ code that:
  - Listens on UDP 4000 (receive `s oscOut` from the patch)
  - Sends to UDP 4001 (deliver hardware events to `r oscIn`)
- Parse OSC manually (no liblo dependency — tiny code, the format is simple)
- Loopback only — no actual network exposure

### 3. Ship real mother.pd
- Replace our minimal mother.pd with C&G's full one from `Organelle_OS/fw_dir/mother.pd`
- BSD-3-Clause attribution already in our LICENSE
- Comes with audioIO, messageIO, MIDI, menuControl, init subpatches — everything

### 4. Translate Move events → Organelle OSC
- Jog turn → `/encoder/turn 0` or `/encoder/turn 1` (one OSC per detent, just direction)
- Jog click → `/encoder/button 1` then `/encoder/button 0`
- Knob 7 touch → `/key 0 100` / `/key 0 0` (Organelle's Aux key)
- Knob 8 touch → `/fs 1` / `/fs 0`
- Knobs 1-4 CC → `/knobs <k1> <k2> <k3> <k4> <vol> <exp>` (Organelle batches all knobs in one OSC)
- Pads → `/key <N+1> <vel>` (key 0 reserved for Aux)

### 5. Parse incoming OSC from patch
- `/oled/line/N "text"` → OP_SET_LINE (already handled)
- `/oled/gPrintln`, `/oled/gFillArea` etc. → existing screen ops
- `/enablepatchsub <0/1>` → record state (informational; we always forward)
- `/patchLoaded <1>` → echo back so patches that wait for it can proceed
- `/gohome <1>` → exit to patch list (replaces our long-press knob 7 hack)
- `/led <0-7>` → optional, RGB color hint (Move doesn't have an RGB LED matching Organelle's spec; skip or repurpose)

### 6. Drop direct `s enc` / `s aux` etc. from DSP set_param
- All hardware events go via OSC now
- Keeps a single source of truth for routing
- The minimal direct-receiver path stays for `set_param` calls from the chain mixer (knob1..4 etc. from slot settings)

### 7. Remove main.pd hacks
- No per-patch modifications needed
- Patches load verbatim from their upstream

## What stays the same

- DSP plugin sandbox (`__wrap_open`/`fopen` redirecting `/root`, `/sdcard`, `/usbdrive` to `/data/UserData/schwung/organelle-sandbox`)
- Per-instance Pd setup, libpd hooks, instance data
- Screen op ring + JS rendering pipeline (set_line, fill, line, print, etc.)
- 5×8 Organelle font port

## What gets simpler

- ui_chain.js no longer needs to know about `r aux`, `r fs`, `r encoderInput` etc. specifically — it just translates Move events to OSC and lets mother.pd route.
- DSP plugin's `set_param` dispatch shrinks: most cases become "compose OSC, send".
- Long-press knob 7 → exit-to-list can use the OS's `/gohome 1` flow instead of a custom JS gesture.

## Risk and unknowns

- libpd's `[netreceive]` / `[netsend]` might have realtime caveats — verify they don't block on packet send/recv in process()
- OSC parsing overhead per knob CC could add up; might need batching
- Move's MIDI rate vs Organelle's polling rate — the OS sends `/knobs` ~100Hz with all 6 knob values; we'd need to throttle / batch our individual CC events

## File layout after v0.2

```
src/dsp/
  pd_host.cpp         — Plugin v2 entry, lifecycle
  osc_bridge.cpp      — NEW: UDP loopback client/server, parser
  osc_bridge.h
  sandbox.cpp         — unchanged
  screen_translator.cpp — unchanged (now fed by osc_bridge)
  mother.pd           — REPLACED with verbatim upstream Organelle mother.pd
```

## Testing milestones

1. **OSC plumbing alive**: send a `/oled/line/1 "hello"` from DSP test code, see it on Move's screen via the existing screen pipeline.
2. **Knobs round-trip**: turn Move knob → OSC `/knobs` → mother.pd → `s knob1` → Drone-1 responds. Confirm parity with v0.1.
3. **Encoder unlocks CZZ**: load CZZ-Multi, send `s enableSubMenu` bang, then turn jog. Pages should navigate across all 5 entries.
4. **Patches verbatim**: confirm no per-patch `.pd` modifications anywhere in `patches/`.

## Out of scope for v0.2 (deferred)

- MIDI in/out OSC (we keep direct libpd_noteon for now — most patches use [r notes] which we cover)
- LED RGB matching Move's PAD LEDs to mother.pd's `/led` semantics
- ORAC (its own multi-process design)
- Externals (`freeverb~`, `else/`, etc.) — separate v0.3 work
