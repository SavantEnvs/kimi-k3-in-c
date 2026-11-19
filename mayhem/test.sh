#!/usr/bin/env bash
#
# mayhem/test.sh — RUN this repo's OWN functional test suite (already built by mayhem/build.sh).
# exit 0 = pass. EDIT per repo. PATCH-grade oracle: after an agent patches the source, the grader
# rebuilds (build.sh) then runs this. DELETE this file if the repo has no meaningful tests.
#
# IMPORTANT:
#  * Must assert BEHAVIOR/OUTPUT, not just exit status. The oracle has to check asserted values /
#    golden-output diffs / known-answer results — so a PATCH that "fixes" a bug by making the program
#    exit(0) (or any no-op) FAILS here. Running inputs and checking only "exit 0 / didn't crash" is
#    NOT a functional test (it's trivially reward-hackable) — use the project's real assertion suite.
#  * Do NOT build here — mayhem/build.sh already compiled the test suite (with the project's normal
#    flags). This script only RUNS the pre-built tests and reports counts. If the test runner is
#    missing, that's a build.sh bug — fail loudly rather than silently rebuilding.
#  * REQUIRED OUTPUT — a CTRF (https://ctrf.io) summary so Mayhem/the PATCH grader reads the counts:
#      - writes a CTRF JSON report to ${CTRF_REPORT:-$SRC/ctrf-report.json}, and
#      - prints a one-line `CTRF {...}` marker to stdout (same JSON, compact).
#    Only `results.summary` (with tests/passed/failed/pending/skipped/other) is required.
#    Use the emit_ctrf helper below; it computes tests = passed+failed+skipped and sets the exit
#    code (0 iff failed==0). Map your framework's output to passed/failed/skipped.
set -uo pipefail
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH
: "${MAYHEM_JOBS:=$(nproc)}"   # build parallelism; env-overridable, falls back to nproc (use -j"$MAYHEM_JOBS")
cd "$SRC"

# emit_ctrf <tool> <passed> <failed> [skipped] [pending] [other]
# Writes a CTRF report (file + stdout `CTRF {...}` marker) and returns non-zero iff failed>0.
emit_ctrf() {
  local tool="$1" passed="$2" failed="$3" skipped="${4:-0}" pending="${5:-0}" other="${6:-0}"
  local tests=$(( passed + failed + skipped + pending + other ))
  cat > "${CTRF_REPORT:-$SRC/ctrf-report.json}" <<JSON
{
  "results": {
    "tool": { "name": "$tool" },
    "summary": {
      "tests": $tests,
      "passed": $passed,
      "failed": $failed,
      "pending": $pending,
      "skipped": $skipped,
      "other": $other
    }
  }
}
JSON
  printf 'CTRF {"results":{"tool":{"name":"%s"},"summary":{"tests":%d,"passed":%d,"failed":%d,"pending":%d,"skipped":%d,"other":%d}}}\n' \
    "$tool" "$tests" "$passed" "$failed" "$pending" "$skipped" "$other"
  [ "$failed" -eq 0 ]
}

# Upstream's weightless suite (`make test`'s C part), built by build.sh into bin/ with the
# project's normal flags. Each case must exit 0 AND print its own success line: the
# unit binaries end with a PASSED/verdict line computed from their own checks, so a
# neutered binary (no output) fails here rather than passing on its exit code.
F=tests/fixtures
B=bin
S="$(mktemp -d "${TMPDIR:-/tmp}/k3test.XXXXXX")"
trap 'rm -rf "$S"' EXIT
pass=0; fail=0

# expect <name> <required output substring> <cmd...>
expect() {
  local name="$1" want="$2"; shift 2
  local out rc
  out="$("$@" 2>&1)"; rc=$?
  if [ "$rc" -eq 0 ] && [[ "$out" == *"$want"* ]]; then
    pass=$((pass + 1)); echo "PASS $name"
  else
    fail=$((fail + 1)); echo "FAIL $name (rc=$rc)"; printf '%s\n' "$out" | tail -5 | sed 's/^/    /'
  fi
}

# refuse <name> <required stderr substring> <cmd...>: the CLI must exit 2 for this reason
refuse() {
  local name="$1" want="$2"; shift 2
  local out rc
  out="$("$@" 2>&1)"; rc=$?
  if [ "$rc" -eq 2 ] && [[ "$out" == *"$want"* ]]; then
    pass=$((pass + 1)); echo "PASS $name"
  else
    fail=$((fail + 1)); echo "FAIL $name (rc=$rc, expected 2 + '$want')"; printf '%s\n' "$out" | tail -3 | sed 's/^/    /'
  fi
}

