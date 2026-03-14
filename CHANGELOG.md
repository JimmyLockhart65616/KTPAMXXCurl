# Changelog

All notable changes to KTP CURL AMXX will be documented in this file.

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
