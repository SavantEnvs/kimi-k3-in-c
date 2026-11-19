#!/usr/bin/env bash
#
# mayhem/build.sh - build kimi-k3-in-c's fuzz harnesses and its weightless test suite.
#
# Runs in the commit image as `mayhem` in /mayhem (the base exports CC, SANITIZER_FLAGS,
# LIB_FUZZING_ENGINE, STANDALONE_FUZZ_MAIN, SRC). Re-runnable and offline: no fetches.
#
#   1. the engine sources, compiled twice into mayhem/build/ (gitignored by upstream's
#      `build/` rule): with $SANITIZER_FLAGS + SanCov for the libFuzzer targets, and
#      with afl-clang-fast for the two file-input targets whose code exit()s
#   2. the harnesses in mayhem/harnesses/: libFuzzer binary + -standalone reproducer,
#      or one AFL binary
#   3. upstream's own test binaries with its normal flags (`make`, into build/ and bin/),
#      minus -march=native and OpenMP (the base has no libomp; the pragmas compile out)
set -euo pipefail
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH

: "${SANITIZER_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer}"
: "${DEBUG_FLAGS:=-g -gdwarf-3}"
: "${CC:=clang}" ; : "${CXX:=clang++}" ; : "${LIB_FUZZING_ENGINE:=-fsanitize=fuzzer}"
: "${STANDALONE_FUZZ_MAIN:=/opt/mayhem/StandaloneFuzzTargetMain.c}"
: "${MAYHEM_JOBS:=$(nproc)}"
: "${COVERAGE_FLAGS=}"
: "${AR:=ar}"
export SANITIZER_FLAGS DEBUG_FLAGS CC CXX LIB_FUZZING_ENGINE MAYHEM_JOBS COVERAGE_FLAGS

cd "$SRC"
OUT="$SRC"
B="$SRC/mayhem/build"
mkdir -p "$B/gen" "$B/lf" "$B/afl"

INC="-Iinclude -Iinclude/k3 -Ithird_party -Isrc/core -Isrc/io -Isrc/cache -Isrc/model \
-Isrc/tokenizer -Isrc/chat -Isrc/cli -I$B/gen -Imayhem/harnesses"
BASE="-std=gnu99 -O1 -pthread -ffp-contract=off -Wno-unknown-pragmas -Wno-unused-function"

# Fixtures the harnesses write into their scratch dir, compiled in so no target reads
# the image's tree (generated from committed files on every run).
F=tests/fixtures
python3 -I mayhem/gen_embed.py "$B/gen/k3_embed_tok.h" \
    k3e_good_tiktoken=$F/tok/good/tiktoken.model k3e_good_tokcfg=$F/tok/good/tokenizer_config.json
python3 -I mayhem/gen_embed.py "$B/gen/k3_embed_cli.h" \
    k3e_st1=$F/st/model-00001-of-00002.safetensors k3e_st2=$F/st/model-00002-of-00002.safetensors \
    k3e_flat_cfg=$F/cfg/flat.json

ENGINE="src/core/k3_ops.c src/io/k3_st.c src/io/k3_load.c src/io/k3_trunk.c src/cache/k3_cache.c \
src/model/k3_bind.c src/chat/k3_chat.c src/chat/k3_sampler.c src/cli/k3_state.c"

