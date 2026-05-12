// Mirror of Schwung's host/plugin_api_v2.h — vendor copy so this repo
// builds without the schwung source tree present. Keep in sync.
#pragma once

#include <stdint.h>

typedef struct host_api_v1 {
    uint32_t api_version;
    // (host callbacks not used yet)
} host_api_v1_t;

typedef struct plugin_api_v2 {
    uint32_t api_version;
    void* (*create_instance)(const char* module_dir, const char* json_defaults);
    void  (*destroy_instance)(void* instance);
    void  (*on_midi)(void* instance, const uint8_t* msg, int len, int source);
    void  (*set_param)(void* instance, const char* key, const char* val);
    int   (*get_param)(void* instance, const char* key, char* buf, int buf_len);
    void  (*render_block)(void* instance, int16_t* out_lr, int frames);
} plugin_api_v2_t;
