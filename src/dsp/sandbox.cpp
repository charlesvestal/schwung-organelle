// Filesystem sandbox for libpd patches.
//
// Pd patches (via [textfile], [file], soundfiler, openpanel, etc.) can
// touch any path the schwung user can. To run Organelle stock patches
// faithfully — and to prevent a stray patch from poking at our real
// filesystem — we intercept Pd's sys_open / sys_fopen / sys_close
// (linker --wrap=) and remap a small set of Organelle-shaped prefixes
// onto a sandbox directory we control on /data/UserData.
//
//   /root/...     -> /data/UserData/schwung/organelle-sandbox/root/...
//   /sdcard/...   -> .../sandbox/sdcard/...
//   /usbdrive/... -> .../sandbox/usbdrive/...
//
// install.sh seeds the sandbox with /root/version so OS-version checks
// in patches like CZZ-Multi pass. Anything else (including paths inside
// /data/UserData itself) passes through unchanged.

#include "sandbox.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace organelle {

namespace {

struct Mapping {
    const char* prefix;     // path prefix to match (with trailing slash)
    int         prefix_len;
};

constexpr Mapping kMappings[] = {
    {"/root/",     6},
    {"/sdcard/",   8},
    {"/usbdrive/", 10},
};

} // anonymous

bool sandbox_translate(const char* in, char* out, int out_cap) {
    if (!in || !out || out_cap <= 0) return false;
    for (const auto& m : kMappings) {
        if (std::strncmp(in, m.prefix, m.prefix_len) == 0) {
            const int n = std::snprintf(out, out_cap, "%s%s", SANDBOX_ROOT, in);
            return n > 0 && n < out_cap;
        }
    }
    // Bare path "/root/version" or similar exact matches go through the
    // /root/ prefix above. Nothing else gets redirected.
    return false;
}

} // namespace organelle

// ---- ld --wrap interceptors -------------------------------------------------

extern "C" {

int  __real_sys_open(const char* path, int oflag, ...);
FILE* __real_sys_fopen(const char* filename, const char* mode);
int  __real_sys_close(int fd);

int __wrap_sys_open(const char* path, int oflag, ...) {
    char buf[1024];
    const char* p = path;
    if (organelle::sandbox_translate(path, buf, sizeof(buf))) p = buf;
    if (oflag & O_CREAT) {
        va_list ap;
        va_start(ap, oflag);
        int mode = va_arg(ap, int);
        va_end(ap);
        return __real_sys_open(p, oflag, mode);
    }
    return __real_sys_open(p, oflag);
}

FILE* __wrap_sys_fopen(const char* filename, const char* mode) {
    char buf[1024];
    const char* p = filename;
    if (organelle::sandbox_translate(filename, buf, sizeof(buf))) p = buf;
    return __real_sys_fopen(p, mode);
}

int __wrap_sys_close(int fd) {
    return __real_sys_close(fd);
}

} // extern "C"
