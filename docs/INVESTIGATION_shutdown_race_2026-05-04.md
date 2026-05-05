# KTPAmxxCurl shutdown-race investigation

**Status:** ACTIVE — root cause identified, fix scoped, **not yet shipped**.
**First seen:** 2026-05-04 03:00:08 EDT, ATL1 (74.91.121.9) port 27015, PID 4074640.
**Second occurrence:** 2026-05-05 03:00:13 EDT, DEN5 (66.163.114.109) port 27019, PID 1577072. Identical signature.
**Severity (current):** **HIGH** — escalated from LOW after second-host hit on 2026-05-05. Still 3 AM ET window only (no players, restart script brings up a new process within ~10 seconds), but the failure mode is now confirmed reproducible across hosts and consecutive nights, not a one-off.
**Escalation triggers:** crash OUTSIDE the 03:00:00–03:00:30 ET window (i.e., during gameplay) → CRITICAL. Third host before fix ships → CRITICAL.

---

## 1. Crash signature

Both crashes have **byte-identical relative offsets** in `amxxcurl_ktp_i386.so`. ASLR varies the absolute addresses; everything else matches.

| | ATL1 (2026-05-04) | DEN5 (2026-05-05) |
|---|---|---|
| EIP | `0xeb53d5a0` | `0xed3865a0` |
| Frame 1 (return-target in amxxcurl) | `0xe883f5db` | `0xecda95db` |
| Frame 1 offset within amxxcurl `.text` | `0x965db` | `0x965db` ← **identical** |
| EAX (= `&g_fn_FindAmxScriptByAmx` in amxxcurl bss) | `0xe8bc70b0` | `0xed1310b0` |
| EAX − amxxcurl bss-end | `0xb0` | `0xb0` ← **identical** |
| EFLAGS | `0x210296` | `0x210296` ← **identical** |
| Backtrace truncation | "corrupt stack?" frame 1 | "corrupt stack?" frame 1 |
| LWP count | 1 (single thread) | 1 (single thread) |

### Address-space attribution

| Region | ATL1 range | DEN5 range | Notes |
|---|---|---|---|
| `amxxcurl_ktp_i386.so` `.text` | `0xe87a9000–0xe882b000` | `0xecd13000–0xecd95000` | **Frame 1 falls inside `.text` on both** — confirmed via `objdump -d`. |
| `amxxcurl_ktp_i386.so` (last segment) | `0xe8bc0000–0xe8bc7000` | `0xed12a000–0xed131000` | EAX is `+0xb0` past this segment end on both → `g_fn_FindAmxScriptByAmx` location. |
| Anonymous mmap (gap) | `0xe8bc7000–0xeb9be000` | `0xed131000–0xeff29000` | **Crash EIP falls in this gap on both.** No object mapped — formerly KTPAMXX core's `.text`, now unmapped. |
| `steamclient.so` | `0xeb9be000–...` | `0xeff29000–...` | Not on the stack on either. |

### Disassembly at the crashing instruction

The byte that the CPU was about to execute (frame 1 = return target) is at module offset `0x965db`, inside `_ZN15CurlCallbackAmxD1Ev` (`CurlCallbackAmx::~CurlCallbackAmx()`):

```asm
000965b0 <_ZN15CurlCallbackAmxD1Ev>:
   ; ... vtable reset prologue ...
   965d0:   mov    eax, DWORD PTR [ebx-0x300]   ; eax = &g_fn_FindAmxScriptByAmx
   965d6:   push   DWORD PTR [edi+0x4]           ; push amx_ (this->amx_)
   965d9:   call   DWORD PTR [eax]               ; ★ call g_fn_FindAmxScriptByAmx(amx_)
   965db:   add    esp, 0x10                     ; ← gdb's "frame 1" = return target
   965de:   cmp    eax, 0xffffffff
   965e1:   je     966f0                         ; jump to else-branch if -1
```

This is the inlined body of `IsAmxValid()`:

```cpp
bool IsAmxValid() const { return MF_FindScriptByAmx(amx_) != -1; }
```

`MF_FindScriptByAmx` is `#define`d to `g_fn_FindAmxScriptByAmx` (a function-pointer in amxxcurl's bss, set by `REQFUNC` at `Amx_Attach` time). At crash time, `[eax]` returns the stored pointer value, which now points into the unmapped gap (formerly KTPAMXX core's `.text`).

**The "corrupt stack?" symptom** is real but a red herring: the call destination `0xeb53d5a0` is in unmapped memory, so the CPU faults *before* a proper frame is pushed. gdb sees a partial stack frame (the `push amx_` at 965d6) and bails on libunwind sanity checks.

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

## 3. Root cause (CONFIRMED 2026-05-05)

The original hypothesis ("Pawn JIT page got freed") was close but wrong in a specific way. The actual bug is:

