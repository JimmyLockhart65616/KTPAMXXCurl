#!/bin/bash
# Build + run the standalone CurlMulti harness (CU-02). See
# tests/test_curl_multi_rearm.cc for what it proves.
#
# Deliberately does NOT use build_linux.sh: that builds the shipping module and
# AUTO-STAGES it into the KTP DoD Server test tree, which would churn a wave
# artifact whose md5 is pinned. This links the same sources into a throwaway
# binary in a scratch dir and touches nothing the module build owns.
#
# Output goes to a verified mktemp dir, never into the repo (cpp-dev: "never run
# a destructive simulation inside the working tree" -- a `cd "$T"` with an empty
# $T silently leaves you in the repo).
set -u

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO" || exit 1

OUT="$(mktemp -d)" || exit 1
[ -n "$OUT" ] && [ -d "$OUT" ] || { echo "scratch dir not created"; exit 1; }
echo "scratch: $OUT"

# -m32 to match the vendored static libs (the module ships i386).
g++ -m32 -std=c++14 -O1 -g \
    -Isrc -Ideps/asio -Ideps/curl/include \
    -Ideps/halflife/common -Ideps/halflife/dlls -Ideps/halflife/engine \
    -Ideps/halflife/public -Ideps/metamod \
    -DASIO_STANDALONE -DAMXXCURL_DEBUG_LOG_ENABLE \
    tests/test_curl_multi_rearm.cc \
    tests/harness_stubs.cc \
    src/curl_multi_class.cc \
    src/asio_poller.cc \
    src/curl_utils_class.cc \
    -Wl,--start-group \
      deps/curl/lib/libcurl.a \
      deps/openssl/lib/libssl.a \
      deps/openssl/lib/libcrypto.a \
      deps/zlib/lib/libz.a \
      deps/cares/lib/libcares.a \
    -Wl,--end-group \
    -lpthread -ldl \
    -o "$OUT/curl_harness"
rc=$?

if [ $rc -ne 0 ]; then
    echo "BUILD FAILED (rc=$rc)"
    exit $rc
fi

echo "built: $OUT/curl_harness"
echo "--------------------------------------------------"
"$OUT/curl_harness" 2>&1 | tee "$OUT/run.log"
run_rc=${PIPESTATUS[0]}
echo "--------------------------------------------------"

# THE ACTUAL CU-02 GATE.
#
# The fix removed the guard that dropped events, so "count the drops" no longer has
# an analogue: arming state is per-direction now, and a completed wait is always
# reported. What stays falsifiable is the transfer itself -- reintroduce the guard
# and this stalls to CURLOPT_TIMEOUT on ~16 KB of 512. The binary's four checks
# assert exactly that, and its exit code carries them.
#
# The one thing an exit code cannot say is whether the INOUT overlap ever formed.
# Without it the run exercises nothing interesting and a green is vacuous -- so
# assert the overlap separately and treat its absence as INCONCLUSIVE, never PASS.
#
# Requires -DAMXXCURL_DEBUG_LOG_ENABLE (set above) -- without it DEBUG_LOG compiles
# to nothing and this grep would report a vacuous 0.
inout_seen=$(grep -c 'prev_action: INOUT' "$OUT/run.log")

echo "INOUT overlap observed : $inout_seen"
echo "binary exit code       : $run_rc"

if [ "$inout_seen" -eq 0 ]; then
    echo "INCONCLUSIVE: the INOUT path never ran, so this proves nothing either way."
    echo "  (a green result here would be vacuous -- fix the fixture, not the code)"
    rm -rf "$OUT"
    exit 2
fi

if [ "$run_rc" -ne 0 ]; then
    echo "HARNESS RED: INOUT was exercised and the transfer did NOT complete cleanly."
    rm -rf "$OUT"
    exit 1
fi

echo "HARNESS GREEN: INOUT exercised and the transfer completed inside the timeout."
rm -rf "$OUT"
exit 0
