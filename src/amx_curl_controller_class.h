#ifndef _AMX_CURL_CONTROLLER_H_
#define _AMX_CURL_CONTROLLER_H_

#include "amx_curl_manager_class.h"
#include "amx_curl_callback_class.h"

// __cxa_atexit against this module's __dso_handle — NOT plain atexit, whose
// NULL dso handle would be skipped by a targeted __cxa_finalize(dlclose).
extern "C" int __cxa_atexit(void (*func)(void*), void* arg, void* dso);
// No extern "C" — GCC's own implicit declaration (emitted for magic-static
// guards) has C++ linkage and a C-linkage redeclaration is a hard conflict.
// Global variables aren't mangled, so this binds the same symbol.
extern void* __dso_handle;

class AmxCurlController
{
public:
    AmxCurlManager& get_curl_manager() { return curl_manager_; }
    AsioPoller& get_asio_poller() { return asio_poller_; }

    static AmxCurlController& Instance()
    {
        static AmxCurlController instance;
        // Registered AFTER instance's destructor, so it runs BEFORE it
        // (atexit entries and static dtors share one LIFO list, at both
        // process exit and dlclose of this module). The engine can unload
        // KTPAMXX without ever calling OnAmxxDetach; the teardown cascade
        // (manager -> tasks -> ~CurlCallbackAmx) would then reach MF_*
        // through pointers into unmapped core. A normal detach sets the
        // flag earlier and this store becomes a no-op.
        static const bool detach_guard = [] {
            int rc = __cxa_atexit(
                [](void*) { g_amxxcurl_detached.store(true, std::memory_order_release); },
                nullptr, &__dso_handle);
            if (rc != 0)
                MF_PrintSrvConsole("[CURL] WARNING: detach-guard atexit registration failed (%d)\n", rc);
            return rc == 0;
        }();
        (void)detach_guard;
        return instance;
    }

private:
    AmxCurlController() :
        curl_manager_(asio_poller_)
    { }

    AmxCurlController(const AmxCurlController& root);
    AmxCurlController& operator=(const AmxCurlController&);

    AmxCurlManager curl_manager_;
    AsioPoller asio_poller_;
};

#endif // _AMX_CURL_CONTROLLER_H_
