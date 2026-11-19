/* fuzz_state - src/cli/k3_state.c: a `k3 --load-state F` saved-state file. Header
 * peek (magic, version, count sanity), then the full load against a fixed config:
 * fingerprint, layer counts, geometry, KV capacity, payload reads and hash check.
 *
 * The config and buffer geometry are tests/unit/test_state.c's: 4 bound layers,
 * 2 MLA layers, a 16-position KV cache. The whole input is the state file. A file
 * the loader accepts is then saved again and reloaded (round_trip), which covers the
 * write path, k3_state_save.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "k3_fuzz_util.h"
#include "k3_state.h"

enum { N_BOUND = 4, N_MLA = 2, KV_CAP = 16 };

/* A state the loader accepted is what `k3 --load-state A --save-state B` would write
 * back: save it the way k3_run.c does (k3_state_save with the loaded counts), then
 * peek and load the saved file and require the same arrays. k3_state.h documents the
 * pair as inverses (a resumed run continues the sequence it saved), so a mismatch or
 * a refusal of a file k3_state_save just wrote is a bug, reported with abort(). */
static void round_trip(const K3Cfg *c, const K3StateHdr *hd, const int *seq,
                       const float *ks, const float *kvc, const float *ropec,
                       int64_t kper, int64_t kvpp, int64_t ropepp)
{
    char path[2048];
    fz_path(path, sizeof path, "state2.k3s");
    if (k3_state_save(path, c, seq, hd->nseq, ks, kvc, ropec, N_BOUND, N_MLA, KV_CAP,
                      hd->cached, kper, kvpp, ropepp) != 0) {
        fprintf(stderr, "k3_state_save failed on a state k3_state_load accepted\n");
        abort();
    }
    const size_t nks = (size_t)(kper * N_BOUND);
    const size_t nkv = (size_t)(N_MLA * KV_CAP * kvpp);
    const size_t nro = (size_t)(N_MLA * KV_CAP * ropepp);
    K3StateHdr h2;
    int *seq2 = (int *)malloc(((size_t)hd->nseq + 1) * sizeof(int));
    float *ks2 = (float *)calloc(nks, sizeof(float));
    float *kvc2 = (float *)calloc(nkv, sizeof(float));
    float *ropec2 = (float *)calloc(nro, sizeof(float));
    if (!seq2 || !ks2 || !kvc2 || !ropec2) abort();
    if (k3_state_peek(path, &h2) != 0 ||
        k3_state_load(path, c, &h2, seq2, ks2, kvc2, ropec2, N_BOUND, N_MLA, KV_CAP) != 0) {
        fprintf(stderr, "k3_state_load refused the file k3_state_save wrote\n");
        abort();
    }
    /* Only the cached positions of each MLA slice are saved; compare those. */
    int ok = h2.nseq == hd->nseq && h2.cached == hd->cached &&
             memcmp(seq, seq2, (size_t)hd->nseq * sizeof(int)) == 0 &&
             memcmp(ks, ks2, nks * sizeof(float)) == 0;
    for (int mi = 0; ok && mi < N_MLA; mi++) {
        const size_t okv = (size_t)mi * KV_CAP * kvpp, oro = (size_t)mi * KV_CAP * ropepp;
        ok = memcmp(kvc + okv, kvc2 + okv, (size_t)hd->cached * kvpp * sizeof(float)) == 0 &&
             memcmp(ropec + oro, ropec2 + oro, (size_t)hd->cached * ropepp * sizeof(float)) == 0;
    }
    if (!ok) { fprintf(stderr, "saved state does not load back to the same arrays\n"); abort(); }
    free(seq2); free(ks2); free(kvc2); free(ropec2);
    fz_rm("state2.k3s");
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    K3Cfg c;
    memset(&c, 0, sizeof c);
    c.hidden = 64; c.n_layers = 4; c.vocab = 256;
    c.kda_heads = 2; c.kda_head_dim = 8; c.conv_k = 4;
    c.n_heads = 2; c.qk_nope = 8; c.qk_rope = 4; c.v_head = 8;
    c.n_experts = 4; c.topk = 2;

    char path[2048];
    fz_write("state.k3s", data, size);
    fz_path(path, sizeof path, "state.k3s");

    K3StateHdr hd;
    if (k3_state_peek(path, &hd) != 0) return -1;

    /* The CLI sizes seq from the header (prior = hd.nseq, plus the new prompt and
     * generation) BEFORE k3_state_load runs, as k3_state.h documents; size it the same
     * way, from the header k3_state_peek accepted. A count no 1 GiB buffer holds is a
     * resource limit, not a parse. */
    if (hd.nseq > (1 << 24)) return -1;
    int64_t kper, kvpp, ropepp;
    k3_state_geom(&c, &kper, &kvpp, &ropepp);
    int *seq = (int *)malloc(((size_t)hd.nseq + 1) * sizeof(int));
    float *ks = (float *)calloc((size_t)(kper * N_BOUND), sizeof(float));
    float *kvc = (float *)calloc((size_t)(N_MLA * KV_CAP * kvpp), sizeof(float));
    float *ropec = (float *)calloc((size_t)(N_MLA * KV_CAP * ropepp), sizeof(float));
    if (seq && ks && kvc && ropec &&
        k3_state_load(path, &c, &hd, seq, ks, kvc, ropec, N_BOUND, N_MLA, KV_CAP) == 0)
        round_trip(&c, &hd, seq, ks, kvc, ropec, kper, kvpp, ropepp);
    free(seq); free(ks); free(kvc); free(ropec);
    return 0;
}
