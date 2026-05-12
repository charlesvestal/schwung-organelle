# Organelle Port — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use `superpowers:executing-plans` to implement this plan task-by-task.

**Goal:** Ship `schwung-organelle` v0.1.0 — a Schwung shadow-slot sound generator that runs Critter & Guitari Organelle Pure Data patches via embedded libpd. Vanilla Pd only (no externals).

**Architecture:** New external module repo (already scaffolded at `e235876`). DSP plugin embeds libpd (multi-instance) and exposes Plugin API v2. JS `ui_chain.js` draws the Organelle patch's screen by pulling parsed `[s oled]` ops from a lock-free ring buffer once per tick. Knob 7/8 capacitive touches drive `auxKey`/`fs`; jog wheel drives `encoderInput`/`encoderButton`. Audio in for `[adc~]`. Full MIDI bridge in both directions.

**Tech Stack:**
- libpd (multi-instance, BSD-3) — vendored submodule at `libs/libpd/`
- C++17 plugin built via Docker → ARM64 (aarch64-linux-gnu-g++)
- Schwung Plugin API v2 (`src/dsp/plugin_api_v2.h`)
- Schwung shared JS utilities (`menu_layout.mjs`, `input_filter.mjs`, `screen_reader.mjs`)

**Design doc:** `schwung/docs/plans/2026-05-12-organelle-port-design.md`

**Testing note:** No automated test suite exists for Schwung modules (per `schwung/CLAUDE.md`). Each task ends with a deploy + manual verification step. Use the unified logger (`/data/UserData/schwung/debug.log`) to confirm behavior.

---

## Pre-flight

Working directory throughout: `/Volumes/ExtFS/charlesvestal/github/schwung-parent/schwung-organelle/`

Schwung host source for reference: `../schwung/` (sibling).

Deploy command for host (if changes are made to Schwung itself): `(cd ../schwung && ./scripts/install.sh local --skip-modules --skip-confirmation)`.

