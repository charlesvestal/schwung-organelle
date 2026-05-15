# CLAUDE.md — schwung-organelle

Schwung sound-generator module that hosts Critter & Guitari Organelle Pure Data patches on Ableton Move via libpd. Component type: `sound_generator`. Installs to `modules/sound_generators/organelle/` on device.

Repo on GitHub: https://github.com/charlesvestal/schwung-organelle

## Build / Deploy

```bash
./scripts/build.sh             # Docker ARM64 cross-compile; produces dist/organelle/
./scripts/install.sh           # scp to move.local + restart shadow_ui
ssh ableton@move.local "pidof shadow_ui | xargs -r kill -9"   # force a fresh UI load
```

The DSP plugin is a single statically-linked `dsp.so` (~1.5 MB) that embeds libpd-multi. JS UI is `ui_chain.js`. Both are deployed alongside `mother.pd`, `help.json`, and the `font_organelle.mjs` Organelle 5×8 font port.

Bundled C&G patches (BSD-3 from `critterandguitari/Organelle_Patches`) ship under `patches/`. The install script seeds them into `/data/UserData/schwung/organelle-patches/` on first install only — preserves user-dropped patches across re-installs.

## Repo layout

```
src/
  dsp/
    pd_host.cpp           Plugin API v2 entry, libpd lifecycle, per-instance state
    mother.pd             Minimal audio bridge (catch~/dac~ + adc~/s~)
    sandbox.cpp/.h        --wrap=open/fopen redirect to /data/UserData/schwung/organelle-sandbox
    screen_translator.{cpp,h}  [s oled ...]/[s screenLineN] parser → ring buffer → JSON
    patch_loader.{cpp,h}  libpd_openfile + filesystem listing
    plugin_api_v2.h       Vendored from schwung/src/host/plugin_api_v1.h
  module/
    module.json           Module metadata + chain_params
    ui_chain.js           Slot UI: patch browser, home-screen renderer, input routing
    font_organelle.mjs    Ported Organelle 5×8 font (verbatim from fonts.h)
    help.json             In-device help text
patches/                  Bundled C&G patches (each retains LICENSE.txt)
libs/libpd/               Submodule, multi-instance build
scripts/
  build.sh                Docker entrypoint, ld --wrap flags
  install.sh              scp + sandbox seed (writes /root/version=5)
  Dockerfile              ARM64 cross toolchain
docs/plans/               Design docs (port-design, osc-bridge, etc.)
```

## Architecture

```
Move shadow slot
       │
       ▼
ui_chain.js (slot UI)
       │  knob CC → integrate delta → host_module_set_param
       │  jog turn → host_module_set_param("encoderInput", delta)
       │  knob 7/8 touch → host_module_set_param("aux"/"fs")
       │
       ▼
pd_host.cpp set_param
       │  publishes to Pd receivers: knob1..4 (0..1), knob1..4Raw (0..1023),
       │  aux, fs, enc, encbut, plus legacy encoderInput/Button
       │
       ▼
Pd instance (libpd)
       │  mother.pd: catch~ outL/outR → dac~, adc~ → s~ inL/inR
       │  user patch: reads receivers, throws to outL/outR, sends [s screenLine*]
       │
       ▼
pd_host.cpp render_block
       │  libpd_process_float interleaved stereo, hard clip × gain (~-10dB default)
       │
       ▼
Move slot mixer
```

Screen pipeline: Pd patches send `[s oled gXxx ...]` or `[s screenLineN text]`. Per-instance libpd messagehook routes them to a lock-free SPSC ring (512 ops). JS `tick()` calls `host_module_get_param("screen_ops")` which drains the ring as JSON. JS executes draw ops against Move's display API.

## Status

**v0.1 working:** audio, MIDI in (`[r notes]` list + `[notein]`), knobs, Aux (knob 7 touch), foot switch (knob 8 touch), jog encoder, custom-screen patches, home-screen patches with the ported Organelle 5×8 font, FS sandbox, libpd hooks per-instance. Five bundled C&G patches confirmed: Drone-1, Dropper, FunFX, Genny-1, Partial-Party.

**v0.2 in design** (`docs/plans/2026-05-15-osc-bridge.md`): full OSC compatibility bridge so patches using the `patchmenu` library (CZZ-Multi, Basic Poly, Orac-style) navigate properly. Plan: ship C&G's full upstream `mother.pd` verbatim and route Move events through UDP-loopback OSC inside the DSP plugin instead of direct `s enc`/`s aux`/etc. sends.

## Hard-won lessons (don't relearn)

### libpd / Pd

