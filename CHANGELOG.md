# Changelog

All notable changes to KTP CURL AMXX will be documented in this file.

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
