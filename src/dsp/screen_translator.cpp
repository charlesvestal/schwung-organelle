#include "screen_translator.h"

#include "z_libpd.h"

#include <cstdio>
#include <cstring>

namespace organelle {

ScreenOpRing::ScreenOpRing() : head_(0), tail_(0) {
    std::memset(buf_, 0, sizeof(buf_));
}

void ScreenOpRing::push(const ScreenOp& op) {
    const uint32_t head = head_.load(std::memory_order_relaxed);
    const uint32_t next = (head + 1) % RING_OPS;
    const uint32_t tail = tail_.load(std::memory_order_acquire);
    if (next == tail) {
        // Full — drop oldest by advancing tail.
        tail_.store((tail + 1) % RING_OPS, std::memory_order_release);
    }
    buf_[head] = op;
    head_.store(next, std::memory_order_release);
}

static int json_escape(const char* in, char* out, int cap) {
    int o = 0;
    for (int i = 0; in[i] && o < cap - 2; ++i) {
        char c = in[i];
        if (c == '"' || c == '\\') {
            if (o < cap - 3) { out[o++] = '\\'; out[o++] = c; }
        } else if (c >= 0x20 && c < 0x7F) {
            out[o++] = c;
        }
        // skip non-printable to keep JSON safe
    }
    out[o] = 0;
    return o;
}

int ScreenOpRing::drain_json(char* out, int cap) {
    if (!out || cap < 4) return 0;
    int w = 0;
    out[w++] = '[';
    bool first = true;
    while (true) {
        const uint32_t tail = tail_.load(std::memory_order_relaxed);
        const uint32_t head = head_.load(std::memory_order_acquire);
        if (tail == head) break;
        const ScreenOp& op = buf_[tail];

        char entry[160];
        int n = 0;
        switch (op.kind) {
            case OP_CLEAR:
                n = std::snprintf(entry, sizeof(entry), "{\"op\":\"clear\"}");
                break;
            case OP_FILL:
                n = std::snprintf(entry, sizeof(entry),
                    "{\"op\":\"fill\",\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"c\":%d}",
                    op.a, op.b, op.c, op.d, op.e);
                break;
            case OP_LINE:
                n = std::snprintf(entry, sizeof(entry),
                    "{\"op\":\"line\",\"x1\":%d,\"y1\":%d,\"x2\":%d,\"y2\":%d,\"c\":%d}",
                    op.a, op.b, op.c, op.d, op.e);
                break;
            case OP_BOX:
                n = std::snprintf(entry, sizeof(entry),
                    "{\"op\":\"box\",\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"c\":%d}",
                    op.a, op.b, op.c, op.d, op.e);
                break;
            case OP_INVERT:
                n = std::snprintf(entry, sizeof(entry),
                    "{\"op\":\"invert\",\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d}",
                    op.a, op.b, op.c, op.d);
                break;
            case OP_PIXEL:
                n = std::snprintf(entry, sizeof(entry),
                    "{\"op\":\"pixel\",\"x\":%d,\"y\":%d,\"c\":%d}",
                    op.a, op.b, op.c);
                break;
            case OP_PRINT: {
                char esc[OP_TEXT_MAX * 2 + 1];
                json_escape(op.text, esc, sizeof(esc));
                n = std::snprintf(entry, sizeof(entry),
                    "{\"op\":\"print\",\"x\":%d,\"y\":%d,\"size\":%d,\"c\":%d,\"text\":\"%s\"}",
                    op.a, op.b, op.c, op.d, esc);
                break;
            }
            case OP_FLIP:
                n = std::snprintf(entry, sizeof(entry), "{\"op\":\"flip\"}");
                break;
            default:
                n = 0;
        }

        if (n <= 0) {
            tail_.store((tail + 1) % RING_OPS, std::memory_order_release);
            continue;
        }
        const int needed = (first ? 0 : 1) + n + 1;  // [,] + entry + ]
        if (w + needed >= cap) break;  // leave the rest for the next drain

        if (!first) out[w++] = ',';
        std::memcpy(out + w, entry, n);
        w += n;
        first = false;
        tail_.store((tail + 1) % RING_OPS, std::memory_order_release);
    }
    out[w++] = ']';
    out[w] = 0;
    return w;
}

// --- Pd message → ScreenOp ---

static int atomi(t_atom* a) {
    return libpd_is_float(a) ? static_cast<int>(libpd_get_float(a)) : 0;
}

void handle_oled_message(const char* selector, int argc, t_atom* argv, ScreenOpRing& ring) {
    if (!selector) return;
    ScreenOp op{};

    if (std::strcmp(selector, "gFillArea") == 0 && argc >= 5) {
        op.kind = OP_FILL;
        op.a = atomi(argv + 0); op.b = atomi(argv + 1);
        op.c = atomi(argv + 2); op.d = atomi(argv + 3);
        op.e = atomi(argv + 4);
        ring.push(op);
    } else if (std::strcmp(selector, "gLine") == 0 && argc >= 5) {
        op.kind = OP_LINE;
        op.a = atomi(argv + 0); op.b = atomi(argv + 1);
        op.c = atomi(argv + 2); op.d = atomi(argv + 3);
        op.e = atomi(argv + 4);
        ring.push(op);
    } else if (std::strcmp(selector, "gBox") == 0 && argc >= 5) {
        op.kind = OP_BOX;
        op.a = atomi(argv + 0); op.b = atomi(argv + 1);
        op.c = atomi(argv + 2); op.d = atomi(argv + 3);
        op.e = atomi(argv + 4);
        ring.push(op);
    } else if (std::strcmp(selector, "gInvertArea") == 0 && argc >= 4) {
        op.kind = OP_INVERT;
        op.a = atomi(argv + 0); op.b = atomi(argv + 1);
        op.c = atomi(argv + 2); op.d = atomi(argv + 3);
        ring.push(op);
    } else if (std::strcmp(selector, "gSetPixel") == 0 && argc >= 3) {
        op.kind = OP_PIXEL;
        op.a = atomi(argv + 0); op.b = atomi(argv + 1);
        op.c = atomi(argv + 2);
        ring.push(op);
    } else if ((std::strcmp(selector, "gPrintln") == 0 || std::strcmp(selector, "gPrint") == 0) && argc >= 5) {
        op.kind = OP_PRINT;
        op.a = atomi(argv + 0); op.b = atomi(argv + 1);
        op.c = atomi(argv + 2); op.d = atomi(argv + 3);
        // Concatenate remaining atoms (symbols/floats) as text.
        int o = 0;
        for (int i = 4; i < argc && o < OP_TEXT_MAX - 2; ++i) {
            if (i > 4 && o < OP_TEXT_MAX - 2) op.text[o++] = ' ';
            if (libpd_is_symbol(argv + i)) {
                const char* s = libpd_get_symbol(argv + i);
                while (s && *s && o < OP_TEXT_MAX - 1) op.text[o++] = *s++;
            } else if (libpd_is_float(argv + i)) {
                char tmp[16];
                int tn = std::snprintf(tmp, sizeof(tmp), "%g", libpd_get_float(argv + i));
                for (int k = 0; k < tn && o < OP_TEXT_MAX - 1; ++k) op.text[o++] = tmp[k];
            }
        }
        op.text[o] = 0;
        ring.push(op);
    } else if (std::strcmp(selector, "gFlip") == 0) {
        op.kind = OP_FLIP;
        ring.push(op);
    } else if (std::strcmp(selector, "gClear") == 0) {
        op.kind = OP_CLEAR;
        ring.push(op);
    }
    // Unknown selectors are silently ignored.
}

} // namespace organelle