# --- 1. engine library, two instrumentations --------------------------------------
build_lib() {  # <dir> <cc> <flags...>
    local d="$1" cc="$2"; shift 2
    local objs=()
    for s in $ENGINE src/cli/k3_run.c; do
        local o="$d/$(basename "${s%.c}").o" extra=""
        [ "$s" = src/cli/k3_run.c ] && extra="-Dmain=k3_cli_main"
        # shellcheck disable=SC2086
        $cc $BASE "$@" $INC $extra -c "$s" -o "$o"
        [ "$s" = src/cli/k3_run.c ] || objs+=("$o")
    done
    rm -f "$d/libk3.a"
    $AR rcs "$d/libk3.a" "${objs[@]}"
}
# shellcheck disable=SC2086
build_lib "$B/lf" "$CC" $SANITIZER_FLAGS $DEBUG_FLAGS -fsanitize=fuzzer-no-link
# shellcheck disable=SC2086
$CC $SANITIZER_FLAGS $DEBUG_FLAGS -c mayhem/lsan_off.c -o "$B/lf/lsan_off.o"
# shellcheck disable=SC2086
$CC $SANITIZER_FLAGS $DEBUG_FLAGS -c "$STANDALONE_FUZZ_MAIN" -o "$B/lf/standalone_main.o"

# shellcheck disable=SC2086
AFL_QUIET=1 build_lib "$B/afl" afl-clang-fast $SANITIZER_FLAGS $DEBUG_FLAGS
# shellcheck disable=SC2086
afl-clang-fast $SANITIZER_FLAGS $DEBUG_FLAGS -c mayhem/lsan_off.c -o "$B/afl/lsan_off.o"

# --- 2. harnesses -------------------------------------------------------------------
for h in json cfg st trunk tok_encode chat_history state; do
    src="mayhem/harnesses/fuzz_$h.c"
    # shellcheck disable=SC2086
    $CC $BASE $SANITIZER_FLAGS $DEBUG_FLAGS -fsanitize=fuzzer-no-link -D_GNU_SOURCE= $INC -c "$src" -o "$B/lf/fuzz_$h.o"
    # shellcheck disable=SC2086
    $CC $SANITIZER_FLAGS $DEBUG_FLAGS $LIB_FUZZING_ENGINE "$B/lf/fuzz_$h.o" "$B/lf/lsan_off.o" \
        "$B/lf/libk3.a" -lm -pthread -o "$OUT/fuzz_$h"
    # shellcheck disable=SC2086
    $CC $SANITIZER_FLAGS $DEBUG_FLAGS "$B/lf/standalone_main.o" "$B/lf/fuzz_$h.o" "$B/lf/lsan_off.o" \
        "$B/lf/libk3.a" -lm -pthread -o "$OUT/fuzz_$h-standalone"
done

# shellcheck disable=SC2086
AFL_QUIET=1 afl-clang-fast $BASE $SANITIZER_FLAGS $DEBUG_FLAGS -D_GNU_SOURCE= $INC mayhem/harnesses/fuzz_tok_load.c \
    "$B/afl/lsan_off.o" -lm -pthread -o "$OUT/fuzz_tok_load"
# shellcheck disable=SC2086
AFL_QUIET=1 afl-clang-fast $BASE $SANITIZER_FLAGS $DEBUG_FLAGS -D_GNU_SOURCE= $INC mayhem/harnesses/fuzz_cli.c \
    "$B/afl/k3_run.o" "$B/afl/lsan_off.o" "$B/afl/libk3.a" -lm -pthread -o "$OUT/fuzz_cli"

# --- 3. upstream's weightless test suite, normal flags ------------------------------
TESTS="test_ops test_cache test_st test_model_stream test_cfg test_tok test_chat test_sampler \
scale_test k3_model test_trunk test_st_faults test_sysmem test_mem test_prefix test_state \
test_cancel test_json test_output"
# shellcheck disable=SC2046
env -u CFLAGS -u LDFLAGS -u SANITIZER_FLAGS make -j"$MAYHEM_JOBS" CC="$CC" ARCH= OMP_CFLAGS= OMP_LDFLAGS= \
    ${COVERAGE_FLAGS:+CFLAGS="-O3 -std=gnu99 -pthread -ffp-contract=off $COVERAGE_FLAGS" LDFLAGS="-lm -pthread $COVERAGE_FLAGS"} \
    all $(for t in $TESTS; do printf 'bin/%s ' "$t"; done)

echo "build.sh: done"
