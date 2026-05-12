#pragma once

namespace organelle {

// Open <patch_dir>/main.pd via libpd. Caller is responsible for
// libpd_set_instance() before calling. Returns the libpd patch handle
// (opaque void*) or nullptr on failure.
void* load_patch(const char* patch_dir);

// Close a previously loaded patch.
void close_patch(void* handle);

// Enumerate patch folders containing main.pd under <root>, sorted.
// Writes a JSON array of objects to buf and returns bytes written:
//   [{"name":"Basic-Synth","path":"/data/.../Basic-Synth"},...]
// Truncates cleanly if buf_len is exceeded.
int list_patches_json(const char* root, char* buf, int buf_len);

} // namespace organelle
