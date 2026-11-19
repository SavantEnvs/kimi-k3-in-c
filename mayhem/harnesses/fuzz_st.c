/* fuzz_st - src/io/k3_st.c + k3_load.c: open a model directory holding one fuzzed
 * safetensors shard (header length, header JSON, tensor table, dtype/shape/offset
 * validation) and an optional model.safetensors.index.json, then read every tensor,
 * widen it to f32, resolve/load/scan a routed expert (mxfp4), and triage misses
 * against the index.
 *
 * Input: u32 LE index length | index JSON (length 0 = no index file) | shard bytes.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "k3_fuzz_util.h"
#include "k3_st.h"
#include "k3_load.h"

#define SHARD "model-00001-of-00002.safetensors"
#define INDEX "model.safetensors.index.json"
#define MAX_READ (8 << 20)

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    const uint8_t *ix, *sh; size_t ixn, shn;
    fz_split(data, size, &ix, &ixn, &sh, &shn);
    fz_write(SHARD, sh, shn);
    if (ixn) fz_write(INDEX, ix, ixn); else fz_rm(INDEX);

    K3St s;
    memset(&s, 0, sizeof s);
    if (k3_st_open(&s, fz_dir()) != 0) return -1;

    for (int i = 0; i < s.nt; i++) {
        const K3Tensor *t = &s.t[i];
        if (k3_st_find(&s, t->name) != t) {
            fprintf(stderr, "k3_st_find disagrees with the tensor table for %s\n", t->name);
            abort();
        }
        if (t->nbytes > 0 && t->nbytes <= MAX_READ) {
            void *buf = malloc((size_t)t->nbytes);
            if (buf) { (void)k3_st_read(&s, t, buf); free(buf); }
        }
        int64_t n = k3_st_numel(t);
        if (n > 0 && n <= MAX_READ / 4) {
            float *f = (float *)malloc((size_t)n * sizeof(float));
            if (f) { (void)k3_st_read_f32(&s, t, f); free(f); }
        }
    }

    for (int L = 0; L < 2; L++) {
        K3ExpertRef r;
        memset(&r, 0, sizeof r);
        if (k3_expert_ref(&s, L, 0, &r) != 0) continue;
        (void)k3_expert_check_shape(&r, 64, 32);
        if (r.nbytes <= 0 || r.nbytes > MAX_READ) continue;
        unsigned char *buf = (unsigned char *)malloc((size_t)r.nbytes);
        if (!buf) continue;
        if (k3_expert_load(&s, &r, buf) == r.nbytes) {
            char msg[256];
            (void)k3_expert_nan_scales(&s, &r, buf, msg, sizeof msg);
            int64_t n1 = k3_qmat_numel(&r.m[0]), n2 = k3_qmat_numel(&r.m[1]),
                    n3 = k3_qmat_numel(&r.m[2]);
            if (n1 > 0 && n2 > 0 && n3 > 0 && n1 + n2 + n3 <= MAX_READ / 4) {
                float *w1 = (float *)malloc((size_t)n1 * 4), *w2 = (float *)malloc((size_t)n2 * 4),
                      *w3 = (float *)malloc((size_t)n3 * 4);
                if (w1 && w2 && w3) k3_expert_dequant(&r, buf, w1, w2, w3);
                free(w1); free(w2); free(w3);
            }
        }
        free(buf);
    }

    char why[512];
    (void)k3_st_explain_missing(&s, "language_model.model.layers.7.self_attn.q_proj.weight", why, sizeof why);
    (void)k3_st_explain_missing(&s, "lm_head.weight", why, sizeof why);
    if (s.nt > 0) (void)k3_st_explain_missing(&s, s.t[0].name, why, sizeof why);
    k3_st_close(&s);
    return 0;
}
