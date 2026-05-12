#include "patch_loader.h"

#include "z_libpd.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace organelle {

void* load_patch(const char* patch_dir) {
    if (!patch_dir || !*patch_dir) return nullptr;
    return libpd_openfile("main.pd", patch_dir);
}

void close_patch(void* handle) {
    if (handle) libpd_closefile(handle);
}

static bool has_main_pd(const std::string& dir) {
    std::string path = dir + "/main.pd";
    struct stat st{};
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

int list_patches_json(const char* root, char* buf, int buf_len) {
    if (!root || !buf || buf_len < 4) return 0;
    DIR* d = opendir(root);
    if (!d) {
        std::snprintf(buf, buf_len, "[]");
        return 2;
    }

    std::vector<std::string> names;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        if (e->d_name[0] == '.') continue;
        std::string full = std::string(root) + "/" + e->d_name;
        struct stat st{};
        if (stat(full.c_str(), &st) != 0) continue;
        if (!S_ISDIR(st.st_mode)) continue;
        if (!has_main_pd(full)) continue;
        names.emplace_back(e->d_name);
    }
    closedir(d);
    std::sort(names.begin(), names.end());

    int w = 0;
    buf[w++] = '[';
    bool first = true;
    for (const auto& n : names) {
        const int extra = (first ? 0 : 1)
                        + 2  // {"
                        + 7  // name":"
                        + static_cast<int>(n.size())
                        + 11 // ","path":"
                        + static_cast<int>(std::strlen(root)) + 1 + static_cast<int>(n.size())
                        + 3; // "}]
        if (w + extra >= buf_len) break;
        if (!first) buf[w++] = ',';
        first = false;
        w += std::snprintf(buf + w, buf_len - w,
            "{\"name\":\"%s\",\"path\":\"%s/%s\"}",
            n.c_str(), root, n.c_str());
    }
    buf[w++] = ']';
    if (w < buf_len) buf[w] = 0;
    return w;
}

} // namespace organelle