Deploy command for this module: `./scripts/build.sh && ./scripts/install.sh`, then restart Schwung via `ssh ableton@move.local "systemctl --user restart schwung"` (or whatever's current — confirm in `../schwung/scripts/install.sh`).

---

### Task 1: Vendor libpd as a submodule and verify it builds

**Files:**
- Create: `.gitmodules`, `libs/libpd/` (submodule)

**Step 1: Add libpd as a submodule**

```bash
cd /Volumes/ExtFS/charlesvestal/github/schwung-parent/schwung-organelle
git submodule add https://github.com/libpd/libpd.git libs/libpd
cd libs/libpd
git submodule update --init --recursive   # pulls pure-data/ sub-submodule
cd ../..
```

**Step 2: Pin to a known-good upstream commit**

```bash
cd libs/libpd
git log --oneline | head -5    # note the current HEAD SHA
cd ../..
```

If you want to pin explicitly, `cd libs/libpd && git checkout <SHA> && cd ../.. && git add libs/libpd`.

**Step 3: Build libpd via Docker**

```bash
./scripts/build.sh
```

Expected: Docker builds the image (first time only), then libpd cmake configures with `PD_MULTI=ON PD_UTILS=OFF PD_EXTRA=ON`, then make. The build will then fail at the DSP plugin step because `pd_host.cpp` is a stub — that's expected. Confirm `build/libpd/libs/libpd.a` exists:

```bash
ls -la build/libpd/libs/libpd.a
```

Expected: file present, >1 MB.

**Step 4: Commit**

```bash
git add .gitmodules libs/libpd
git commit -m "deps: vendor libpd as submodule for multi-instance Pd host"
```

---

### Task 2: Sync `plugin_api_v2.h` against the live Schwung header

**Files:**
- Modify: `src/dsp/plugin_api_v2.h`

**Step 1: Diff against the source of truth**

```bash
diff src/dsp/plugin_api_v2.h ../schwung/src/host/plugin_api_v1.h
```

(Note: Schwung's header is named `plugin_api_v1.h` but contains both v1 and v2 structs.) Look for any host callbacks (`host_log`, `host_get_module_dir`, etc.) we should mirror. Copy any v2 type definitions our skeleton is missing.

**Step 2: Update our header**

Mirror exactly the types used by `plugin_api_v2_t` and `host_api_v1_t` so the vendored copy compiles standalone.

**Step 3: Commit**

```bash
git add src/dsp/plugin_api_v2.h
git commit -m "dsp: sync plugin_api_v2.h with schwung host header"
```

---

### Task 3: Plugin entry point with create/destroy + silent render

**Files:**
- Modify: `src/dsp/pd_host.cpp`

**Step 1: Implement the instance struct and entry points**

```cpp
#include "plugin_api_v2.h"
#include "z_libpd.h"
#include <cstring>
#include <cstdlib>

extern "C" {
#include "m_pd.h"
}

namespace {

struct OrganelleInstance {
    t_pdinstance* pd = nullptr;
    bool         audio_ready = false;
};

void* create_instance(const char* /*module_dir*/, const char* /*json_defaults*/) {
    static bool g_libpd_inited = false;
    if (!g_libpd_inited) {
        libpd_init();
        g_libpd_inited = true;
    }
    auto* inst = new OrganelleInstance{};
    inst->pd = libpd_new_instance();
    libpd_set_instance(inst->pd);
    libpd_init_audio(2, 2, 44100);
    inst->audio_ready = true;
    return inst;
}

void destroy_instance(void* p) {
    auto* inst = static_cast<OrganelleInstance*>(p);
    if (!inst) return;
    if (inst->pd) {
        libpd_set_instance(inst->pd);
        libpd_free_instance(inst->pd);
    }
    delete inst;
}

void on_midi(void* /*p*/, const uint8_t* /*msg*/, int /*len*/, int /*src*/) {}
void set_param(void* /*p*/, const char* /*key*/, const char* /*val*/) {}
int  get_param(void* /*p*/, const char* /*key*/, char* /*buf*/, int /*n*/) { return 0; }

void render_block(void* /*p*/, int16_t* out_lr, int frames) {
    std::memset(out_lr, 0, sizeof(int16_t) * 2 * frames);
}

} // anonymous

extern "C" plugin_api_v2_t* move_plugin_init_v2(const host_api_v1_t* /*host*/) {
    static plugin_api_v2_t api = {};
    api.api_version = 2;
    api.create_instance = create_instance;
    api.destroy_instance = destroy_instance;
    api.on_midi = on_midi;
    api.set_param = set_param;
    api.get_param = get_param;
    api.render_block = render_block;
    return &api;
}
```

**Step 2: Build**

```bash
./scripts/build.sh
ls -la dist/organelle/dsp.so
```

Expected: `dsp.so` produced, ~3–5 MB.

**Step 3: Deploy and verify the module loads in a shadow slot**

```bash
./scripts/install.sh
ssh ableton@move.local "systemctl --user restart schwung || pkill -HUP schwung"
ssh ableton@move.local "touch /data/UserData/schwung/debug_log_on"
```

On the device:
1. Open shadow mode (Shift+Vol+Track1).
2. Pick a slot → Synth → look for "Organelle" in the sound generator list.
3. Load it. Slot should accept it; no crash.
4. Check logs: `ssh ableton@move.local "tail -50 /data/UserData/schwung/debug.log"`. Expect `mm:` lines indicating module loaded; no SIGSEGV.

**Step 4: Commit**

```bash
git add src/dsp/pd_host.cpp
git commit -m "dsp: libpd instance lifecycle + silent render skeleton"
```

---

### Task 4: Wire libpd into render_block

**Files:**
- Modify: `src/dsp/pd_host.cpp`

**Step 1: Add float scratch buffers and call `libpd_process_short`**

In `OrganelleInstance` add:

```cpp
float in_buf[256];   // 128 frames × stereo
float out_buf[256];
```

(Allocate inline; struct lives the lifetime of the instance — no realtime malloc.)

Replace `render_block`:

```cpp
void render_block(void* p, int16_t* out_lr, int frames) {
    auto* inst = static_cast<OrganelleInstance*>(p);
    libpd_set_instance(inst->pd);

    // Input is zeros for now (Task 13 wires real line-in).
    std::memset(inst->in_buf, 0, sizeof(inst->in_buf));

    // Pd block = 64 frames; we get 128 → 2 ticks.
    const int ticks = frames / 64;
    libpd_process_float(ticks, inst->in_buf, inst->out_buf);

    // Convert float [-1, 1] → int16, interleaved stereo.
    for (int i = 0; i < frames * 2; ++i) {
        float s = inst->out_buf[i];
        if (s >  1.0f) s =  1.0f;
        if (s < -1.0f) s = -1.0f;
        out_lr[i] = static_cast<int16_t>(s * 32767.0f);
    }
}
```

**Step 2: Build, deploy**

```bash
./scripts/build.sh && ./scripts/install.sh
ssh ableton@move.local "systemctl --user restart schwung"
```

**Step 3: Verify on device**

Load Organelle in a slot. No patch is loaded → libpd renders silence. Confirm:
- No audio dropouts on Move's idle output.
- No `unified_log` warnings about render time.
- `top -p $(pidof schwung)` shows reasonable CPU (<10% expected for idle libpd).

**Step 4: Commit**

```bash
git add src/dsp/pd_host.cpp
git commit -m "dsp: route libpd_process_float through render_block (silent until patch loads)"
```

---

### Task 5: Patch loading via `set_param("patch_path", ...)`

**Files:**
- Modify: `src/dsp/pd_host.cpp`
- Modify: `src/dsp/patch_loader.cpp`, add header `src/dsp/patch_loader.h`

**Step 1: Implement `patch_loader.cpp`**

```cpp
// patch_loader.cpp
#include "patch_loader.h"
#include "z_libpd.h"
#include <cstring>
#include <libgen.h>     // basename
#include <string>

namespace organelle {

void* load_patch(const char* path, void** out_dir_token) {
    if (!path || !*path) return nullptr;
    // path is the patch *folder*, containing main.pd
    std::string dir = path;
    std::string file = "main.pd";
    void* handle = libpd_openfile(file.c_str(), dir.c_str());
    if (out_dir_token) *out_dir_token = handle;
    return handle;
}

void close_patch(void* handle) {
    if (handle) libpd_closefile(handle);
}

} // namespace organelle
```

And `patch_loader.h`:

```cpp
#pragma once
namespace organelle {
void* load_patch(const char* path, void** out_dir_token);
void  close_patch(void* handle);
}
```

**Step 2: Wire it into `set_param` in `pd_host.cpp`**

In `OrganelleInstance` add `void* current_patch = nullptr; std::string current_patch_path;`.

```cpp
void set_param(void* p, const char* key, const char* val) {
    auto* inst = static_cast<OrganelleInstance*>(p);
    if (!inst || !key) return;
    libpd_set_instance(inst->pd);
    if (std::strcmp(key, "patch_path") == 0) {
        if (inst->current_patch) {
            organelle::close_patch(inst->current_patch);
            inst->current_patch = nullptr;
        }
        if (val && *val) {
            inst->current_patch = organelle::load_patch(val, nullptr);
            inst->current_patch_path = val;
        } else {
            inst->current_patch_path.clear();
        }
    }
}
```

**Step 3: Test patch — create `patches/_test-sine/main.pd`**

Use a minimal vanilla Pd patch:

```
#N canvas 0 0 400 300 10;
#X obj 100 100 osc~ 440;
#X obj 100 130 *~ 0.2;
#X obj 100 160 dac~;
#X connect 0 0 1 0;
#X connect 1 0 2 0;
#X connect 1 0 2 1;
```

Save as `patches/_test-sine/main.pd`.

**Step 4: Deploy, load patch via Schwung console**

```bash
./scripts/build.sh && ./scripts/install.sh
```

On device, load Organelle in a slot. Without UI yet, set the patch via the unified log + a hardcoded test — easiest: add a one-line override in `create_instance` for now to `set_param(inst, "patch_path", "/data/UserData/schwung/organelle-patches/_test-sine")`. Build, deploy, listen for the 440 Hz sine.

**Step 5: Remove the hardcoded override, commit**

```bash
git add src/dsp/patch_loader.h src/dsp/patch_loader.cpp src/dsp/pd_host.cpp patches/_test-sine/main.pd
git commit -m "dsp: load Pd patches on set_param(patch_path)"
```

---

### Task 6: MIDI dispatch — notein / ctlin / bendin

**Files:**
- Modify: `src/dsp/pd_host.cpp`

**Step 1: Implement `on_midi`**

```cpp
void on_midi(void* p, const uint8_t* msg, int len, int /*source*/) {
    auto* inst = static_cast<OrganelleInstance*>(p);
    if (!inst || !msg || len < 1) return;
    libpd_set_instance(inst->pd);
    const uint8_t status = msg[0];
    const uint8_t hi = status & 0xF0;
    const int ch = status & 0x0F;
    if (hi == 0x90 && len >= 3) {
        if (msg[2] == 0) libpd_noteon(ch, msg[1], 0);
        else             libpd_noteon(ch, msg[1], msg[2]);
    } else if (hi == 0x80 && len >= 3) {
        libpd_noteon(ch, msg[1], 0);
    } else if (hi == 0xB0 && len >= 3) {
        libpd_controlchange(ch, msg[1], msg[2]);
    } else if (hi == 0xC0 && len >= 2) {
        libpd_programchange(ch, msg[1]);
    } else if (hi == 0xE0 && len >= 3) {
        int bend = ((msg[2] << 7) | msg[1]) - 8192;
        libpd_pitchbend(ch, bend);
    } else if (hi == 0xD0 && len >= 2) {
        libpd_aftertouch(ch, msg[1]);
    }
}
```

**Step 2: Test patch — `patches/_test-keys/main.pd`**

```
#N canvas 0 0 400 300 10;
#X obj 50 50 notein;
#X obj 50 80 mtof;
#X obj 50 110 osc~;
#X obj 50 140 *~ 0.2;
#X obj 50 170 dac~;
#X connect 0 0 1 0;
#X connect 1 0 2 0;
#X connect 2 0 3 0;
#X connect 3 0 4 0;
#X connect 3 0 4 1;
```

**Step 3: Deploy, hardcode the test-keys patch, hit pads**

Verify pads sound notes through the slot.

**Step 4: Commit**

```bash
git add src/dsp/pd_host.cpp patches/_test-keys/main.pd
git commit -m "dsp: forward slot MIDI to libpd (notein/ctlin/bendin/aftertouch)"
```

---

### Task 7: Parameter dispatch — knob1..4, volume, aux, fs, encoder

**Files:**
- Modify: `src/dsp/pd_host.cpp`

**Step 1: Extend `set_param`**

Add string helpers and dispatch:

```cpp
static void send_float(const char* recv, float v) {
    libpd_float(recv, v);
}

void set_param(void* p, const char* key, const char* val) {
    auto* inst = static_cast<OrganelleInstance*>(p);
    if (!inst || !key) return;
    libpd_set_instance(inst->pd);

    if (std::strcmp(key, "patch_path") == 0) { /* as in Task 5 */ return; }

    auto eq = [&](const char* k){ return std::strcmp(key, k) == 0; };
    const float fv = val ? std::strtof(val, nullptr) : 0.0f;

    if      (eq("knob1"))         send_float("knob1",         fv * 1023.0f / 127.0f);
    else if (eq("knob2"))         send_float("knob2",         fv * 1023.0f / 127.0f);
    else if (eq("knob3"))         send_float("knob3",         fv * 1023.0f / 127.0f);
    else if (eq("knob4"))         send_float("knob4",         fv * 1023.0f / 127.0f);
    else if (eq("volume"))        send_float("volume",        fv * 1023.0f);
    else if (eq("aux"))           send_float("auxKey",        fv != 0.0f ? 1.0f : 0.0f);
    else if (eq("fs"))            send_float("fs",            fv != 0.0f ? 1.0f : 0.0f);
    else if (eq("encoderInput"))  send_float("encoderInput",  fv);
    else if (eq("encoderButton")) send_float("encoderButton", fv != 0.0f ? 1.0f : 0.0f);
}
```

**Step 2: Test patch — `patches/_test-knobs/main.pd`**

```
#N canvas 0 0 400 300 10;
#X obj 50 50 r knob1;
#X obj 50 80 / 1023;
#X obj 50 110 * 1000;
#X obj 50 140 + 100;
#X obj 50 170 osc~;
#X obj 50 200 *~ 0.2;
#X obj 50 230 dac~;
#X connect 0 0 1 0;
#X connect 1 0 2 0;
#X connect 2 0 3 0;
#X connect 3 0 4 0;
#X connect 4 0 5 0;
#X connect 5 0 6 0;
#X connect 5 0 6 1;
```

**Step 3: Deploy, hardcode the test, twist knob 1 → pitch sweeps**

**Step 4: Commit**

```bash
git add src/dsp/pd_host.cpp patches/_test-knobs/main.pd
git commit -m "dsp: parameter dispatch for knob1-4, volume, aux/fs, encoder"
```

---

### Task 8: Screen op ring buffer + parser

**Files:**
- Modify: `src/dsp/screen_translator.cpp`, add `src/dsp/screen_translator.h`
- Modify: `src/dsp/pd_host.cpp`

**Step 1: Implement the ring buffer**

`screen_translator.h`:

```cpp
#pragma once
#include <cstdint>

namespace organelle {

constexpr int RING_OPS    = 256;
constexpr int OP_TEXT_MAX = 32;

enum ScreenOpKind : uint8_t {
    OP_NONE = 0,
    OP_FILL, OP_LINE, OP_BOX, OP_INVERT, OP_PIXEL, OP_PRINT, OP_FLIP, OP_CLEAR,
};

struct ScreenOp {
    ScreenOpKind kind;
    int16_t a, b, c, d, e;   // generic args
    char    text[OP_TEXT_MAX];
};

class ScreenOpRing {
public:
    void push(const ScreenOp& op);          // realtime safe, drops oldest on overflow
    int  drain_json(char* out, int cap);    // called from non-RT (get_param), returns bytes
private:
    ScreenOp buf_[RING_OPS] = {};
    uint32_t head_ = 0;   // RT writer
    uint32_t tail_ = 0;   // non-RT reader
};

void handle_pd_message(const char* recv, const char* sel, int argc, struct _atom* argv, ScreenOpRing& ring);

} // namespace organelle
```

`screen_translator.cpp`: implement the SPSC ring with `__atomic_*` operations, and the message parser that maps `[s oled gFillArea 0 0 128 64 0]` etc. to `ScreenOp{OP_FILL, 0, 0, 128, 64, 0, ""}`.

**Step 2: Wire it into `pd_host.cpp`**

Add `ScreenOpRing ring;` per instance. In `create_instance`:

```cpp
libpd_set_instance(inst->pd);
libpd_set_concatenated_message_callback([](const char* recv, const char* msg){
    // We registered a thread-local instance pointer; look it up.
});
libpd_bind("oled");
```

(libpd's actual hook API: use `libpd_set_message_hook` for symbol/list/float dispatch. Pick the variant that gives the cleanest dispatch path. The instance pointer needs to be stashed via a thread-local or a global map keyed by `t_pdinstance*`. Confirm by reading `libpd_wrapper/z_libpd.h` in `libs/libpd/`.)

In the hook, when `recv == "oled"`, call `handle_pd_message(...)` with our ring.

Implement `get_param("screen_ops")`:

```cpp
int get_param(void* p, const char* key, char* buf, int n) {
    auto* inst = static_cast<OrganelleInstance*>(p);
    if (!inst || !key || !buf) return 0;
    if (std::strcmp(key, "screen_ops") == 0) {
        return inst->ring.drain_json(buf, n);
    }
    return 0;
}
```

**Step 3: Test patch — `patches/_test-screen/main.pd`**

```
#N canvas 0 0 400 300 10;
#X obj 50 50 loadbang;
#X msg 50 80 gFillArea 0 0 128 64 0 \; gPrintln 10 28 16 1 HELLO \; gFlip;
#X obj 50 110 s oled;
#X connect 0 0 1 0;
#X connect 1 0 2 0;
```

**Step 4: Verify**

Deploy with hardcoded patch. Without ui_chain.js drawing yet, you'll only see the screen ops via a debug `get_param` test:

```bash
ssh ableton@move.local "schwung-cli get_param organelle screen_ops"
```

(Or whatever Schwung exposes for param introspection — check `../schwung/src/host/`.) Expected output: JSON array with `fill` + `print` + `flip` ops.

**Step 5: Commit**

```bash
git add src/dsp/screen_translator.h src/dsp/screen_translator.cpp src/dsp/pd_host.cpp patches/_test-screen/main.pd
git commit -m "dsp: screen op ring buffer + [s oled] message parser"
```

---

### Task 9: `ui_chain.js` State A — patch browser

**Files:**
- Modify: `src/module/ui_chain.js`

**Step 1: Implement the browser**

```javascript
import {
    drawMenuHeader,
    drawMenuList,
    drawMenuFooter,
    menuLayoutDefaults,
} from '../../shared/menu_layout.mjs';
import { shouldFilterMessage } from '../../shared/input_filter.mjs';

const PATCH_ROOT = '/data/UserData/schwung/organelle-patches';

let state = 'browser';     // 'browser' | 'running'
let patches = [];
let selectedIndex = 0;

function scanPatches() {
    // host_list_dir is the canonical helper; if not present we shell out via host_read_file on an index file.
    const entries = host_list_dir(PATCH_ROOT) || [];
    return entries
        .filter(e => e.is_dir)
        .filter(e => host_file_exists(`${PATCH_ROOT}/${e.name}/main.pd`))
        .map(e => ({ name: e.name, path: `${PATCH_ROOT}/${e.name}` }))
        .sort((a, b) => a.name.localeCompare(b.name));
}

function drawBrowser() {
    clear();
    drawMenuHeader('Organelle Patches');
    drawMenuList({
        items: patches,
        selectedIndex,
        listArea: {
            topY: menuLayoutDefaults.listTopY,
            bottomY: menuLayoutDefaults.listBottomWithFooter,
        },
        getLabel: (p) => p.name,
    });
    drawMenuFooter(`${patches.length} patches`);
    flip();
}

function init() {
    patches = scanPatches();
    drawBrowser();
}

function tick() {
    if (state === 'browser') {
        // Browser is event-driven; nothing to do per tick.
        return;
    }
    // State B handled in Task 10.
}

function loadSelected() {
    if (!patches.length) return;
    const p = patches[selectedIndex];
    host_module_set_param('patch_path', p.path);
    state = 'running';
}

function onMidiMessageInternal(data) {
    if (shouldFilterMessage(data)) return;
    if (state !== 'browser') { /* Task 11 handles running state */ return; }
    const [status, d1, d2] = data;
    // Jog turn: CC 14 (relative-ish; treat any nonzero as direction)
    if (status === 0xB0 && d1 === 14) {
        const delta = d2 < 64 ? d2 : d2 - 128;
        if (delta > 0) selectedIndex = Math.min(patches.length - 1, selectedIndex + 1);
        if (delta < 0) selectedIndex = Math.max(0, selectedIndex - 1);
        drawBrowser();
        return;
    }
    // Jog click: CC 3, value > 0 = press
    if (status === 0xB0 && d1 === 3 && d2 > 0) {
        loadSelected();
        return;
    }
}

function onMidiMessageExternal(_data) {}

globalThis.chain_ui = { init, tick, onMidiMessageInternal, onMidiMessageExternal };
```

(Confirm `host_list_dir` exists in Schwung; if not, the alternative is `host_read_file` of an index manifest produced by the install script. Read `../schwung/src/host/js_host_functions.c` or the equivalent.)

**Step 2: Deploy, open slot editor, scroll patches**

```bash
./scripts/build.sh && ./scripts/install.sh
ssh ableton@move.local "systemctl --user restart schwung"
```

On device: load Organelle in a slot, long-press the track → patch browser appears. Jog scrolls. Jog click loads a patch (Pd output should now produce sound). The browser screen stays even after load because State B isn't drawn yet.

**Step 3: Commit**

```bash
git add src/module/ui_chain.js
git commit -m "ui: patch browser (State A)"
```

---

### Task 10: `ui_chain.js` State B — Pd screen passthrough

**Files:**
- Modify: `src/module/ui_chain.js`

**Step 1: Add the renderer**

```javascript
function executeOps(ops) {
    for (const o of ops) {
        switch (o.op) {
            case 'clear':  clear(); break;
            case 'fill':   rect(o.x, o.y, o.w, o.h, o.c); break;
            case 'line':   line(o.x1, o.y1, o.x2, o.y2, o.c); break;
            case 'box':    drawBox(o.x, o.y, o.w, o.h, o.c); break;
            case 'invert': invertArea(o.x, o.y, o.w, o.h); break;
            case 'pixel':  pixel(o.x, o.y, o.c); break;
            case 'print':  print(o.x, o.y, o.text, o.size); break;
            case 'flip':   flip(); break;
        }
    }
}

function tick() {
    if (state === 'browser') return;
    const json = host_module_get_param('screen_ops');
    if (!json) return;
    let ops;
    try { ops = JSON.parse(json); } catch { return; }
    if (Array.isArray(ops) && ops.length) executeOps(ops);
}
```

Confirm the actual Move JS draw API names (`rect`, `line`, `pixel`, `print`, `clear`, `flip`, `invertArea`, `drawBox`) match what Schwung exposes. Check `../schwung/docs/API.md`.

**Step 2: Test**

Deploy. Load `_test-screen` patch. Move display shows "HELLO" centered.

**Step 3: Commit**

```bash
git add src/module/ui_chain.js
git commit -m "ui: Pd screen passthrough (State B)"
```

---

### Task 11: Input routing in State B — knobs, jog, knob touches, back

**Files:**
- Modify: `src/module/ui_chain.js`

**Step 1: Extend `onMidiMessageInternal`**

```javascript
function onMidiMessageInternal(data) {
    if (shouldFilterMessage(data)) return;
    const [status, d1, d2] = data;

    // Knob capacitive touches: notes 0-9 from Move's filter docs.
    if ((status & 0xF0) === 0x90 || (status & 0xF0) === 0x80) {
        const on = (status & 0xF0) === 0x90 && d2 > 0;
        if (d1 === 7) { host_module_set_param('aux', on ? '1' : '0'); return; }
        if (d1 === 8) { host_module_set_param('fs',  on ? '1' : '0'); return; }
    }

    // Knob CCs (71-78 per Schwung HW MIDI map; we want 71-74 for knobs 1-4).
    if (status === 0xB0 && d1 >= 71 && d1 <= 74) {
        const knob = d1 - 71 + 1;
        host_module_set_param(`knob${knob}`, String(d2));
        return;
    }

    // Jog
    if (status === 0xB0 && d1 === 14) {
        const delta = d2 < 64 ? d2 : d2 - 128;
        if (state === 'browser') {
            if (delta > 0) selectedIndex = Math.min(patches.length - 1, selectedIndex + 1);
            if (delta < 0) selectedIndex = Math.max(0, selectedIndex - 1);
            drawBrowser();
        } else {
            host_module_set_param('encoderInput', String(delta));
        }
        return;
    }
    if (status === 0xB0 && d1 === 3) {
        if (state === 'browser') { if (d2 > 0) loadSelected(); }
        else { host_module_set_param('encoderButton', d2 > 0 ? '1' : '0'); }
        return;
    }

    // Back button (CC 51)
    if (status === 0xB0 && d1 === 51 && d2 > 0) {
        if (state === 'running') {
            host_module_set_param('patch_path', '');
            state = 'browser';
            drawBrowser();
        }
        return;
    }
}
```

**Step 2: Deploy and exercise**

- Load `_test-knobs`, verify knob 1 sweeps pitch.
- Load a patch that uses `[r encoderInput]`, scroll jog → expect the patch's reaction.
- Touch knob 7 / knob 8 — verify aux/fs behavior in a foot-switch-using patch.
- Press Back → returns to patch browser.

**Step 3: Commit**

```bash
git add src/module/ui_chain.js
git commit -m "ui: input routing in State B (knobs, jog, touches, back)"
```

---

### Task 12: Audio in for `[adc~]`

**Files:**
- Modify: `src/dsp/pd_host.cpp`

**Step 1: Accept input in render_block**

The Schwung Plugin API v2 `render_block` signature only has `out_lr`. Confirm by reading `../schwung/src/host/plugin_api_v1.h` (it actually wraps both). If v2 lacks `in_lr`, we'll need a host pull — check the chain mixer source `../schwung/src/host/shadow_chain_process_fx.c` for how `audio_fx` modules receive input. The likely answer is a separate buffer the host writes before calling `render_block`, or a `host->get_audio_in()` callback.

Implement: feed converted int16→float input into `libpd_process_float` input buffer if `audio_in_enable == "On"`; else zeros.

**Step 2: Add `audio_in_enable` param**

```cpp
bool audio_in_enable = true;   // in instance struct
// in set_param: if (eq("audio_in_enable")) inst->audio_in_enable = (std::strcmp(val, "On") == 0);
```

**Step 3: Test patch — `patches/_test-passthrough/main.pd`**

```
#N canvas 0 0 400 300 10;
#X obj 50 50 adc~;
#X obj 50 80 dac~;
#X connect 0 0 1 0;
#X connect 0 1 1 1;
```

Deploy. Plug in line-in. Verify audio passes through. Flip `audio_in_enable` Off in slot settings → input mutes.

**Step 4: Commit**

```bash
git add src/dsp/pd_host.cpp patches/_test-passthrough/main.pd
git commit -m "dsp: line-in audio routed to [adc~] (toggleable)"
```

---

### Task 13: MIDI out for `[noteout]` / `[ctlout]`

**Files:**
- Modify: `src/dsp/pd_host.cpp`

**Step 1: Set libpd output hooks**

```cpp
// in create_instance, after libpd_set_instance(inst->pd):
libpd_set_noteonhook([](int ch, int pitch, int vel){
    uint8_t msg[3] = { (uint8_t)(0x90 | (ch & 0x0F)), (uint8_t)pitch, (uint8_t)vel };
    host_module_send_midi(msg, 3, /*source*/ 0);
});
libpd_set_controlchangehook([](int ch, int ctl, int val){
    uint8_t msg[3] = { (uint8_t)(0xB0 | (ch & 0x0F)), (uint8_t)ctl, (uint8_t)val };
    host_module_send_midi(msg, 3, 0);
});
// programchange, pitchbend, aftertouch similarly
```

We need a host-side send-MIDI callback. Confirm v2 host API: read `../schwung/src/host/plugin_api_v1.h` for `host_api_v1_t`. If absent, we'll route via a per-tick queue drained by JS calling `host_module_get_param("midi_out_queue")` and then `host_module_send_midi`. Use the queue approach if no direct host callback.

**Step 2: Test patch — `patches/_test-midi-out/main.pd`**

```
#N canvas 0 0 400 300 10;
#X obj 50 50 metro 500;
#X obj 50 80 loadbang;
#X obj 50 110 t b;
#X obj 50 140 60;
#X obj 50 170 noteout;
#X connect 1 0 0 0;
#X connect 0 0 2 0;
#X connect 2 0 3 0;
#X connect 3 0 4 0;
```

Deploy, plug external synth via USB, expect note C4 every 500 ms.

**Step 3: Commit**

```bash
git add src/dsp/pd_host.cpp patches/_test-midi-out/main.pd
git commit -m "dsp: forward Pd [noteout]/[ctlout] to slot MIDI out"
```

---

### Task 14: Tempo injection

**Files:**
- Modify: `src/dsp/pd_host.cpp`

**Step 1: Add `set_param("chain_tempo", bpm)`**

Schwung's chain host calls `set_param("chain_tempo", "120.0")` on tempo changes (confirm by grepping `../schwung/src/modules/chain/dsp/chain_host.c`). Each call:

```cpp
if (eq("chain_tempo")) send_float("clock", fv);
```

**Step 2: Test patch — `patches/_test-clock/main.pd`**

```
#N canvas 0 0 400 300 10;
#X obj 50 50 r clock;
#X floatatom 50 80 5 0 0;
```

Open `_test-clock` patch and verify the floatatom shows the current tempo (will only be visible via a screen patch unfortunately; replace with a `[s oled]` printout if needed).

**Step 3: Commit**

```bash
git add src/dsp/pd_host.cpp patches/_test-clock/main.pd
git commit -m "dsp: inject chain tempo on [r clock]"
```

---

### Task 15: Slot params + `state` persistence

**Files:**
- Modify: `src/dsp/pd_host.cpp`

**Step 1: Add `chain_params` and `state` handlers**

```cpp
int get_param(void* p, const char* key, char* buf, int n) {
    auto* inst = static_cast<OrganelleInstance*>(p);
    if (std::strcmp(key, "chain_params") == 0) {
        // Static metadata for slot settings menu.
        static const char* params_json = R"([
            {"key":"patch_path","name":"Patch","type":"enum","options_param":"patch_list"},
            {"key":"octave_transpose","name":"Octave","type":"int","min":-4,"max":4,"default":0},
            {"key":"audio_in_enable","name":"Line In to Pd","type":"enum","options":["Off","On"],"default":"On"},
            {"key":"gain","name":"Output Gain","type":"float","min":0,"max":1.5,"step":0.01,"default":1.0},
            {"key":"aux_source","name":"Aux Button","type":"enum","options":["Knob7 Touch","Off"],"default":"Knob7 Touch"},
            {"key":"fs_source","name":"Foot Switch","type":"enum","options":["Knob8 Touch","Off"],"default":"Knob8 Touch"}
        ])";
        std::strncpy(buf, params_json, n);
        return std::strlen(buf);
    }
    if (std::strcmp(key, "state") == 0) {
        int w = std::snprintf(buf, n,
            "{\"patch_path\":\"%s\",\"audio_in_enable\":\"%s\",\"gain\":%.3f}",
            inst->current_patch_path.c_str(),
            inst->audio_in_enable ? "On" : "Off",
            inst->gain);
        return w;
    }
    if (std::strcmp(key, "screen_ops") == 0) return inst->ring.drain_json(buf, n);
    return 0;
}
```

In `set_param`, when `key == "state"`, parse the JSON and re-apply each field.

**Step 2: Test persistence**

Deploy. Load a patch in a slot, save the slot via Schwung's save flow. Reboot Move. Re-enter shadow → confirm slot reloads with the same patch.

**Step 3: Commit**

```bash
git add src/dsp/pd_host.cpp
git commit -m "dsp: chain_params metadata + slot state persistence"
```

---

### Task 16: Bundle a starter patch set

**Files:**
- Add: `patches/Basic-Synth/main.pd`, `patches/Basic-Synth/metadata.json`
- Add: 2–3 more vanilla Pd patches (folder + main.pd + metadata.json each)

**Step 1: Pick 3 patches we know are vanilla-clean**

Suggested:
- **Basic-Synth** — `osc~` saw + filter + ADSR, knob1=cutoff, knob2=resonance, knob3=attack, knob4=release.
- **Sub-Bass** — `phasor~` + lowpass, monophonic.
- **Drum-Pads** — small percussion module that triggers off note numbers.

Hand-write `main.pd` for each (or import from public-domain Mother examples that are vanilla — *verify each is GPL/CC-compatible before committing*).

**Step 2: Add `metadata.json` per patch**

```json
{
    "name": "Basic Synth",
    "author": "schwung-organelle",
    "description": "Saw oscillator into resonant LPF with ADSR.",
    "knob_labels": ["Cutoff", "Resonance", "Attack", "Release"]
}
```

**Step 3: Verify install seeds them**

```bash
./scripts/build.sh && ./scripts/install.sh
ssh ableton@move.local "ls /data/UserData/schwung/organelle-patches/"
```

Expected: starter folders present on first install; preserved across re-installs.

**Step 4: Commit**

```bash
git add patches/
git commit -m "patches: starter set (Basic-Synth, Sub-Bass, Drum-Pads)"
```

---

### Task 17: Add `organelle` to Schwung's module catalog

**Files:**
- Modify: `../schwung/module-catalog.json`

**Step 1: Add the entry**

```json
{
    "id": "organelle",
    "name": "Organelle",
    "description": "Critter & Guitari Organelle Pure Data patch host (libpd)",
    "author": "charlesvestal",
    "component_type": "sound_generator",
    "github_repo": "charlesvestal/schwung-organelle",
    "default_branch": "main",
    "asset_name": "organelle-module.tar.gz",
    "min_host_version": "0.9.12",
    "requires": "Bundled starter patches included; drop more into /data/UserData/schwung/organelle-patches/ via SFTP"
}
```

**Step 2: Commit in the schwung repo**

```bash
cd ../schwung
git add module-catalog.json
git commit -m "catalog: add Organelle (sound_generator)"
cd ../schwung-organelle
```

(Push to remote only after Task 18 ships an actual release.)

---

### Task 18: Tag v0.1.0 and ship

**Files:**
- Modify: `src/module/module.json` (version)

**Step 1: Bump module.json version to 0.1.0**

**Step 2: Push to GitHub**

```bash
gh repo create charlesvestal/schwung-organelle --public --source=. --remote=origin --push
```

(If the repo already exists: `git remote add origin git@github.com:charlesvestal/schwung-organelle.git && git push -u origin main`.)

**Step 3: Tag**

```bash
git tag v0.1.0
git push --tags
```

**Step 4: Watch the GitHub Action**

```bash
gh run watch
```

Expected: Docker build succeeds, tarball uploaded to release `v0.1.0`, `release.json` auto-updates on `main`.

**Step 5: Add release notes**

```bash
gh release edit v0.1.0 -R charlesvestal/schwung-organelle --notes "$(cat <<'EOF'
- Initial release.
- Runs vanilla Pure Data patches (no externals) on Move shadow slots.
- Maps Organelle's knobs, encoder, aux, foot switch (knob 7/8 touch), and 128×64 OLED to Move controls.
- Starter set: Basic Synth, Sub-Bass, Drum-Pads.
- Patches live in /data/UserData/schwung/organelle-patches/ — drop more in via SFTP.
EOF
)"
```

**Step 6: Verify Module Store picks it up**

On device: open Module Store → look for "Organelle" in the sound generators section. Install. Smoke test: load a slot, scroll the patch browser, sound the test sine.

**Step 7: Push the catalog entry**

```bash
cd ../schwung
git push
```

---

## Post-v0.1 backlog (not in this plan)

- Externals scaffold (`libs/externals/`) — `freeverb~`, `else`, `cyclone` as first additions.
- Separate `organelle-launcher` overtake module (Tools menu).
- Knob-label live display from patch's `[s knob1Label]` messages.
- In-device patchstorage.com browser.
- ORAC (its own design effort).
