# Changelog

All notable changes to KTP CURL AMXX will be documented in this file.

## [1.3.11-ktp] - 2026-05-13

### Fixed
- **SIGABRT in `WrapTcpSocket` from throw across C-callback boundary** — CHI1 27015 crashed at 2026-05-13 00:26 ET with a `std::system_error("assign: Bad file descriptor")` propagating out of `AsioPoller::WrapTcpSocket` through `CurlSocketCallbackStatic`, hitting the C-callback boundary from libcurl with no handler, and tripping `std::terminate()`. Distinct failure class from the 1.3.9 / 1.3.10 shutdown race (`~CurlCallbackAmx()` at module-detach time) — this one is mid-operation during normal traffic when libcurl's multi-handle dispatch hands us a stale fd that a sibling event closed between socket-callback issuance and our dispatch. The prior `WrapTcpSocket` used the throwing `basic_socket(io_context, protocol, native_socket)` constructor, which calls `asio::detail::throw_error(ec, "assign")` (`basic_socket.hpp:164`) on EBADF. Fix: `WrapTcpSocket` now takes an `asio::error_code& ec` out-param and uses the non-throwing `socket.assign(protocol, native_socket, ec)` overload (`basic_socket.hpp:389`); on EBADF the returned socket is default-constructed (not open) and `ec` is populated. Caller in `CurlMulti::CurlSocketCallback` handles the failure by calling `curl_multi_assign(curl_multi_, s, nullptr)` to clear libcurl's stale `socketp` for this fd, erasing our `socket_data_map_` entry, and returning 0 so libcurl can continue dispatching other transfers. Belt-and-suspenders try/catch at the `CurlSocketCallbackStatic` boundary returns `-1` on any future escaping exception (libcurl interprets as transfer abort, not process crash). ktp-code-review round 1 caught the missing `curl_multi_assign` clear (without it libcurl retained the stale socketp pointer across our recovery); round 2 caught the thread-safety note about `MF_PrintSrvConsole` (safe in current single-threaded `Poll()` model; would need a thread-safe log queue if io_context ever moves to worker threads). See memory `amxxcurl_asio_throw_assign.md` for the full investigation. 
  
  **Deployment timeline:** binary md5 `b1932ed0c74efe6eff1cf1c68b6ddd0a` SCP'd as `.so.new` to all 24/24 active fleet instances + tier2 runner 2026-05-13; auto-swapped 2026-05-14 03:00 EDT via the established `ktp-scheduled-restart.sh` glob; soak window 2026-05-14 → 2026-05-21.

---

## [1.3.10-ktp] - 2026-05-05

### Changed
- **Flag-store ordering inverted in `OnAmxxDetach`** — `g_amxxcurl_detached.store(true)` now happens AFTER the in-flight transfer drain loop exits but BEFORE `manager.RemoveAllTasks()` and `curl_global_cleanup()`. The 1.3.9 order (flag stored at the very end) closed the confirmed crash path but left a theoretical window: if a `shared_ptr<CurlCallbackAmx>` escaped `RemoveAllTasks` via a late asio handler that captured one, the destructor would have observed `detached=false` and dereferenced `g_fn_FindAmxScriptByAmx` after KTPAMXX core was already unmapped — exactly the failure mode 1.3.9 was meant to prevent. The new order eliminates that window: every `~CurlCallbackAmx()` fired during or after teardown atomically sees `detached=true` and takes the safe `registered_callbacks_.clear()` path. Skipping `MF_UnregisterSPForward` during teardown is correct — KTPAMXX is about to free its forward table anyway. The drain loop runs first so legitimate in-flight completion callbacks still fire with valid MF_* function pointers; only after the loop returns does the no-op path activate. Defensive-only: no current code path triggers the previous failure mode (1.3.9 already covers the confirmed `0x965d6` crash signature). 5-line diff (one block move + comment refresh in `src/callbacks.cc`; declaration comment refresh in `src/amx_curl_callback_class.h`). See `docs/INVESTIGATION_shutdown_race_2026-05-04.md`.

---

## [1.3.9-ktp] - 2026-05-05