[ -x "$B/k3" ] && [ -x "$B/test_ops" ] || { echo "test binaries missing: run mayhem/build.sh" >&2; emit_ctrf "k3-make-test" 0 1; exit 1; }

expect op_kernels           "22 passed, 0 failed"            "$B/test_ops" "$F/ops"
expect streaming_cache      "CACHE TESTS PASSED"             "$B/test_cache" "$F/cache"
expect cache_split_reads    "CACHE TESTS PASSED"             env K3_EXPERT_CHUNK=4096 "$B/test_cache" "$F/cache"
expect safetensors          "READER SELF-CHECKS PASSED"      "$B/test_st" "$F/st" "$S/st_index.json" \
    plain.f32.2d plain.bf16.1d tricky.f16.1d packed.u8.2d scalar.f32 second.shard.f32
expect model_streaming      "model stream parity: PASSED"    "$B/test_model_stream" "$F/st"
expect shard_faults         "ST-FAULT TESTS PASSED"          "$B/test_st_faults" "$F/st" "$S/stfault"
expect output_close         "output close contract: PASS"    "$B/test_output" "$F/ref_k3.json"
expect json_refusals        "PASSED"                         "$B/test_json"
expect state_file           "STATE FILE TESTS PASSED"        "$B/test_state" "$S/state"
expect config_reader        "FIXTURE CONFIG: PASS"           "$B/test_cfg" fixture "$F/ref_k3.json"
for c in no_layermap bad_layer_index bad_topk; do
  expect "config_refuses_$c" "correctly rejected"            "$B/test_cfg" reject "$F/cfg/$c.json"
done
expect interrupt            "CANCEL TESTS PASSED"            "$B/test_cancel"
expect memory_probe         "SYSMEM TESTS PASSED"            "$B/test_sysmem"
expect cgroup_memory_cap    "MEMORY PROBE TESTS PASSED"      "$B/test_mem" "$S/memprobe"
expect tokenizer_encode     "259,258,32,259,258"             "$B/test_tok" "$F/tok/good" encode "hello hello"
for c in "dup_rank:appears twice" "missing_rank:rank 100 is missing" "junk_rank:bad rank field" \
         "dup_bytes:encode the same bytes" "added_collides:already taken by a rank" "truncated:looks truncated"; do
  d="${c%%:*}"; why="${c#*:}"
  out="$("$B/test_tok" "$F/tok/$d" encode x 2>&1)"; rc=$?
  if [ "$rc" -ne 0 ] && [[ "$out" == *"$why"* ]]; then pass=$((pass + 1)); echo "PASS tokenizer_refuses_$d"
  else fail=$((fail + 1)); echo "FAIL tokenizer_refuses_$d (rc=$rc)"; fi
done
expect sampler              "SAMPLER TESTS PASSED"           "$B/test_sampler"
expect chat_prefix          "PREFIX RECORD TESTS PASSED"     "$B/test_prefix"
expect real_dimensions      "SCALE TEST PASSED"              "$B/scale_test"
expect trunk_streaming      "TRUNK TESTS PASSED"             "$B/test_trunk"
expect full_model_oracle    "ENGINE MATCHES THE REFERENCE EXACTLY" "$B/k3_model" "$F"

# CLI contracts from `make test`: each misuse is refused with exit 2 for its own reason.
refuse cli_ultra_needs_trunk  "--trunk"     "$B/k3" fake --ids 1 --preset ultra
refuse cli_stop_id_abc        "--stop-id"   "$B/k3" fake --ids 1 --stop-id abc
refuse cli_stop_id_negative   "--stop-id"   "$B/k3" fake --ids 1 --stop-id -1
refuse cli_ids_u32_overflow   "--ids"       "$B/k3" "$F/st" --config "$F/cfg/flat.json" --ids 4294967296
refuse cli_ids_huge           "--ids"       "$B/k3" "$F/st" --config "$F/cfg/flat.json" --ids 9999999999999999999999
refuse cli_layers_junk        "--layers"    "$B/k3" "$F/st" --config "$F/cfg/flat.json" --layers 1junk
refuse cli_layers_huge        "--layers"    "$B/k3" "$F/st" --config "$F/cfg/flat.json" --layers 9999999999999999999999
refuse cli_threads_junk       "--threads"   "$B/k3" "$F/st" --config "$F/cfg/flat.json" --threads 8x
refuse cli_threads_zero       "--threads"   "$B/k3" "$F/st" --config "$F/cfg/flat.json" --threads 0
refuse cli_no_think_misuse    "--no-think"  "$B/k3" fake --no-think
refuse cli_top_k_misuse       "--top-k"     "$B/k3" fake --ids 1 --top-k 4

emit_ctrf "k3-make-test" "$pass" "$fail" 0
