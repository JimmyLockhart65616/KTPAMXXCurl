# KTPAmxxCurl shutdown-race investigation

**Status:** ACTIVE — single occurrence, low severity, not yet fixed.
**First seen:** 2026-05-04 03:00:08 EDT, ATL1 (74.91.121.9) port 27015, PID 4074640.
**Severity:** LOW. Crash is shutdown-time only (during 3 AM scheduled-restart window). No player impact, no service interruption visible to users — restart script brings up a new process within ~10 seconds.
**Escalation triggers:** SECOND host hits the same signature → MEDIUM. Crash occurs OUTSIDE the 03:00:00–03:00:30 ET window (i.e., during gameplay) → HIGH.

---

## 1. Crash signature

```
Program terminated with signal SIGSEGV, Segmentation fault.
#0  0xeb53d5a0 in ?? ()
#1  0xe883f5db in ?? ()
Backtrace stopped: previous frame inner to this frame (corrupt stack?)

eax  0xe8bc70b0    ecx  0xff9af424   edx  0xaf2bfcc
ebx  0xe8bbfff4    esp  0xff9af5cc   ebp  0xe8bbfff4
esi  0xef263470    edi  0xaf2bfcc    eip  0xeb53d5a0
```

### Address-space attribution

| Region | Range | Notes |
|---|---|---|
| `amxxcurl_ktp_i386.so` | `0xe87a9000–0xe8bc7000` | **Frame 1 EIP `0xe883f5db` is inside this range.** EBX/EBP `0xe8bbfff4` is also here (writable data). |
| `dodx_ktp_i386.so` | `0xe8677000–0xe8692000` | Not on the stack. |
| `reapi_ktp_i386.so` | `0xe870d000–0xe8797000` | Not on the stack. |
| Anonymous mmap (gap) | `0xe8bc7000–0xeb9be000` | **Crash EIP `0xeb53d5a0` is in this gap.** No mapped object covers it — this is where AMXX/Pawn JIT stages emitted code. |
| `steamclient.so` | `0xeb9be000–0xee74d000` | Not relevant. |

Path inferred: **amxxcurl → JIT'd Pawn callback → unmapped page jump.**

