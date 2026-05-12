// Organelle module — libpd host (Plugin API v2).
// Skeleton; implementation tracked in docs/plans/2026-05-12-organelle-port-plan.md.

#include "plugin_api_v2.h"

extern "C" plugin_api_v2_t* move_plugin_init_v2(const host_api_v1_t* /*host*/) {
    static plugin_api_v2_t api = {};
    api.api_version = 2;
    // TODO: wire create_instance / destroy_instance / on_midi / set_param /
    // get_param / render_block once libpd is integrated (Task 1+).
    return &api;
}
