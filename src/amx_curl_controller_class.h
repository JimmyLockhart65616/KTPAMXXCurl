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
        // process exit and dlclose of this module). Extension-mode engine
        // shutdown never calls OnAmxxDetach (ReleaseEntityDlls dlcloses
        // ktpamx with no module-detach pass), so this handler IS the real
        // teardown: arm the flag so the destructor cascade (manager ->
        // tasks -> ~CurlCallbackAmx) never reaches MF_* through pointers
        // into unmapped core, then detach in-flight easies from the multi
        // while libcurl/asio are still alive — curl_easy_cleanup on a
        // still-attached handle is UB per libcurl docs. When a normal
        // detach DID run, the flag is already set and we must not touch
        // curl again (curl_global_cleanup already happened) — hence the
        // exchange gate.
        static const bool detach_guard = [] {
            int rc = __cxa_atexit(
                [](void*) {
                    if (!g_amxxcurl_detached.exchange(true, std::memory_order_acq_rel))
                        Instance().get_curl_manager().RemoveAllTasks();
                },
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

    // asio_poller_ FIRST: members destruct in reverse declaration order, and
    // ~CurlMulti (inside the manager) still owns asio sockets bound to the
    // poller's io_context — the poller must outlive it. Also makes the ctor's
    // curl_manager_(asio_poller_) reference a fully-constructed object.
    AsioPoller asio_poller_;
    AmxCurlManager curl_manager_;
};

#endif // _AMX_CURL_CONTROLLER_H_
