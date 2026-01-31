# Changelog

All notable changes to KTP CURL AMXX will be documented in this file.

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
