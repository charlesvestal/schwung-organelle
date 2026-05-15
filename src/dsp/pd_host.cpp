// Organelle module — libpd host (Plugin API v2).
//
// Embeds Pure Data via libpd (multi-instance). Each Move shadow slot that
// loads this module gets its own t_pdinstance with independent state.

#include "plugin_api_v2.h"
#include "patch_loader.h"
#include "screen_translator.h"

#include "z_libpd.h"

extern "C" {
#include "m_pd.h"
}

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

namespace {

// ----- shared host pointer -----
// Stashed at module init; used by render_block to read audio-in, by hooks
// to emit slot MIDI, and (later) by callbacks to query tempo.
const host_api_v1_t* g_host = nullptr;

// ----- libpd one-time init guard -----
bool g_libpd_inited = false;
std::mutex g_libpd_global_mtx;

// ----- per-instance state -----
struct Instance {
    t_pdinstance*  pd = nullptr;
    void*          current_patch = nullptr;
    void*          mother_handle = nullptr;   // host wrapper providing catch~/dac~
    std::string    current_patch_path;
    std::string    module_dir;

    // Slot params
    int            octave_transpose = 0;
    bool           audio_in_enable = true;
    float          gain = 1.0f;

    // Audio scratch (no realtime alloc).
    float          in_buf[MOVE_FRAMES_PER_BLOCK * 2];
    float          out_buf[MOVE_FRAMES_PER_BLOCK * 2];

    // Pending MIDI out from libpd hooks, drained by render_block tail.
    static constexpr int MIDI_OUT_RING = 64;
    uint8_t        midi_out_buf[MIDI_OUT_RING][3];
    std::atomic<uint32_t> midi_out_head{0};
    std::atomic<uint32_t> midi_out_tail{0};

    // Screen op ring.
    organelle::ScreenOpRing screen_ring;

