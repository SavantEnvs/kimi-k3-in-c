/* fuzz_json - third_party/json.h, the parser every config, safetensors header,
 * trunk manifest and tokenizer config in the engine goes through. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "json.h"

static double walk(jval *v, int depth)
{
    double acc = 0;
    if (!v || depth > 64) return 0;
    switch (v->t) {
    case J_NUM: acc += v->num; break;
    case J_BOOL: acc += v->boolean; break;
    case J_STR: acc += (double)strlen(v->str); break;
    case J_ARR: for (int i = 0; i < v->len; i++) acc += walk(v->kids[i], depth + 1); break;
    case J_OBJ:
        for (int i = 0; i < v->len; i++) {
            acc += (double)strlen(v->keys[i]);
            acc += walk(json_get(v, v->keys[i]), depth + 1);
        }
        break;
    default: break;
    }
    return acc;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    /* json_parse takes a NUL-terminated document (as every caller hands it). */
    char *text = (char *)malloc(size + 1);
    if (!text) return 0;
    memcpy(text, data, size);
    text[size] = 0;
    char *arena = NULL;
    jval *root = json_parse(text, &arena);
    if (root) {
        volatile double sink = walk(root, 0);
        (void)sink;
        (void)json_get(root, "__metadata__");
        (void)json_get(root, "weight_map");
        json_free_tree(root);
    }
    free(arena);
    free(text);
    return root ? 0 : -1;
}
