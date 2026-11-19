/* fuzz_trunk - src/io/k3_trunk.c: open a packed trunk (trunk.json manifest +
 * trunk.bin runs) against a fixed small config, then fetch, bind and prefetch every
 * layer through the slot ring and its asynchronous reader.
 *
 * Input: u32 LE manifest length | trunk.json | trunk.bin.
 * The config is the synthetic 3-layer one tests/unit/test_trunk.c builds its trunk
 * fixture against (layer 0 dense, layer 2 MLA).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "k3_fuzz_util.h"
#include "k3.h"
#include "k3_bind.h"
#include "k3_trunk.h"

static K3Cfg cfg;
static int fa[1] = {3};

static void cfg_init(void)
{
    memset(&cfg, 0, sizeof cfg);
    cfg.hidden = 16; cfg.n_layers = 3; cfg.vocab = 1000; cfg.rms_eps = 1e-5f;
    cfg.kda_heads = 2; cfg.kda_head_dim = 4; cfg.conv_k = 2; cfg.gate_lb = -5.0f;
    cfg.n_heads = 2; cfg.q_lora = 4; cfg.kv_lora = 4; cfg.qk_nope = 4; cfg.qk_rope = 2;
    cfg.v_head = 4; cfg.mla_out_gate = 0; cfg.n_experts = 1; cfg.topk = 1; cfg.n_shared = 1;
    cfg.latent = 4; cfg.moe_inter = 8; cfg.routed_scale = 1.0f; cfg.moe_renorm = 1;
    cfg.latent_norm = 1; cfg.first_dense = 1; cfg.dense_inter = 16; cfg.attn_res_block = 1;
    cfg.situ_b1 = 4.0f; cfg.situ_b2 = 25.0f;
    cfg.n_full_attn = 1; cfg.full_attn = fa;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (!cfg.hidden) cfg_init();
    const uint8_t *js, *bin; size_t jn, bn;
    fz_split(data, size, &js, &jn, &bin, &bn);
    fz_write("trunk.json", js, jn);
    fz_write("trunk.bin", bin, bn);

    /* Two budgets: 12 KiB (one slot, synchronous reads) and 64 KiB (a two-slot ring
     * with the reader thread), the two regimes test_trunk.c distinguishes. */
    static const int64_t budgets[2] = {12288, 65536};
    int opened = 0;
    for (int bi = 0; bi < 2; bi++) {
        K3Trunk tr;
        memset(&tr, 0, sizeof tr);
        if (k3_trunk_open(&tr, fz_dir(), &cfg, budgets[bi], 0) != 0) continue;
        opened = 1;
        for (int L = 0; L < cfg.n_layers; L++) {
            unsigned char *run = NULL;
            (void)k3_trunk_fetch(&tr, L, &run);
            k3_trunk_prefetch(&tr, (L + 1) % cfg.n_layers);
            K3LayerBind b;
            memset(&b, 0, sizeof b);
            (void)k3_trunk_bind(&tr, &cfg, L, &b);
        }
        K3LayerBind b;
        memset(&b, 0, sizeof b);
        (void)k3_trunk_bind(&tr, &cfg, 0, &b);
        k3_trunk_close(&tr);
    }
    return opened ? 0 : -1;
}
