#include <curl/curl.h>
#include <chrono>
#include <thread>
#include <atomic>
#include "sdk/amxxmodule.h"
#include "amx_curl_callback_class.h"
#include "amx_curl_controller_class.h"
#include "asio_poller.h"

extern AMX_NATIVE_INFO g_amx_curl_natives[];

// See declaration in amx_curl_callback_class.h. Set after the in-flight
// drain loop exits but BEFORE RemoveAllTasks (1.3.10 ordering) so every
// destructor fired during teardown — including any shared_ptr escapees in
// late asio handlers — atomically sees detached=true and skips MF_*.
std::atomic<bool> g_amxxcurl_detached{false};

// KTP: Frame callback for async cURL processing
// This replaces Metamod's StartFrame callback
void CurlFrameCallback()
{
    AmxCurlController::Instance().get_asio_poller().Poll();
    AmxCurlController::Instance().get_curl_manager().SweepDeferredCleanups();
}

// amxmodx

void OnAmxxAttach()
{
    CURLcode res = curl_global_init(CURL_GLOBAL_ALL);
    if(res != CURLE_OK)
    {
        MF_PrintSrvConsole("[CURL] Cannot init curl: %s\n", curl_easy_strerror(res));
        return;
    }

    MF_AddNatives(g_amx_curl_natives);

    // KTP: Register frame callback for async processing (KTPAMXX only)
    if (MF_RegModuleFrameFunc)
    {
        MF_RegModuleFrameFunc(CurlFrameCallback);
        MF_PrintSrvConsole("[CURL] Module loaded (extension mode, using frame callbacks)\n");
    }
    else
    {
        MF_PrintSrvConsole("[CURL] Module loaded\n");
    }
}

void OnAmxxDetach()
{
    // KTP: Unregister frame callback
    if (MF_UnregModuleFrameFunc)
        MF_UnregModuleFrameFunc(CurlFrameCallback);

    // Interrupt all in-flight transfers and wait for completion with timeout
    // (Replaces Metamod's ServerDeactivate callback)
    AmxCurlManager& manager = AmxCurlController::Instance().get_curl_manager();

    manager.TryInterruptAllTransfers();

    // Wall-clock timeout: poll until all transfers complete or 5 seconds elapse
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while(!manager.IsAllTransfersCompleted() && std::chrono::steady_clock::now() < deadline)
    {
        AmxCurlController::Instance().get_asio_poller().Poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));  // KTP: Avoid busy-spin during shutdown
    }

    if (!manager.IsAllTransfersCompleted())
        MF_PrintSrvConsole("[CURL] WARNING: Detach timeout after 5s, forcing cleanup\n");

    // Set the detach flag BEFORE RemoveAllTasks so every ~CurlCallbackAmx()
    // fired during teardown atomically sees detached=true and takes the
    // no-MF_* path. This also covers the previously-uncovered window where
    // a shared_ptr<CurlCallbackAmx> could escape RemoveAllTasks via a late
    // asio handler that captured one — its destructor would have observed
    // detached=false in 1.3.9 and dereferenced g_fn_FindAmxScriptByAmx after
    // KTPAMXX core was already unmapped.
    //
    // The drain loop above lets legitimate in-flight transfers complete with
    // valid MF_* function pointers; only after the loop returns is it safe
    // to switch into the no-op path. Skipping MF_UnregisterSPForward during
    // teardown is correct — KTPAMXX is about to free its forward table.
    //
    // Fixes shutdown SIGSEGV at module offset 0x965d6 (CurlCallbackAmx
    // destructor's MF_FindScriptByAmx call). See
    // docs/INVESTIGATION_shutdown_race_2026-05-04.md.
    g_amxxcurl_detached.store(true, std::memory_order_release);

    manager.RemoveAllTasks();

    // Tear the CURLM* down BEFORE curl_global_cleanup(): libcurl requires all
    // multi handles cleaned up first. The frame callback is already unregistered
    // and the drain loop has finished, so nothing touches the multi after this.
    // Otherwise the ~CurlMulti destructor cascade (during dlclose) runs
    // curl_multi_cleanup after global teardown — UB (benign only on the currently
    // vendored curl/OpenSSL, one bump from the CHI1 shutdown-crash class).
    manager.get_curl_multi().Shutdown();

    curl_global_cleanup();
}