- **Hooks are per-instance.** Install `libpd_set_messagehook` etc. AFTER `libpd_set_instance(inst->pd)`, not once globally. Global call only sets `libpd_mainimp`'s hooks; per-slot instances get empty hook tables.
- **No mutex on the audio thread.** Use `libpd_set_instancedata`/`libpd_get_instancedata` to find your `Instance*` from inside hooks. The earlier mutex-guarded `unordered_map` lookup starved SPI when Genny-1 emitted error prints at ~300/sec.
- **No-op print hook.** Pd's default printhook writes to `stderr` which is unusable in the schwung shim context (glibc `0xfbad8001` → segfault). Install a no-op `libpd_set_printhook` per-instance.
- **No `gensym()` from RT contexts.** It allocates. Build text inline; reserve `t_atom` arrays on stack.
- **No `host_log` from hooks.** File I/O on the audio thread = freeze.
- **`libpd_list("notes", 2, atoms)` for [r notes].** Bare floats don't trigger downstream `[unpack 0 0]` correctly.
- **`[r notes]` is Organelle convention.** Patches also use `[notein]` (legacy). Send both (`libpd_noteon` + the list).
- **`[s enc]` is 0/1 per detent**, not signed magnitude. Patches use `[sel 0 1]`.

### Build / linking

- **`ld --wrap=` works for libc, not libpd.** `--wrap=sys_open` failed because `sys_open` is defined inside `libpd-multi.a` alongside its callers; `--wrap` only catches cross-archive references. libc is separate, so `--wrap=open`, `--wrap=open64`, `--wrap=fopen`, `--wrap=fopen64` works for our FS sandbox.
- **libpd archive is `libpd-multi.a`**, not `libpd.a`, when built with `-DPD_MULTI=ON`.
- **No reverting commits without explicit user approval.** The user has been burned by destructive git. Use forward-commits to undo.

### Pd file format

- **`\$1`/`\$2`** in `.pd` files = the input list element substitution. Bare `$1` parses as canvas-arg literal (0 at top level). Took a while to find this when test patches showed all zeros.
- **Comments inside `/* */` in C headers** confused my font-data extraction (the commented-out `0xff` block in `fonts.h`). Strip comments before parsing.

### Schwung host integration

- **Back button is intercepted** by shadow_ui in COMPONENT_EDIT view (`src/shadow/shadow_ui.js:15217`). Our `onMidiMessageInternal` never sees Back. Use long-press knob 7 (Aux) for exit-to-list.
- **Move knob CCs are RELATIVE deltas.** Values 1-63 = CW, 64-127 = CCW. Integrate in JS into 0-1023 (Organelle's native knob range).
- **Knob touch notes are 0-7** (knob N → note N-1). They get dropped by `shouldFilterMessage` from `input_filter.mjs`, so handle them BEFORE the filter.
- **`host_module_get_param` buffer is 65536 bytes** — plenty for screen ops and patch lists.
- **`host_read_file` is path-validated** to `/data/UserData/*` — fine for our use.

### Aux/foot switch mapping

- **Aux on Organelle = key 0** of the keybed (the leftmost key sends OSC `/key 0 100/0`), not a separate button. We map it to knob 7 capacitive touch.
- **Foot switch = `[r fs]`.** We map to knob 8 capacitive touch.
- **Long-press knob 7 (2s)** exits to patch list. The user originally thought Aux = exit to menu, but on Organelle, Aux is a normal patch input. The menu access is a Vol+Aux gesture.

## Testing flow

1. Edit code
2. `./scripts/build.sh`
3. `./scripts/install.sh`
4. `ssh ableton@move.local "pidof shadow_ui | xargs -r kill -9"`
5. On device: Shift+Vol+Track1, load Organelle into slot, long-press track to open slot UI
6. Browser jog-scroll selects patch; jog-click loads
7. Pads play notes; knob 7 touch = Aux to patch; knob 8 touch = foot switch
8. Hold knob 7 2s to exit back to patch list
9. Check `/data/UserData/schwung/debug.log` for issues (touch `debug_log_on` to enable)

## Releasing

Standard Schwung external module flow (see parent `CLAUDE.md`):

1. Bump version in `src/module/module.json`
2. `git tag v0.X.0 && git push --tags`
3. GitHub Actions builds + uploads tarball + updates `release.json`
4. `gh release edit v0.X.0 -R charlesvestal/schwung-organelle --notes "..."`

Catalog entry lives in `../schwung/module-catalog.json` (already added).

## License posture

- Our code (DSP wrappers, `mother.pd`, `ui_chain.js`): MIT (`LICENSE`)
- **libpd**: BSD-3 / Pure Data License
- **5×8 font** (`src/module/font_organelle.mjs`): BSD-3, verbatim from `Organelle_OS/fonts.h`. Full license text in `LICENSE`.
- **Bundled patches** (`patches/*`): BSD-3 from C&G's `Organelle_Patches` repo. Each patch folder retains its own `LICENSE.txt`.
