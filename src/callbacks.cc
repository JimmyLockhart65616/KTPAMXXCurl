#include <curl/curl.h>
#include "sdk/amxxmodule.h"
#include "amx_curl_controller_class.h"
#include "asio_poller.h"

extern AMX_NATIVE_INFO g_amx_curl_natives[];

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

    int timeout_polls = 0;
    static const int MAX_DETACH_POLLS = 5000;  // ~5 seconds at typical poll rate
    while(!manager.IsAllTransfersCompleted() && timeout_polls < MAX_DETACH_POLLS)
    {
        AmxCurlController::Instance().get_asio_poller().Poll();
        timeout_polls++;
    }

    if (timeout_polls >= MAX_DETACH_POLLS)
        MF_PrintSrvConsole("[CURL] WARNING: Detach timeout after %d polls, forcing cleanup\n", timeout_polls);

    manager.RemoveAllTasks();
}
