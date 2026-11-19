/* fuzz_cfg - include/k3/k3_cfg.h: config.json -> K3Cfg (field extraction, layer map,
 * full-attention list, range checks), then the geometry the engine sizes its buffers
 * from: k3_state_geom, the layer/MLA/KDA/MoE scratch and the bind widen area. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "k3.h"
#include "k3_cfg.h"
#include "k3_bind.h"
#include "k3_state.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    /* k3_cfg_load_file reads the file into a NUL-terminated buffer and parses it; this
     * is the same path without the fread. */
    char *text = (char *)malloc(size + 1);
    if (!text) return 0;
    memcpy(text, data, size);
    text[size] = 0;
    char *arena = NULL;
    jval *root = json_parse(text, &arena);
    free(arena);
    if (!root) { free(text); return -1; }

    K3Cfg c;
    memset(&c, 0, sizeof c);
    static int fa[512];
    int ok = k3_cfg_load(&c, fa, 512, root, "config.json");
    json_free_tree(root);
    free(text);
    if (!ok) return -1;

    int64_t kper, kvpp, ropepp;
    k3_state_geom(&c, &kper, &kvpp, &ropepp);
    int32_t fp[12];
    k3_state_fp(&c, fp);
    volatile size_t sink = 0;
    sink += k3_layer_scratch(&c, 1);
    sink += k3_layer_scratch(&c, 16);
    sink += k3_mla_scratch(&c, 16);
    sink += k3_mla_scratch_cached(&c, 1, 16, 1);
    sink += k3_kda_scratch(&c, 16);
    sink += k3_moe_scratch(&c);
    sink += k3_bind_widen_bytes(&c);
    (void)sink; (void)kper; (void)kvpp; (void)ropepp;
    return 0;
}