The "corrupt stack?" message is consistent with the JIT trampoline already having unwound its own frame before the bad jump (so gdb can't walk further), or with the freed JIT page now containing pointer-sized garbage that fails the sanity checks libunwind uses.

---

## 2. Pre-crash timeline (from rotated log `dodserver-console-2026-05-04-03:00:17.log`)

```
03:00:02  HLTV<1> disconnected (data-server hltv-restart-all.sh fired its own 3 AM cron)
03:00:02  HLTV<14> connected, 74.91.112.242:27020
03:00:02  HLTV<14> joined Spectators
03:00:02  HLTV<14> entered the game
03:00:02–03  DODX InitObj events
[4-second gap with NO log lines — quit handler running]
03:00:07  HLTV "time"/"latency"/disconnect (server told it to drop)
03:00:08  Server shutdown (×3, two via Sys_Quit/log_close paths)
03:00:08  Segmentation fault (core dumped)
```

The 4-second gap (03:00:03 → 03:00:07) is the engine running `quit` shutdown handlers — `plugin_end` on every Pawn plugin, then `Amx_Detach` on every C++ module (`amxxcurl`, `dodx`, `reapi`), then `ServerDeactivate`, then final cleanup. The crash fires AFTER the engine has logged "Server shutdown" 3×, so it is in the very last cleanup step.

### Steady-state curl activity

Every map change (~20 min cadence on dod_anzio rotation) `KTPMatchHandler` logs:
```
event=CURL_HEADERS_INIT persistent=1
event=AC_CURL_HEADERS_INIT persistent=1
event=TEAM_NAMES_RESET reason=no_pending_mode
```
Latest pre-crash init: `02:41:06`, ~19 minutes before the crash. The persistent slist had been stable. No errors / timeouts in the final window.

The only `code 28` (timeout) in the last 24h was at `2026-05-03 05:21:24` from KTPHLTVRecorder — unrelated, hours before the crash.

---

## 3. Root-cause hypothesis

**Plugin-unload-vs-curl-callback ordering bug in shutdown.**

Sequence:
1. `quit` issued at ~03:00:03.
2. AMXX runs `plugin_end` on every Pawn plugin → frees JIT pages of unloaded plugins.
3. Before `amxxcurl::Amx_Detach()` finishes draining its `curl_multi_handle`, an outstanding HTTP request finishes.
4. The result callback dispatches into a Pawn function pointer. That Pawn function's JIT page was already unmapped by step 2.
5. CPU jumps to the now-freed page → SIGSEGV.

### Why rare

The unsafe window is the gap between "plugin_end has run on the callback's owning plugin" and "amxxcurl Amx_Detach has fully torn down its multi-handle". On a normal day no curl request happens to complete inside that window — most matchday HTTP traffic is bursty and completes within tens of milliseconds; nothing's typically in flight at 3 AM during the quiet warmup.

What plausibly primed today's crash: the HLTV proxy reconnected at 03:00:02 (5 seconds before `quit`). HLTV `client_putinserver` triggers KTPMatchHandler/KTPHLTVRecorder forwards. If either plugin issued an HTTP request as part of that handling, the request's response could land in the unsafe window 4 seconds later.

### Why this is NOT one of the previously-resolved curl bugs

| Prior bug | This one |
|---|---|
| **HudObserver curl_slist UAF** (resolved `7e1ce00`, 2026-04-27): freed an active slist on map change in steady state. | Shutdown-only, no map-change involvement. |
| **HudObserver fault** crashed inside `Curl_strncasecompare` with ASCII-data-as-pointer addresses. | Crash EIP is in anonymous mmap (JIT region). |
| **KTPHLTVRecorder 1.5.0 segfault** (2026-02-18): shared g_curlHeaders torn down across overlapping in-flight requests. | Current 1.7.0 is HLTV-cfg-driven and doesn't touch slists during gameplay. |

Different fault class entirely. Memory entries `[KTPAmxxCurl Async Gotcha]` and `[hud_observer_uaf_2026-04-26]` document the prior patterns; this one would be a new sibling pattern in the same family ("amxxcurl callback dispatch races a Pawn lifetime event").

---

## 4. Fix candidates

Listed cheapest → most principled.

### Option A — Validate-before-dispatch (smallest diff)
In `Amx_Curl_Callback_class` dispatch (in amxxcurl), before calling the Pawn function, check the target plugin's load state. Skip the callback if the plugin is in "unloading" or "unloaded" state.

- **Pro:** smallest diff, validates at use-site, mirrors the pattern Jimmy used for the HudObserver fix.
- **Con:** Depends on a reliable "is plugin loaded" check. AMXX exposes plugin state via `MF_GetAmxxFuncs()`/`g_plugins` but the timing of state transitions across `plugin_end` boundaries needs verification.

### Option B — Cancel-without-callback in Amx_Detach
In `amxxcurl::Amx_Detach()`, abort all outstanding handles via `curl_multi_remove_handle` + `curl_easy_cleanup` WITHOUT invoking their result callbacks.

- **Pro:** Simple; eliminates the UAF unconditionally in the shutdown path.
- **Con:** Loses telemetry on the in-flight requests at shutdown. Acceptable — these are dropped anyway since the engine is going down.

### Option C — Drain-before-detach (most principled)
In `amxxcurl::Amx_Detach()`, drain the curl multi-handle (`curl_multi_perform` until `still_running == 0` AND callback queue empty) BEFORE any plugin's `plugin_end` runs.

- **Pro:** Correct fix at the source. Keeps callback semantics intact.
- **Con:** Requires hooking earlier than `Amx_Detach` in the AMXX shutdown ordering, and bounded blocking on remote HTTP could delay shutdown by up to the request timeout (typically 30s). Bad UX for shutdown.

**Recommendation:** Option B, because:
- Shutdown is the only context where this fires; losing in-flight callback telemetry there is fine.
- Simple change, easy to review, easy to test.
- Doesn't add shutdown latency.

If we want telemetry preserved, fall back to Option A.

---

## 5. Reproduction notes

We don't have a clean repro yet. The crash has fired exactly once across 25 instances over 1+ days. Suggestions:

- **Synthetic repro:** A test that queues N curl POSTs (each to a fast endpoint that responds in 5-50ms), then issues `quit` while requests are in flight. Run in a loop. With current code we'd expect to hit this UAF within tens of iterations.
- **Targeted instrumentation:** Add a `[CURL_INFLIGHT_AT_DETACH]` log line in `amxxcurl::Amx_Detach()` reporting the count of pending handles. Deploy fleet-wide for a week and correlate any new crash with the count at time-of-shutdown.
- **Static analysis:** Walk the amxxcurl source in `KTPAmxxCurl/src/` (especially `amx_curl_callback_class.cc` which we already know was missing from the CMake migration) and audit:
  - Where is `Amx_Detach` defined?
  - Does it iterate the multi-handle and call `curl_easy_cleanup` on each?
  - Where are callback Pawn-function pointers stored, and what's their lifetime tied to?

---

## 6. Severity rationale

LOW because:
- **No player impact.** No one is connected at 3 AM ET (per our scheduled-restart window). HLTV was the only client at the time and it had just reconnected for its own restart; it disconnected when the server told it to.
- **Self-healing.** ktp-scheduled-restart.sh starts a new process within seconds. 03:00:17 saw the new PID alive; 6h25m+ uptime by 09:25 with no further issues.
- **Single occurrence on one host.** Not fleet-wide. Apr 30 23:17 ATL1 crash is a different root cause (sprite precache, already resolved).
- **No data loss.** Match state in localinfo persists across the restart by design. Any in-flight curl requests at shutdown were going to be lost anyway with `quit`.

Worth fixing for cleanliness — a SIGSEGV in production is a smell — but does not warrant interrupting other work.

---

## 7. Cross-references

- Crash artifacts: `_crash-artifacts/atl1-2026-05-04/core.4074640.bt`, `console-tail-before-crash.log`, `verify_sprites.py`, `sweep_other_regions.py`, `investigate_shutdown_race.py`.
- Memory: `amxxcurl_shutdown_race_2026-05-04.md` (this doc's brief twin).
- Related memory (different bug, shared module): `KTPAmxxCurl Async Gotcha` (1.5.0 era), `hud_observer_uaf_2026-04-26.md` (resolved 2026-04-27).
- TODO: § "KTPAmxxCurl shutdown-time crash".
- KTPAmxxCurl version at time of crash: **1.3.8-ktp**.