### Fixed
- **Shutdown SIGSEGV in `~CurlCallbackAmx()` after KTPAMXX core unmap** — When a curl request was still in flight at engine `quit` and the 5-second drain in `OnAmxxDetach` couldn't reach a clean state (or an asio handler held a `shared_ptr<CurlCallbackAmx>` past `RemoveAllTasks`), the callback object survived into the `AmxCurlController` Meyers singleton's static-destructor phase. Its destructor calls `IsAmxValid()`, which calls `MF_FindScriptByAmx(amx_)` — an indirect call through `g_fn_FindAmxScriptByAmx`. By that point KTPAMXX core's `.text` is already unmapped, so the call jumps into a freed page and segfaults. Hit on ATL1 27015 (2026-05-04 03:00:08 EDT) and DEN5 27019 (2026-05-05 03:00:13 EDT) at the scheduled-restart window, with byte-identical relative offsets in the amxxcurl module — confirmed root cause via `objdump -d` at offset `0x965d6`. Trigger correlated with HLTV proxy's 03:00:01-03 reconnect cron priming a Discord/HLStatsX POST that landed in the unsafe window. Fix: module-level `std::atomic<bool> g_amxxcurl_detached`, set at the very end of `OnAmxxDetach`. `IsAmxValid()` and `OnPerformComplete()` now short-circuit on it before any `MF_*` call, so late destructors take their safe `registered_callbacks_.clear()` branch and never dereference the stale function pointer. No behavior change at gameplay time. See `docs/INVESTIGATION_shutdown_race_2026-05-04.md`.

---

## [1.3.8-ktp] - 2026-04-19

