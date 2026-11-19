/* k3_fuzz_util.h - shared scratch-directory plumbing for the kimi-k3-in-c harnesses.
 *
 * The engine's loaders take a directory or a path, never a buffer, so each harness
 * stages the fuzz input as the files the loader expects inside one private scratch
 * directory under $TMPDIR (fallback /tmp). Every input rewrites (or removes) every
 * file it uses, so no state carries from one input to the next; the directory and its
 * files are removed at exit.
 */
#ifndef K3_FUZZ_UTIL_H
#define K3_FUZZ_UTIL_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ftw.h>
#include <sys/stat.h>

static char fz_dirbuf[1024];

static int fz_rm_cb(const char *p, const struct stat *sb, int flag, struct FTW *ftw)
{
    (void)sb; (void)flag; (void)ftw;
    return remove(p) == 0 ? 0 : 0;
}

/* Remove the scratch directory and everything under it. */
static void fz_cleanup(void)
{
    if (fz_dirbuf[0]) nftw(fz_dirbuf, fz_rm_cb, 16, FTW_DEPTH | FTW_PHYS);
}

/* The scratch directory, created on first use. */
static const char *fz_dir(void)
{
    if (!fz_dirbuf[0]) {
        const char *base = getenv("TMPDIR");
        if (!base || !*base) base = "/tmp";
        snprintf(fz_dirbuf, sizeof fz_dirbuf, "%s/k3fuzz_XXXXXX", base);
        if (!mkdtemp(fz_dirbuf)) { perror("mkdtemp"); abort(); }
        atexit(fz_cleanup);
    }
    return fz_dirbuf;
}

static void fz_path(char *out, size_t cap, const char *name)
{
    snprintf(out, cap, "%s/%s", fz_dir(), name);
}

static void fz_write(const char *name, const void *p, size_t n)
{
    char path[2048];
    fz_path(path, sizeof path, name);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); abort(); }
    if (n && fwrite(p, 1, n, f) != n) { perror(path); abort(); }
    if (fclose(f) != 0) { perror(path); abort(); }
}

static void fz_rm(const char *name)
{
    char path[2048];
    fz_path(path, sizeof path, name);
    unlink(path);
}

/* Split an input into two parts: a little-endian u32 length of part A, part A, then
 * part B (the rest). A length past the end is clamped to what is there. */
static void fz_split(const uint8_t *d, size_t n, const uint8_t **a, size_t *an,
                     const uint8_t **b, size_t *bn)
{
    uint32_t len = 0;
    if (n >= 4) { len = (uint32_t)d[0] | (uint32_t)d[1] << 8 | (uint32_t)d[2] << 16 | (uint32_t)d[3] << 24; d += 4; n -= 4; }
    else { d += n; n = 0; }
    if (len > n) len = (uint32_t)n;
    *a = d; *an = len; *b = d + len; *bn = n - len;
}

/* Read a whole file (file-input targets). */
static uint8_t *fz_slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    size_t cap = 1 << 16, len = 0;
    uint8_t *b = (uint8_t *)malloc(cap);
    for (;;) {
        if (len == cap) { cap *= 2; b = (uint8_t *)realloc(b, cap); }
        size_t got = fread(b + len, 1, cap - len, f);
        if (!got) break;
        len += got;
    }
    fclose(f);
    *n = len;
    return b;
}
#endif
