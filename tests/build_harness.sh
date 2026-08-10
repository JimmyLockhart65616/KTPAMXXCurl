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
# Completing the transfer is NOT the test: libcurl's timer re-drives a socket
# whose readiness event was dropped, so the defect degrades to latency and a
# completion check passes straight through it. Measured on the broken build,
# 512 KB over loopback took ~1024 ms while dropping 7 events.
#
# So assert on the defect itself. A handler armed as INOUT(3) that fires once
# previous_action has moved to IN(1) satisfies neither arm of
#     action == previous_action || previous_action == CURL_POLL_INOUT
# and is silently discarded. Any occurrence means the re-arm bug is live.
#
# Requires -DAMXXCURL_DEBUG_LOG_ENABLE (set above) -- without it DEBUG_LOG
# compiles to nothing and this grep would report a vacuous 0.
inout_seen=$(grep -c 'INOUT' "$OUT/run.log")
# Anchored: whatstr[] is {none,IN,OUT,INOUT,REMOVE}, so an unanchored IN also
# matches INOUT -- i.e. the benign already-INOUT case, which is precisely what
# the guard handles correctly. Unanchored the gate counts every INOUT line and
# can never report GREEN.
dropped=$(grep -cE 'action: INOUT; prev_action: (IN|OUT|none)$' "$OUT/run.log")

echo "INOUT transitions observed : $inout_seen"
echo "dropped readiness events   : $dropped"

if [ "$inout_seen" -eq 0 ]; then
    echo "INCONCLUSIVE: the INOUT path never ran, so this proves nothing either way."
    echo "  (a green result here would be vacuous -- fix the fixture, not the code)"
    rm -rf "$OUT"
    exit 2
fi

if [ "$dropped" -gt 0 ]; then
    echo "HARNESS RED: CU-02 is live -- $dropped readiness event(s) dropped by the guard."
    rm -rf "$OUT"
    exit 1
fi

echo "HARNESS GREEN: INOUT exercised and no readiness event was dropped."
rm -rf "$OUT"
exit $run_rc
