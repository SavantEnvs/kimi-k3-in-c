/* fuzz_tok_load - src/tokenizer/k3_tok.h k3_tok_load: the tokenizer files shipped in
 * a model directory. tiktoken.model's "<base64> <rank>" lines (base64 decoding, rank
 * parsing, duplicate/gap/collision checks), tokenizer_config.json's
 * added_tokens_decoder, then a short encode/decode with the loaded vocabulary.
 *
 * The loader exit(1)s on every malformed file, so this is a file-input (AFL) target:
 * one process per input.  Input: u32 LE length | tiktoken.model | tokenizer_config.json.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "k3_fuzz_util.h"
#include "k3_tok.h"

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <input>\n", argv[0]); return 2; }
    size_t n;
    uint8_t *d = fz_slurp(argv[1], &n);
    const uint8_t *a, *b; size_t an, bn;
    fz_split(d, n, &a, &an, &b, &bn);
    fz_write("tiktoken.model", a, an);
    fz_write("tokenizer_config.json", b, bn);

    static Tok T;
    k3_tok_load(&T, fz_dir());

    static const char probe[] = "Hello, world! 你好 <|im_end|> [BOS] 12345\n\tdon't";
    int ids[256];
    char back[2048];
    int m = tok_encode(&T, probe, (int)sizeof probe - 1, ids, 256);
    int o = tok_decode(&T, ids, m, back, (int)sizeof back - 1);
    printf("%d ids, %d bytes back\n", m, o);
    free(d);
    return 0;
}
