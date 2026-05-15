#pragma once

namespace organelle {

// Sandbox root on the device. install.sh pre-populates root/version etc.
constexpr const char* SANDBOX_ROOT = "/data/UserData/schwung/organelle-sandbox";

// Returns true if `in` matched a sandboxed prefix and `out` was filled with
// the redirected path. Returns false if the path should pass through.
bool sandbox_translate(const char* in, char* out, int out_cap);

} // namespace organelle
