#pragma once

namespace organelle {

// Open <patch_dir>/main.pd via libpd. Caller is responsible for
// libpd_set_instance() before calling. Returns the libpd patch handle
// (opaque void*) or nullptr on failure.
void* load_patch(const char* patch_dir);

// Close a previously loaded patch.
void close_patch(void* handle);

} // namespace organelle