### Fixed
- **`CURLM_RECURSIVE_API_CALL` when `timeout_ms == 0`** (PR #1 by JimmyLockhart65616) — `CurlTimerCallback` fires synchronously from inside libcurl callbacks (notably `curl_multi_add_handle`). Calling `curl_multi_socket_action` directly in that context is rejected by libcurl as a recursive API call (rc=8), so transfers never progressed and user callbacks never fired on high-RPS streams. Fix posts the socket-action to the asio `io_context` so it runs on the next `Poll()` outside any libcurl callback. Observed while wiring the DoD HUD Observer ingest path at ~50 events/sec.
- **UAF guard on `this`-captured asio handlers** (PR #2) — The lambda posted in the `timeout_ms == 0` branch and the existing `async_wait(std::bind(&CurlMulti::AsioTimerCallback, this, _1))` in the `timeout_ms > 0` branch both capture `this`. If either runs after `~CurlMulti()` (e.g. if any `Poll()` fires between the drain loop exit and `RemoveAllTasks` during `OnAmxxDetach`), the `curl_multi_` member would already be cleaned up. `~CurlMulti()` now sets `curl_multi_ = nullptr` and both handlers early-return on null. Defensive — no current code path reaches this, but it removes a footgun for future shutdown-path changes.
- **`moduleconfig.h` `MODULE_VERSION` was frozen at `1.3.6-ktp`** — missed during the 1.3.7 release; the module was self-identifying as `1.3.6-ktp` in logs and plugin-info messages despite the binary containing 1.3.7 code. Now tracks the real version.

---

## [1.3.7-ktp] - 2026-04-02

### Changed
- **Build system migrated to CMake** — Replaced Premake5 + generated Makefiles with a single `CMakeLists.txt`. Consistent with KTP-ReHLDS and KTP-ReAPI build systems.
- **Compiler optimizations** — `-O3 -march=native -mtune=native -flto -fno-math-errno` for CPU-specific instructions and link-time optimization.

### Fixed
- **Buffer overflow in `amx_curl_formadd`** — `strcpy` replaced with `strncpy` + null terminator. `MF_GetAmxString` could return strings larger than the 16384-byte buffer, causing heap overflow.
- **Memory leak in `amx_curl_easy_perform`** — Added catch-all exception handler to prevent `data` array leak on unexpected exceptions during `CurlPerformTask`.
- **Exception in libcurl callback (`SetSock`)** — Replaced `throw std::runtime_error` with graceful return + debug log. Throwing from a libcurl socket callback is undefined behavior and can crash the server.
- **Exception safety in `AddCurl`** — Reordered operations to set curl options before inserting into `curl_map_`. If `SetOption` throws, the handle is no longer orphaned in the map.
- **CPU busy-spin during detach** — Added 10ms sleep in the poll loop during module unload. Without sleep, `io_context::poll()` returns immediately when no I/O is ready, causing the loop to spin at 100% CPU for up to 5 seconds.

---

## [1.3.6-ktp] - 2026-03-24

### Fixed
- **`curl_global_cleanup` added to `OnAmxxDetach`** — `curl_global_init` was called on attach but `curl_global_cleanup` was never called on detach. On a long-running server with frequent map changes, the unpaired init/cleanup calls leaked SSL/OpenSSL state and OS resolver threads.
- **`curl_formadd` params array bounds check** — The `CURLFORM_END` sentinel scan had no upper bound on the params array index. Malformed plugin calls without a terminating `CURLFORM_END` could read past the end of the params array. Now checks against the actual param count.
- **`OnAmxxDetach` timeout uses wall-clock** — The interrupt-and-drain loop used an iteration counter (~5000 polls) as a proxy for 5 seconds, but `io_context_.poll()` returns immediately when no I/O is ready, making the counter exhaust in microseconds. Now uses `std::chrono::steady_clock` for a real 5-second wall-clock deadline.
- **`CurlReset` re-binds WriteCallback** — `curl_easy_reset` removes all options including `CURLOPT_WRITEFUNCTION`. After reset, the auto-buffering WriteCallback (needed for `curl_get_response_body`) was lost. Now re-binds it after every reset.

---

## [1.3.5-ktp] - 2026-03-14

### Async Safety + POSTFIELDS Fix

**Fixed:**
- **`CURLOPT_POSTFIELDS` used stale pointer during async perform** — `MF_GetAmxString` returns a pointer to a static internal buffer that gets overwritten on the next call. For async `Perform`, libcurl reads the POST data later when the buffer is stale, sending corrupted or unrelated data. Now auto-upgrades `CURLOPT_POSTFIELDS` to `CURLOPT_COPYPOSTFIELDS`, which makes libcurl copy the data immediately.
- **`RemoveAllTasks` left handles attached to curl_multi** — `curl_easy_cleanup` ran while handles were still in the multi, which is undefined behavior per libcurl docs. Now removes all in-flight handles from curl_multi before destroying them.
- **IOCTL interrupt code incorrect** — `CURLIOE_UNKNOWNCMD` tells libcurl the command is unknown; `CURLIOE_FAILRESTART` correctly signals a failed restart, which triggers proper abort handling.
- **`curl_multi_add_handle` failures silent** — Return code was unchecked. Now logs error and cleans up the curl map entry on failure.
- **`curl_formadd` static aliasing risk** — Changed from `static char[14][16384]` to heap allocation (`new`/`delete`). Static storage with `CURLFORM_PTRCONTENTS` could alias across concurrent calls; heap allocation ensures each invocation gets its own buffers.

---

## [1.3.4-ktp] - 2026-03-12

### In-Flight Callback Safety

**Fixed:**
- **Segfault from stale AMX in mid-transfer callbacks** — `WriteCallback`, `HeaderCallback`, `ReadCallback`, and all other libcurl callbacks called `MF_ExecuteForward` without checking if the plugin was still loaded. If a map change unloaded a plugin during a slow HTTP response, the next callback would dereference a stale AMX pointer. All 10 callback methods now check `IsAmxValid()` before calling into Pawn, aborting the transfer cleanly via the interrupt mechanism if the plugin is gone.
- **Move constructor omitted `is_transfer_in_progress_`** — The `AmxCurl` move constructor did not copy `is_transfer_in_progress_`, leaving it uninitialized (undefined behavior). The primary constructor set it to `false`, but after move-construction into the handle map, the value was garbage. This could cause `IsAllTransfersCompleted()` to return false indefinitely, hanging `OnAmxxDetach`. Now properly copied in the move initializer list.
- **`OnAmxxDetach` spin-wait could hang indefinitely** — The detach cleanup loop polled `IsAllTransfersCompleted()` without first interrupting in-flight transfers. A stuck or slow transfer (DNS timeout, hung upstream) would block server shutdown forever. Now calls `TryInterruptAllTransfers()` before the loop, with a 5000-poll timeout as a safety bound.
- **`RemoveTask` on in-flight handle caused use-after-free** — `curl_easy_cleanup` from Pawn during an active transfer destroyed the `AmxCurl` object while libcurl still held a reference to the easy handle. The next `CheckMultiInfo` call would fire the completion callback on freed memory. Now checks `is_transfer_in_progress` and interrupts instead of destroying, deferring cleanup to the completion callback.
- **Destructor called `MF_UnregisterSPForward` on stale forwards** — `~CurlCallbackAmx` unconditionally unregistered all Pawn forwards, but after plugin unload the forward IDs reference freed function tables. Now checks `IsAmxValid()` first — if invalid, clears the map without calling AMXX.
- **Response body grew unbounded** — Auto-buffered response bodies (`response_body_`) had no size limit. A misbehaving endpoint returning megabytes would accumulate it all in heap memory. Now capped at 64KB — sufficient for Discord API responses while preventing memory exhaustion.
- **`curl_formadd` 224KB stack allocation** — `char strings[14][16384]` allocated 224KB on the stack. Changed to `static` storage to move out of the stack frame while keeping the correct 16384 buffer size (matching KTPAMXX's `MAX_BUFFER_LENGTH`).
- **Deferred cleanup for in-flight handles** — `RemoveTask` now marks handles with `cleanup_deferred_` instead of silently leaking them. `SweepDeferredCleanups()` runs each frame and erases completed deferred handles, preventing unbounded growth of `amx_curl_` on long-running servers.
- **Detach timeout warning logged actual poll count** — Previously printed hardcoded `0` instead of the actual poll count reached.

---

## [1.3.3-ktp] - 2026-03-10

### Stale AMX Pointer Validation

**Fixed:**
- **Segfault when plugin unloaded during async transfer** -- When an async HTTP request completes (`OnPerformComplete`), the module calls `MF_RegisterSPForward(amx, func)` to invoke the Pawn callback. If the plugin that started the request was unloaded during the async operation (e.g., during a map change), the stored AMX pointer is stale and `amx->base` points to freed memory, causing a segfault in `amx_GetPublic`. Now validates the AMX pointer via `MF_FindScriptByAmx()` before registering the forward. If the plugin is no longer loaded, the callback is skipped with a warning logged to the server console.

---

## [1.3.2-ktp] - 2026-02-25

### Auto-Buffering Fix

**Fixed:**
- **`curl_get_response_body()` always returned empty string** — The v1.3.0 auto-buffering feature was broken because `CURLOPT_WRITEFUNCTION` was never bound to the curl handle by default. The C++ `WriteCallback` (which buffers into `response_body_`) only fires if explicitly installed on the handle via `BindCallback()`. Without a Pawn `WRITEFUNCTION` set, `BindCallback` was never called, so libcurl used its default writer (stdout), bypassing the C++ callback entirely. Fixed by calling `BindCallback(CURLOPT_WRITEFUNCTION)` in `Curl::InitCurl()` so the C++ WriteCallback is always installed. Discovered via Discord embed message IDs never being captured (empty response body → `DISCORD_MSG_ID_NOT_FOUND` → all embed updates skipped with `no_msg_id`).

---

## [1.3.1-ktp] - 2026-02-25

### Bug Fixes

**Fixed:**
- **`curl_easy_unescape` native called escape instead of unescape** — Copy-paste bug in `curl_natives.cc`: `amx_curl_easy_unescape` called `manager.CurlEscapeUrl()` instead of `manager.CurlUnescapeUrl()`. The unescape native was actually escaping URLs.
- **Server crash when callback function not found** — `curl_easy_perform` only caught `CurlAmxManagerInvalidHandleException` but not `CurlTaskCallbackNotFoundException`. If a plugin passed a non-existent callback function name, the unhandled exception crashed the server and leaked the `data[]` array.
- **Potential infinite loop on module detach** — `AmxCurl` constructor didn't initialize `is_transfer_in_progress_`, leaving it as garbage. `OnAmxxDetach()` loops `while(!IsAllTransfersCompleted())` — an uninitialized `true` value would cause an infinite hang on server shutdown or map change. All `AmxCurl` members now properly initialized.
- **Corrupted error messages in `BindCallback`** — `"failture with code " + code` performed pointer arithmetic on the string literal instead of string concatenation. For `CURLcode` values >= 20, this read past the null terminator (undefined behavior). Replaced with `curl_easy_strerror(code)` for proper human-readable error messages.

---

## [1.3.0-ktp] - 2026-02-23

### Built-in Response Body Capture

**Added:**
- **`curl_get_response_body()` native** - Retrieves the response body captured during an async transfer. When no `CURLOPT_WRITEFUNCTION` Pawn callback is set, the module automatically buffers response data in C++ (`std::string`). Call this native in your completion callback before `curl_easy_cleanup` to read the captured body.

**Why:**
- The Pawn-level `WRITEFUNCTION` callback path (`MF_ExecuteForward` → `amx_Allot`) can fail silently under memory pressure, writing response data to an uninitialized pointer and corrupting the heap — causing segfaults traced to `discord_curl_write` on production servers (ATL3, ATL4, NY1). Capturing in C++ eliminates the dangerous Pawn callback entirely.

**Technical Details:**
- `CurlCallbackAmx::WriteCallback()` now appends to `response_body_` when no Pawn callback is registered (previously discarded data)
- `CurlCallbackAmx::ResetAmxCallbacks()` clears the response body buffer
- New `AmxCurlManager::CurlGetResponseBody()` accessor method
- Native copies response body to Pawn buffer via `MF_SetAmxString`

---

## [1.2.1-ktp] - 2026-01-31

### Forward Registration Validation & Diagnostics

**Fixed:**
- **Silent callback registration failures** - Forward registration could fail silently, causing curl to abort transfers
  - `MF_RegisterSPForwardByName` returns -1 when function not found, but this was stored anyway
  - Invalid forward IDs caused `MF_ExecuteForward` to return 0
  - Curl interpreted 0 from WriteCallback as "abort transfer", preventing completion callback from firing
  - Result: Discord embeds created but no response captured, HLTV recording commands not processed

**Added:**
- **Forward registration validation** - Now checks return value and logs error if registration fails
- **Detailed callback logging** - Logs successful registrations with forward ID and option type
- **WriteCallback diagnostics** - Logs if forward ID is invalid or if callback returns unexpected value
- **Graceful fallback** - If write callback registration fails, accepts data silently instead of aborting

**Technical Details:**
- `SetupAmxCallback()` now validates forward ID before storing
- `WriteCallback()` double-checks forward ID validity before execution
- Error messages printed to server console with `[CURL]` prefix for easy grep

---

## [1.2.0-ktp] - 2026-01-10

### Critical Segfault Fixes

**Fixed:**
- **Use-after-free in async socket callbacks** - Raw `SocketData*` pointers passed to ASIO async callbacks could be deleted before callback executed
  - Changed to `shared_ptr<SocketData>` with `socket_data_map_` tracking
- **Handle allocation bug** - `count() > 1` always false (count returns 0 or 1), causing handle collisions
  - Fixed to `count() != 0`
- **Stale socket map entries** - Non-ARES sockets weren't erased from `socket_map_` on CURL_POLL_REMOVE
  - Now always erases from socket_map_ regardless of socket type
- **Unvalidated callback execution** - `MF_ExecuteForward` called without checking callback registration
  - Added `.count()` validation before all 10 callback functions

**Changed:**
- `curl_multi_class.h` - Added `SocketDataPtr` typedef and `socket_data_map_` for lifetime management
- `curl_multi_class.cc` - Refactored socket handling to use shared_ptr
- `amx_curl_callback_class.cc` - All callback functions now validate registration before execution
- `amx_curl_manager_class.h` - Fixed handle allocation loop condition

---

## [1.1.1-ktp] - 2025-12-04

### KTP Fork - KTPAMXX Integration

**Breaking Changes:**
- **Requires KTPAMXX** - Standard AMX Mod X not supported (module loads but async transfers won't process)

**Removed:**
- Metamod dependency - No longer requires Metamod to run

**Added:**
- KTPAMXX frame callback integration via `MF_RegModuleFrameFunc()` API
- Console logging for module load events
- Graceful fallback when frame callback API not available

**Changed:**
- `callbacks.cc` - Replaced Metamod StartFrame hook with KTPAMXX frame callback
- `moduleconfig.h` - Disabled USE_METAMOD, updated branding to KTP
- Binary renamed to `amxxcurl_ktp_i386` (from `amxxcurl_amxx_i386`)

**Technical Details:**
- Frame callbacks registered in `OnAmxxAttach()`, unregistered in `OnAmxxDetach()`
- `CurlFrameCallback()` processes pending curl_multi transfers each server frame
- All native functions unchanged - full API compatibility with original AmxxCurl

---

## Upstream Releases (Polarhigh/AmxxCurl)

### [1.1.1] - Upstream

- Fixed error "Failed to send data to host"
- Linux issues fixed

### [1.1.0] - Upstream

- Replaced threading with curl_multi interface + ASIO polling
- Non-blocking transfers without spawning threads
- Improved stability and performance

### [1.0.x] - Upstream

- Full libcurl easy interface wrapper
- SSL/TLS support via OpenSSL
- Callback support (write, read, progress, header, debug)
- URL encoding/decoding
- slist support for custom headers
- Cross-platform Windows/Linux support

---

## Credits

**KTP Fork:**
- **Nein_** ([@afraznein](https://github.com/afraznein)) - KTPAMXX integration, Metamod removal

**Upstream AmxxCurl:**
- **Polarhigh** (Igor Minin) - Original module development
