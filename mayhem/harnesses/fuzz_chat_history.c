/* fuzz_chat_history - src/chat/k3_chat.c: a `k3 --chat --history F` transcript.
 * The JSONL reader (role/content/reasoning_content per line), history validation,
 * XTML rendering with and without the think channel, segment text, encoding with a
 * fixed vocabulary, and a save/reload round trip.
 *
 * The vocabulary is tests/fixtures/tok/good compiled in (k3_embed_tok.h), loaded once
 * and read-only after. The whole input is the history file.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "k3_fuzz_util.h"
#include "k3_chat.h"
#include "k3_embed_tok.h"

static Tok T;
static int loaded;

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (!loaded) {
        fz_write("tiktoken.model", k3e_good_tiktoken, sizeof k3e_good_tiktoken);
        fz_write("tokenizer_config.json", k3e_good_tokcfg, sizeof k3e_good_tokcfg);
        k3_tok_load(&T, fz_dir());
        loaded = 1;
    }
    char path[2048], saved[2048], err[512];
    fz_write("history.jsonl", data, size);
    fz_path(path, sizeof path, "history.jsonl");
    fz_path(saved, sizeof saved, "saved.jsonl");

    K3ChatHistory h;
    k3_chat_history_init(&h);
    if (k3_chat_history_load(&h, path, err, sizeof err) != 0) {
        k3_chat_history_free(&h);
        return -1;
    }
    (void)k3_chat_history_validate(&h, err, sizeof err);

    for (int mode = 0; mode < 3; mode++) {
        K3ChatOptions o = k3_chat_options_default();
        if (mode == 1) o.thinking = 0;
        if (mode == 2) o.thinking_effort = "low";
        K3ChatSegments segs;
        k3_chat_segments_init(&segs);
        if (k3_chat_render_opts(&h, mode != 1, &o, &segs, err, sizeof err) == 0) {
            char *text = NULL; int len = 0;
            if (k3_chat_segments_text(&segs, &text, &len, err, sizeof err) == 0) {
                int max = len + 64;
                int *ids = (int *)malloc((size_t)max * sizeof(int));
                if (ids) { (void)k3_chat_encode(&T, &segs, ids, max, err, sizeof err); free(ids); }
            }
            free(text);
        }
        k3_chat_segments_free(&segs);
    }

    if (k3_chat_history_save(&h, saved, err, sizeof err) == 0) {
        K3ChatHistory h2;
        k3_chat_history_init(&h2);
        if (k3_chat_history_load(&h2, saved, err, sizeof err) == 0 && h2.n != h.n) {
            fprintf(stderr, "history save/load round trip changed %d turns into %d\n", h.n, h2.n);
            abort();
        }
        k3_chat_history_free(&h2);
    }
    fz_rm("saved.jsonl");
    k3_chat_history_free(&h);
    return 0;
}
