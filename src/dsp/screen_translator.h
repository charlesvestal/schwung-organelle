#pragma once

#include <atomic>
#include <cstdint>

extern "C" {
struct _atom;
typedef struct _atom t_atom;
}

namespace organelle {

constexpr int RING_OPS    = 512;
constexpr int OP_TEXT_MAX = 48;

enum ScreenOpKind : uint8_t {
    OP_NONE = 0,
    OP_CLEAR,
    OP_FILL,
    OP_LINE,
    OP_BOX,
    OP_INVERT,
    OP_PIXEL,
    OP_PRINT,
    OP_FLIP,
};

struct ScreenOp {
    ScreenOpKind kind;
    int16_t      a, b, c, d, e;
    char         text[OP_TEXT_MAX];
};

class ScreenOpRing {
public:
    ScreenOpRing();

    // Realtime-safe writer: push from libpd message hook.
    // On overflow, drops the oldest op.
    void push(const ScreenOp& op);

    // Non-realtime drain into JSON. Returns bytes written (0 if empty).
    int drain_json(char* out, int cap);

private:
    ScreenOp              buf_[RING_OPS];
    std::atomic<uint32_t> head_;   // writer
    std::atomic<uint32_t> tail_;   // reader
};

// Parse a [s oled <selector> <args...>] message into a ScreenOp and push to ring.
// Called from libpd message hook on the realtime thread.
void handle_oled_message(const char* selector, int argc, t_atom* argv, ScreenOpRing& ring);

} // namespace organelle
