/* fuzz_cli - src/cli/k3_run.c main(): the `k3` command line. Option parsing and its
 * numeric contracts (--ids lists, --layers, --stop-id, --gen, --spec, --threads,
 * --top-k, --seed, temperatures, presets, --out/--save-state paths), then the loader
 * steps the options drive (tokenizer, config, shard directory, history).
 *
 * k3_run.c is compiled with -Dmain=k3_cli_main and linked with this file, which is a
 * file-input (AFL) target because the CLI exit()s on refusals: one process per input.
 *
 * The input is the argument list, one argument per line (or NUL-separated), appended
 * after a fixed prefix that points the CLI at the repository's small fixtures, which
 * are compiled in (k3_embed_cli.h) and written into a scratch directory the process
 * runs in:  k3 st --config flat.json <input args...>
 *   st/        tests/fixtures/st's two shards
 *   flat.json  tests/fixtures/cfg/flat.json
 *   tok/       tests/fixtures/tok/good (for --tok tok)
 *
 * Every path-valued option (see PATH_OPTS) is confined to that scratch directory:
 * its value loses any leading '/' and every '.'/'..' component, so it names a path
 * relative to the cwd, which is the scratch directory. The option parsing and the
 * load/save code still run on every input; only where they read and write moves, so
 * an input cannot write outside the scratch directory (which is removed at exit) and
 * cannot read back a file an earlier input left behind.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "k3_fuzz_util.h"
#include "k3_embed_tok.h"
#include "k3_embed_cli.h"

int k3_cli_main(int argc, char **argv);

#define MAX_ARGS 48

/* k3_run.c's options whose value is a file or directory path (read or written). */
static const char *const PATH_OPTS[] = {
    "--prompt-file", "--tok", "--config", "--out", "--trunk", "--load-state",
    "--save-state", "--draft-trunk", "--history", "--dump-logits", "--dump-cache-trace",
};

static int is_path_opt(const char *a)
{
    for (size_t i = 0; i < sizeof PATH_OPTS / sizeof *PATH_OPTS; i++)
        if (!strcmp(a, PATH_OPTS[i])) return 1;
    return 0;
}

/* Rewrite a path in place to a relative one with no '.', '..' or empty components.
 * The result is never longer than the input. A path left with no components becomes
 * "." (the scratch directory itself). */
static void confine_path(char *p)
{
    if (!*p) return;
    char *w = p;
    const char *r = p;
    while (*r) {
        while (*r == '/') r++;
        const char *c = r;
        while (*r && *r != '/') r++;
        size_t len = (size_t)(r - c);
        if (len == 0 || (len == 1 && c[0] == '.') || (len == 2 && c[0] == '.' && c[1] == '.'))
            continue;
        if (w != p) *w++ = '/';
        memmove(w, c, len);
        w += len;
    }
    if (w == p) *w++ = '.';
    *w = 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <input>\n", argv[0]); return 2; }
    size_t n;
    uint8_t *d = fz_slurp(argv[1], &n);

    char p[2048];
    fz_path(p, sizeof p, "st"); mkdir(p, 0700);
    fz_path(p, sizeof p, "tok"); mkdir(p, 0700);
    fz_write("st/model-00001-of-00002.safetensors", k3e_st1, sizeof k3e_st1);
    fz_write("st/model-00002-of-00002.safetensors", k3e_st2, sizeof k3e_st2);
    fz_write("flat.json", k3e_flat_cfg, sizeof k3e_flat_cfg);
    fz_write("tok/tiktoken.model", k3e_good_tiktoken, sizeof k3e_good_tiktoken);
    fz_write("tok/tokenizer_config.json", k3e_good_tokcfg, sizeof k3e_good_tokcfg);
    if (chdir(fz_dir()) != 0) { perror("chdir"); abort(); }

    char *buf = (char *)malloc(n + 1);
    memcpy(buf, d, n);
    buf[n] = 0;
    char *av[MAX_ARGS + 5];
    int ac = 0;
    av[ac++] = (char *)"k3";
    av[ac++] = (char *)"st";
    av[ac++] = (char *)"--config";
    av[ac++] = (char *)"flat.json";
    char *s = buf, *end = buf + n;
    while (s < end && ac < MAX_ARGS + 4) {
        char *e = s;
        while (e < end && *e != '\n' && *e != '\0') e++;
        *e = 0;
        av[ac++] = s;
        s = e + 1;
    }
    av[ac] = NULL;
    /* Confine the argument after EVERY occurrence of a path option, whether or not the
     * CLI's parser ends up reading that occurrence as an option (a superset of what it
     * opens), so no misalignment between this walk and k3_run.c's can leak a path. */
    for (int i = 4; i + 1 < ac; i++)
        if (is_path_opt(av[i])) confine_path(av[i + 1]);
    int rc = k3_cli_main(ac, av);
    free(buf);
    free(d);
    return rc;
}
