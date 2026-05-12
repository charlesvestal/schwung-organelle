#include "patch_loader.h"

#include "z_libpd.h"

#include <cstring>
#include <string>

namespace organelle {

void* load_patch(const char* patch_dir) {
    if (!patch_dir || !*patch_dir) return nullptr;
    return libpd_openfile("main.pd", patch_dir);
}

void close_patch(void* handle) {
    if (handle) libpd_closefile(handle);
}

} // namespace organelle