    // Tempo tracking (avoid sending duplicate float values every tick).
    float          last_tempo_sent = -1.0f;
};

// Map t_pdinstance* → Instance* so libpd hooks (which only know the
// current instance pointer via libpd_this_instance()) can find their
// owning Instance and route messages/MIDI back.
std::mutex g_inst_map_mtx;
std::unordered_map<t_pdinstance*, Instance*> g_inst_map;

Instance* current_instance_from_libpd() {
    t_pdinstance* pd = libpd_this_instance();
    std::lock_guard<std::mutex> lk(g_inst_map_mtx);
    auto it = g_inst_map.find(pd);
    return it == g_inst_map.end() ? nullptr : it->second;
}

// ===== libpd hooks =====

void on_pd_message(const char* recv, const char* sel, int argc, t_atom* argv) {
    Instance* inst = current_instance_from_libpd();
    if (!inst || !recv || !sel) return;
    if (std::strcmp(recv, "oled") == 0) {
        organelle::handle_oled_message(sel, argc, argv, inst->screen_ring);
    }
}

void on_pd_noteon(int ch, int pitch, int vel) {
    Instance* inst = current_instance_from_libpd();
    if (!inst) return;
    const uint32_t head = inst->midi_out_head.load(std::memory_order_relaxed);
    const uint32_t next = (head + 1) % Instance::MIDI_OUT_RING;
    if (next == inst->midi_out_tail.load(std::memory_order_acquire)) return; // drop
    inst->midi_out_buf[head][0] = static_cast<uint8_t>(0x90 | (ch & 0x0F));
    inst->midi_out_buf[head][1] = static_cast<uint8_t>(pitch & 0x7F);
    inst->midi_out_buf[head][2] = static_cast<uint8_t>(vel & 0x7F);
    inst->midi_out_head.store(next, std::memory_order_release);
}

void on_pd_controlchange(int ch, int ctl, int val) {
    Instance* inst = current_instance_from_libpd();
    if (!inst) return;
    const uint32_t head = inst->midi_out_head.load(std::memory_order_relaxed);
    const uint32_t next = (head + 1) % Instance::MIDI_OUT_RING;
    if (next == inst->midi_out_tail.load(std::memory_order_acquire)) return;
    inst->midi_out_buf[head][0] = static_cast<uint8_t>(0xB0 | (ch & 0x0F));
    inst->midi_out_buf[head][1] = static_cast<uint8_t>(ctl & 0x7F);
    inst->midi_out_buf[head][2] = static_cast<uint8_t>(val & 0x7F);
    inst->midi_out_head.store(next, std::memory_order_release);
}

void on_pd_programchange(int ch, int val) {
    Instance* inst = current_instance_from_libpd();
    if (!inst) return;
    const uint32_t head = inst->midi_out_head.load(std::memory_order_relaxed);
    const uint32_t next = (head + 1) % Instance::MIDI_OUT_RING;
    if (next == inst->midi_out_tail.load(std::memory_order_acquire)) return;
    inst->midi_out_buf[head][0] = static_cast<uint8_t>(0xC0 | (ch & 0x0F));
    inst->midi_out_buf[head][1] = static_cast<uint8_t>(val & 0x7F);
    inst->midi_out_buf[head][2] = 0;
    inst->midi_out_head.store(next, std::memory_order_release);
}

void on_pd_pitchbend(int ch, int val) {
    Instance* inst = current_instance_from_libpd();
    if (!inst) return;
    const uint32_t head = inst->midi_out_head.load(std::memory_order_relaxed);
    const uint32_t next = (head + 1) % Instance::MIDI_OUT_RING;
    if (next == inst->midi_out_tail.load(std::memory_order_acquire)) return;
    const int bend = (val + 8192) & 0x3FFF;
    inst->midi_out_buf[head][0] = static_cast<uint8_t>(0xE0 | (ch & 0x0F));
    inst->midi_out_buf[head][1] = static_cast<uint8_t>(bend & 0x7F);
    inst->midi_out_buf[head][2] = static_cast<uint8_t>((bend >> 7) & 0x7F);
    inst->midi_out_head.store(next, std::memory_order_release);
}

// Pd patches can emit error prints during render_block (which runs on the
// realtime SPI callback thread). Calling g_host->log from there triggers
// file I/O and will stall audio — Genny-1 spams "screenLine: no such
// object" at ~300/sec and the device locks up. No print hook for now.

// libpd hooks are PER-INSTANCE after libpd_init. Set them AFTER each
// libpd_new_instance + libpd_set_instance, not once globally.
void install_instance_hooks() {
    libpd_set_messagehook(on_pd_message);
    libpd_set_noteonhook(on_pd_noteon);
    libpd_set_controlchangehook(on_pd_controlchange);
    libpd_set_programchangehook(on_pd_programchange);
    libpd_set_pitchbendhook(on_pd_pitchbend);
}

// ===== plugin entry points =====

void* create_instance(const char* module_dir, const char* /*json_defaults*/) {
    {
        std::lock_guard<std::mutex> lk(g_libpd_global_mtx);
        if (!g_libpd_inited) {
            libpd_init();
            g_libpd_inited = true;
        }
    }

    auto* inst = new Instance();
    inst->pd = libpd_new_instance();
    libpd_set_instance(inst->pd);

    {
        std::lock_guard<std::mutex> lk(g_inst_map_mtx);
        g_inst_map[inst->pd] = inst;
    }

    // Hooks are PER-INSTANCE after libpd_init — install on this instance now.
    install_instance_hooks();

    libpd_init_audio(2, 2, MOVE_SAMPLE_RATE);

    // Subscribe to [s oled] in this instance.
    libpd_bind("oled");

    // Pd needs an explicit "start audio" message.
    libpd_start_message(1);
    libpd_add_float(1.0f);
    libpd_finish_message("pd", "dsp");

    // Load the Organelle "mother" wrapper. It provides catch~ outL/outR →
    // dac~ (so patches using [throw~ outL]/[throw~ outR] are audible) and
    // adc~ → s~ inL/inR (so patches using [r~ inL]/[r~ inR] get line-in).
    if (module_dir && *module_dir) {
        inst->module_dir = module_dir;
        inst->mother_handle = libpd_openfile("mother.pd", module_dir);
        if (g_host && g_host->log) {
            char line[256];
            std::snprintf(line, sizeof(line),
                "[organelle] mother.pd %s (dir=%s)",
                inst->mother_handle ? "loaded" : "FAILED to load", module_dir);
            g_host->log(line);
        }
    }

    if (g_host && g_host->log) {
        g_host->log("[organelle] create_instance: libpd ready, dsp on");
    }
    return inst;
}

void destroy_instance(void* p) {
    auto* inst = static_cast<Instance*>(p);
    if (!inst) return;
    if (inst->pd) {
        libpd_set_instance(inst->pd);
        if (inst->current_patch) {
            organelle::close_patch(inst->current_patch);
            inst->current_patch = nullptr;
        }
        if (inst->mother_handle) {
            organelle::close_patch(inst->mother_handle);
            inst->mother_handle = nullptr;
        }
        {
            std::lock_guard<std::mutex> lk(g_inst_map_mtx);
            g_inst_map.erase(inst->pd);
        }
        libpd_free_instance(inst->pd);
    }
    delete inst;
}

void on_midi(void* p, const uint8_t* msg, int len, int /*source*/) {
    auto* inst = static_cast<Instance*>(p);
    if (!inst || !msg || len < 1 || !inst->pd) return;
    libpd_set_instance(inst->pd);

    const uint8_t status = msg[0];
    const uint8_t hi = status & 0xF0;
    const int ch = status & 0x0F;

    if (hi == 0x90 && len >= 3) {
        int pitch = msg[1] + inst->octave_transpose * 12;
        if (pitch < 0) pitch = 0;
        if (pitch > 127) pitch = 127;
        libpd_noteon(ch, pitch, msg[2]);
        // Also fire [r notes] (Organelle convention: list of [pitch velocity]).
        libpd_start_message(2);
        libpd_add_float(static_cast<float>(pitch));
        libpd_add_float(static_cast<float>(msg[2]));
        libpd_finish_list("notes");
    } else if (hi == 0x80 && len >= 3) {
        int pitch = msg[1] + inst->octave_transpose * 12;
        if (pitch < 0) pitch = 0;
        if (pitch > 127) pitch = 127;
        libpd_noteon(ch, pitch, 0);
        libpd_start_message(2);
        libpd_add_float(static_cast<float>(pitch));
        libpd_add_float(0.0f);
        libpd_finish_list("notes");
    } else if (hi == 0xB0 && len >= 3) {
        libpd_controlchange(ch, msg[1], msg[2]);
    } else if (hi == 0xC0 && len >= 2) {
        libpd_programchange(ch, msg[1]);
    } else if (hi == 0xE0 && len >= 3) {
        const int bend = ((msg[2] << 7) | msg[1]) - 8192;
        libpd_pitchbend(ch, bend);
    } else if (hi == 0xD0 && len >= 2) {
        libpd_aftertouch(ch, msg[1]);
    } else if (hi == 0xA0 && len >= 3) {
        libpd_polyaftertouch(ch, msg[1], msg[2]);
    }
}

static void send_float(const char* recv, float v) {
    libpd_float(recv, v);
}

void set_param(void* p, const char* key, const char* val) {
    auto* inst = static_cast<Instance*>(p);
    if (!inst || !key || !inst->pd) return;
    libpd_set_instance(inst->pd);

    const float fv = val ? std::strtof(val, nullptr) : 0.0f;

    if (std::strcmp(key, "patch_path") == 0) {
        if (inst->current_patch) {
            organelle::close_patch(inst->current_patch);
            inst->current_patch = nullptr;
            inst->current_patch_path.clear();
        }
        if (val && *val) {
            inst->current_patch = organelle::load_patch(val);
            if (inst->current_patch) {
                inst->current_patch_path = val;
                if (g_host && g_host->log) {
                    char line[512];
                    std::snprintf(line, sizeof(line), "[organelle] patch loaded: %s", val);
                    g_host->log(line);
                }
            } else if (g_host && g_host->log) {
                char line[512];
                std::snprintf(line, sizeof(line), "[organelle] patch LOAD FAILED: %s", val);
                g_host->log(line);
            }
        }
        return;
    }

    if (std::strcmp(key, "octave_transpose") == 0) {
        int oct = static_cast<int>(fv);
        if (oct < -4) oct = -4;
        if (oct >  4) oct =  4;
        inst->octave_transpose = oct;
        return;
    }
    if (std::strcmp(key, "audio_in_enable") == 0) {
        inst->audio_in_enable = val && std::strcmp(val, "On") == 0;
        return;
    }
    if (std::strcmp(key, "gain") == 0) {
        inst->gain = fv;
        if (inst->gain < 0.0f) inst->gain = 0.0f;
        if (inst->gain > 4.0f) inst->gain = 4.0f;
        return;
    }
    if (std::strcmp(key, "aux_source") == 0 || std::strcmp(key, "fs_source") == 0) {
        // Stored client-side (JS); module doesn't need to remember the mapping.
        return;
    }

    // Knob values arrive already scaled 0-1023 (JS integrates relative deltas).
    if (std::strcmp(key, "knob1") == 0) { send_float("knob1", fv); return; }
    if (std::strcmp(key, "knob2") == 0) { send_float("knob2", fv); return; }
    if (std::strcmp(key, "knob3") == 0) { send_float("knob3", fv); return; }
    if (std::strcmp(key, "knob4") == 0) { send_float("knob4", fv); return; }
    if (std::strcmp(key, "volume") == 0)        { send_float("volume", fv * 1023.0f / 127.0f); return; }
    if (std::strcmp(key, "aux") == 0)           { send_float("auxKey", fv != 0.0f ? 1.0f : 0.0f); return; }
    if (std::strcmp(key, "fs") == 0)            { send_float("fs",     fv != 0.0f ? 1.0f : 0.0f); return; }
    if (std::strcmp(key, "encoderInput") == 0)  { send_float("encoderInput", fv); return; }
    if (std::strcmp(key, "encoderButton") == 0) { send_float("encoderButton", fv != 0.0f ? 1.0f : 0.0f); return; }
    if (std::strcmp(key, "chain_tempo") == 0)   { send_float("clock", fv); inst->last_tempo_sent = fv; return; }

    if (std::strcmp(key, "state") == 0 && val) {
        // Minimal JSON parse: look for "patch_path":"...", "audio_in_enable":"...",
        // "gain":<num>, "octave_transpose":<num>. Keep it dumb on purpose; the
        // host always writes these via get_param("state") which we control.
        const char* s = val;
        auto find_str = [&](const char* k, std::string& out) {
            const char* p = std::strstr(s, k);
            if (!p) return false;
            p = std::strchr(p + std::strlen(k), '"');
            if (!p) return false;
            const char* q = std::strchr(p + 1, '"');
            if (!q) return false;
            out.assign(p + 1, q - p - 1);
            return true;
        };
        auto find_num = [&](const char* k, float& out) {
            const char* p = std::strstr(s, k);
            if (!p) return false;
            p = std::strchr(p + std::strlen(k), ':');
            if (!p) return false;
            out = std::strtof(p + 1, nullptr);
            return true;
        };
        std::string path, ain;
        float num = 0.0f;
        if (find_str("\"patch_path\"", path) && !path.empty()) {
            set_param(p, "patch_path", path.c_str());
        }
        if (find_str("\"audio_in_enable\"", ain)) {
            set_param(p, "audio_in_enable", ain.c_str());
        }
        if (find_num("\"gain\"", num)) {
            char b[32]; std::snprintf(b, sizeof(b), "%g", num);
            set_param(p, "gain", b);
        }
        if (find_num("\"octave_transpose\"", num)) {
            char b[32]; std::snprintf(b, sizeof(b), "%d", static_cast<int>(num));
            set_param(p, "octave_transpose", b);
        }
        return;
    }
}

int get_param(void* p, const char* key, char* buf, int buf_len) {
    auto* inst = static_cast<Instance*>(p);
    if (!inst || !key || !buf || buf_len <= 0) return 0;

    if (std::strcmp(key, "screen_ops") == 0) {
        return inst->screen_ring.drain_json(buf, buf_len);
    }
    if (std::strcmp(key, "chain_params") == 0) {
        static const char* params_json =
            "["
            "{\"key\":\"patch_path\",\"name\":\"Patch\",\"type\":\"enum\",\"options_param\":\"patch_list\"},"
            "{\"key\":\"octave_transpose\",\"name\":\"Octave\",\"type\":\"int\",\"min\":-4,\"max\":4,\"default\":0},"
            "{\"key\":\"audio_in_enable\",\"name\":\"Line In to Pd\",\"type\":\"enum\",\"options\":[\"Off\",\"On\"],\"default\":\"On\"},"
            "{\"key\":\"gain\",\"name\":\"Output Gain\",\"type\":\"float\",\"min\":0,\"max\":1.5,\"step\":0.01,\"default\":1.0},"
            "{\"key\":\"fs_source\",\"name\":\"Foot Switch\",\"type\":\"enum\",\"options\":[\"Knob8 Touch\",\"Off\"],\"default\":\"Knob8 Touch\"}"
            "]";
        const int n = static_cast<int>(std::strlen(params_json));
        const int to_copy = n < buf_len - 1 ? n : buf_len - 1;
        std::memcpy(buf, params_json, to_copy);
        buf[to_copy] = 0;
        return to_copy;
    }
    if (std::strcmp(key, "state") == 0) {
        return std::snprintf(buf, buf_len,
            "{\"patch_path\":\"%s\",\"octave_transpose\":%d,\"audio_in_enable\":\"%s\",\"gain\":%.3f}",
            inst->current_patch_path.c_str(),
            inst->octave_transpose,
            inst->audio_in_enable ? "On" : "Off",
            inst->gain);
    }
    if (std::strcmp(key, "patch_path") == 0) {
        const int n = static_cast<int>(inst->current_patch_path.size());
        const int to_copy = n < buf_len - 1 ? n : buf_len - 1;
        std::memcpy(buf, inst->current_patch_path.c_str(), to_copy);
        buf[to_copy] = 0;
        return to_copy;
    }
    if (std::strcmp(key, "patch_list") == 0) {
        const int n = organelle::list_patches_json(
            "/data/UserData/schwung/organelle-patches", buf, buf_len);
        if (g_host && g_host->log) {
            char line[256];
            std::snprintf(line, sizeof(line),
                "[organelle] patch_list: %d bytes, buf_len=%d, head=%.80s",
                n, buf_len, buf);
            g_host->log(line);
        }
        return n;
    }
    if (std::strcmp(key, "midi_out_queue") == 0) {
        // Drain MIDI-out ring into a compact base64-ish hex string:
        // "AABBCC,AABBCC,..." — 3 bytes per message, comma-separated.
        int w = 0;
        bool first = true;
        while (true) {
            const uint32_t tail = inst->midi_out_tail.load(std::memory_order_relaxed);
            const uint32_t head = inst->midi_out_head.load(std::memory_order_acquire);
            if (tail == head) break;
            if (w + (first ? 6 : 7) >= buf_len - 1) break;
            if (!first) buf[w++] = ',';
            const uint8_t* m = inst->midi_out_buf[tail];
            static const char H[] = "0123456789ABCDEF";
            buf[w++] = H[(m[0] >> 4) & 0xF]; buf[w++] = H[m[0] & 0xF];
            buf[w++] = H[(m[1] >> 4) & 0xF]; buf[w++] = H[m[1] & 0xF];
            buf[w++] = H[(m[2] >> 4) & 0xF]; buf[w++] = H[m[2] & 0xF];
            first = false;
            inst->midi_out_tail.store((tail + 1) % Instance::MIDI_OUT_RING, std::memory_order_release);
        }
        buf[w] = 0;
        return w;
    }

    return 0;
}

int get_error(void* /*p*/, char* /*buf*/, int /*buf_len*/) { return 0; }

void render_block(void* p, int16_t* out_lr, int frames) {
    auto* inst = static_cast<Instance*>(p);
    if (!inst || !inst->pd || !out_lr) {
        if (out_lr) std::memset(out_lr, 0, sizeof(int16_t) * 2 * frames);
        return;
    }
    libpd_set_instance(inst->pd);

    // ----- Tempo update (cheap; sent only if changed) -----
    if (g_host && g_host->get_bpm) {
        const float bpm = g_host->get_bpm();
        if (bpm != inst->last_tempo_sent) {
            libpd_float("clock", bpm);
            inst->last_tempo_sent = bpm;
        }
    }

    // ----- Audio in (libpd takes interleaved stereo, same layout as Move's mailbox) -----
    const int ticks = frames / 64;
    const float inv32k = 1.0f / 32768.0f;
    const int total = frames * 2;

    if (inst->audio_in_enable && g_host && g_host->mapped_memory) {
        const int16_t* in_lr = reinterpret_cast<const int16_t*>(
            g_host->mapped_memory + g_host->audio_in_offset);
        for (int i = 0; i < total; ++i) {
            inst->in_buf[i] = static_cast<float>(in_lr[i]) * inv32k;
        }
    } else {
        std::memset(inst->in_buf, 0, sizeof(float) * total);
    }

    // ----- Render -----
    if (inst->current_patch) {
        libpd_process_float(ticks, inst->in_buf, inst->out_buf);
    } else {
        std::memset(inst->out_buf, 0, sizeof(float) * total);
    }

    // ----- Output (interleaved float → interleaved int16, gain) -----
    const float g = inst->gain;
    for (int i = 0; i < total; ++i) {
        float s = inst->out_buf[i] * g;
        if (s >  1.0f) s =  1.0f;
        if (s < -1.0f) s = -1.0f;
        out_lr[i] = static_cast<int16_t>(s * 32767.0f);
    }
}

} // anonymous namespace

extern "C" plugin_api_v2_t* move_plugin_init_v2(const host_api_v1_t* host) {
    g_host = host;
    static plugin_api_v2_t api = {};
    api.api_version      = MOVE_PLUGIN_API_VERSION_2;
    api.create_instance  = create_instance;
    api.destroy_instance = destroy_instance;
    api.on_midi          = on_midi;
    api.set_param        = set_param;
    api.get_param        = get_param;
    api.get_error        = get_error;
    api.render_block     = render_block;
    return &api;
}