**Static-destruction-order failure: the `AmxCurlController` Meyers singleton's destructor fires after KTPAMXX core has been unmapped, and the inline `IsAmxValid()` guard inside `~CurlCallbackAmx()` calls `MF_FindScriptByAmx` through a now-stale function-pointer.**

### Mechanism

`AmxCurlController` (`src/amx_curl_controller_class.h`) is a Meyers singleton — a function-local `static`:

```cpp
static AmxCurlController& Instance() {
    static AmxCurlController instance;     // ← constructed on first call,
    return instance;                       //   destroyed during .fini phase
}
```

Its destructor runs as part of amxxcurl's `__cxa_finalize` (i.e., when amxxcurl is dlclose'd, or when the process exits). At that point KTPAMXX core's `.so` has already been unmapped — engine shutdown unloads modules in dependency order, and KTPAMXX core's `dlclose` happens before the C++ runtime gets to amxxcurl's static destructors (or during the same engine-exit cleanup, after a cascade of `Amx_Detach` returns).

`OnAmxxDetach` *does* call `manager.RemoveAllTasks()` to clear `amx_curl_` while KTPAMXX is still alive. But there's a 5-second drain timeout (`callbacks.cc` lines 56-61), and at least one of the following can leave a `CurlCallbackAmx` instance reachable past the drain:

- The drain **times out** before all in-flight curl handles complete (the warning at line 64 is silent in our crash logs, but it's not fatal — the code falls through to `RemoveAllTasks()` regardless).
- An asio handler captured the `shared_ptr<CurlCallbackAmx>` and runs after `RemoveAllTasks()` clears the map.
- libcurl's `curl_easy_cleanup` (called from `~Curl()`) flushes a final WriteCallback that holds a `shared_ptr` reference for the duration of the call.

When the surviving instance is finally destroyed (during the singleton's static-dtor sweep), `~CurlCallbackAmx()` runs:

```cpp
CurlCallbackAmx::~CurlCallbackAmx() {
    if (IsAmxValid())                  // ← inline: MF_FindScriptByAmx(amx_) != -1
        ResetAmxCallbacks();
    else
        registered_callbacks_.clear();
    response_body_.clear();
}
```

`IsAmxValid()` was added precisely to prevent calling into a freed AMX object. But the implementation goes through `g_fn_FindAmxScriptByAmx` — the AMXX-SDK function-pointer to KTPAMXX core's `FindAmxScriptByAmx`. By the time we're at static-dtor time, that pointer is stale (KTPAMXX `.text` is unmapped). The dereference jumps into the gap → SIGSEGV.

### Why both hosts on consecutive nights

Identical trigger: HLTV proxy reconnect at 03:00:01–03 from the data-server `hltv-restart-all.sh` cron → KTPMatchHandler/KTPHLTVRecorder forwards on `client_putinserver` → at least one plugin issues a Discord/HLStatsX HTTP POST → engine `quit` fires at 03:00:07 → request still in-flight at OnAmxxDetach → drain may time out OR a late asio handler keeps the shared_ptr alive past `RemoveAllTasks` → singleton dtor at .fini → crash 4–10s later.

The pattern is fleet-wide. We've now hit it on two different hosts in two different regions, on different ports, on consecutive nights. It's not an ATL1- or DEN5-specific quirk.

### Why this is NOT one of the previously-resolved curl bugs

| Prior bug | This one |
|---|---|
| **HudObserver curl_slist UAF** (resolved `7e1ce00`, 2026-04-27): freed an active slist on map change in steady state. | Shutdown-only, no map-change involvement. |
| **HudObserver fault** crashed inside `Curl_strncasecompare` with ASCII-data-as-pointer addresses. | Crash is in `~CurlCallbackAmx` calling through a stale `MF_*` function pointer. |
| **KTPHLTVRecorder 1.5.0 segfault** (2026-02-18): shared g_curlHeaders torn down across overlapping in-flight requests. | Current 1.7.0 is HLTV-cfg-driven and doesn't touch slists during gameplay. |

Different fault class entirely. Memory entries `[KTPAmxxCurl Async Gotcha]` and `[hud_observer_uaf_2026-04-26]` document the prior patterns; this is a new sibling — "amxxcurl static destruction races KTPAMXX core unload".

---

## 4. Recommended fix

The original options were drafted before we knew the crash was inside `IsAmxValid()` itself. Now that we do, the answer changes: the bug is that `IsAmxValid()` uses an MF_* function-pointer to validate, and we need a way to short-circuit it that does NOT touch any MF_*.

**Module-level "detached" flag, short-circuited in `IsAmxValid()`.**

Three small edits:

1. `src/amx_curl_callback_class.h` — declare the flag, short-circuit `IsAmxValid()`:
   ```cpp
   #include <atomic>
   extern std::atomic<bool> g_amxxcurl_detached;

   bool IsAmxValid() const {
       return !g_amxxcurl_detached.load(std::memory_order_acquire)
              && MF_FindScriptByAmx(amx_) != -1;
   }
   ```

2. `src/callbacks.cc` — define the flag and set it at the END of `OnAmxxDetach`, after every in-flight transfer has been drained:
   ```cpp
   std::atomic<bool> g_amxxcurl_detached{false};

   void OnAmxxDetach() {
       // ... existing drain + cleanup ...
       manager.RemoveAllTasks();
       curl_global_cleanup();
       // After this line, MF_* function pointers may become stale at any moment.
       // Late CurlCallbackAmx destructors (from .fini) will short-circuit
       // IsAmxValid() and take the safe registered_callbacks_.clear() branch.
       g_amxxcurl_detached.store(true, std::memory_order_release);
   }
   ```

3. `src/amx_curl_class.h` — same short-circuit at the `OnPerformComplete` validation site (line 83), so an asio handler firing late doesn't `MF_RegisterSPForward` into freed memory either.

### Why this is correct

- After `g_amxxcurl_detached == true`, the `IsAmxValid()` short-circuit returns false → `~CurlCallbackAmx()` takes the `else { registered_callbacks_.clear(); }` branch, which is pure STL container teardown. No MF_* call.
- The flag itself lives in amxxcurl's bss, which is alive as long as amxxcurl is alive (i.e., for the lifetime of the singleton dtors).
- Atomics with acquire/release ordering protect against any conceivable cross-thread sequencing (in practice the curl callbacks are all on the main thread via asio, but `release` is essentially free here).
- Fits the repository's existing pattern: same "check a flag, skip the dangerous call" idiom used in `~CurlMulti()` / `CurlTimerCallback` (curl_multi_class.cc:201–204).

### What about losing telemetry on late-completing requests?

By design, late-completing requests at shutdown have nowhere useful to dispatch (the engine is going down, the AMX context is gone). The current code already plans to drop them — the early-return at amx_curl_class.h:83 does so when `MF_FindScriptByAmx == -1`. We're just making the *guard itself* safe to call after KTPAMXX is gone.

### Rejected alternatives

- **Make `OnAmxxDetach()` block until truly drained.** Bounded by request timeout (up to 30s), would block engine shutdown unacceptably long for cases where the remote endpoint is slow.
- **Move the singleton's lifetime into `OnAmxxDetach`.** Would require restructuring controller ownership — much larger diff for the same outcome.
- **Use a "dlsym KTPAMXX_isalive" probe.** Brittle; relies on internal AMXX symbols not exported across versions.

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

## 6. Severity rationale (updated 2026-05-05)

**HIGH** because:
- **Reproducible.** Two different hosts in two different regions on consecutive nights, identical signature, identical relative offset inside amxxcurl. This is not a one-off cosmic-ray event; it's a real bug in our shutdown teardown.
- **Trigger is fleet-wide.** The HLTV reconnect cron at 03:00:01-03 hits all 25 HLTV proxies. Any host with a curl POST in flight when `quit` fires can hit this. We've now confirmed the prerequisite holds widely enough to catch instances on consecutive nights.
- **Engine-shutdown smoke.** A SIGSEGV during shutdown is unsightly and obscures real shutdown failures in monitoring (the relay bot pings @here for these).

Mitigating (still HIGH, but not yet CRITICAL):
- **No player impact.** No one is connected at 3 AM ET. HLTV is the only client at the time.
- **Self-healing.** ktp-scheduled-restart.sh starts a new process within seconds. New PIDs were live by 03:00:17 / 03:00:24 on ATL1 / DEN5 respectively.
- **No data loss.** Match state in localinfo persists across the restart by design. Any in-flight curl requests at shutdown were going to be lost anyway with `quit`.

Promotion to CRITICAL would happen if the crash ever fired during gameplay (i.e., outside the 03:00:00–03:00:30 ET shutdown window) or if a third host hits it before the fix ships.

---

## 7. Cross-references

- Crash artifacts (ATL1 first occurrence): `_crash-artifacts/atl1-2026-05-04/core.4074640.bt`, `console-tail-before-crash.log`, `verify_sprites.py`, `sweep_other_regions.py`, `investigate_shutdown_race.py`.
- Crash artifacts (DEN5 second occurrence): `_crash-artifacts/den5-2026-05-05/core.1577072.bt`, `console-tail-before-crash.log`.
- Memory: `amxxcurl_shutdown_race_2026-05-04.md` (this doc's brief twin, updated 2026-05-05 with HIGH severity + DEN5).
- Related memory (different bug, shared module): `KTPAmxxCurl Async Gotcha` (1.5.0 era), `hud_observer_uaf_2026-04-26.md` (resolved 2026-04-27).
- TODO: § "KTPAmxxCurl shutdown-time crash".
- KTPAmxxCurl version at time of both crashes: **1.3.8-ktp** (binary mtime Apr 19, sha256 `c9b75cb5e718f3aec80b32c43cb1ebbba38f4fcb58774f9af02294ef49b86d7b`).
- Fix will ship as **1.3.9-ktp**.
