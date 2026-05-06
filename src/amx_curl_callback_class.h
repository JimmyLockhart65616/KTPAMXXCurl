#ifndef _AMX_CURL_CALLBACK_CLASS_H_
#define _AMX_CURL_CALLBACK_CLASS_H_

#include <unordered_map>
#include <atomic>
#include <algorithm>
#include <string>

#include "sdk/amxxmodule.h"
#include "curl_callback_class.h"

// Set true in OnAmxxDetach AFTER the in-flight transfer drain loop exits
// but BEFORE manager.RemoveAllTasks() (1.3.10 ordering — was post-cleanup
// in 1.3.9 which left a window for shared_ptr<CurlCallbackAmx> escapees).
// After the store, any MF_* function pointer (including MF_FindScriptByAmx)
// may resolve into freed pages — KTPAMXX core can be unmapped before our
// Meyers-singleton destructors finish running. Late ~CurlCallbackAmx() /
// OnPerformComplete() callers must short-circuit before any MF_* call.
extern std::atomic<bool> g_amxxcurl_detached;

class CurlCallbackAmx : public CurlCallback
{
    using AmxForward = int;

    struct CurlOptionHash
    {
        size_t operator()(const CURLoption& key) const
        {
            return static_cast<size_t>(key);
        }
    };

public:
    CurlCallbackAmx(AMX* amx);

    virtual ~CurlCallbackAmx();

    void SetData(CURLoption data_option, void* data);
    void TryInterrupt();
    void ResetAmxCallbacks();
    void SetupAmxCallback(CURLoption callback_option, const char* amx_function_name);

    const std::string& GetResponseBody() const { return response_body_; }
    void ClearResponseBody() { response_body_.clear(); }

    // Check if the AMX pointer is still valid (plugin not unloaded).
    // Short-circuits when the module is detached: MF_FindScriptByAmx is
    // an indirect call through g_fn_FindAmxScriptByAmx, which goes stale
    // once KTPAMXX core is unmapped (see g_amxxcurl_detached comment).
    bool IsAmxValid() const
    {
        if (g_amxxcurl_detached.load(std::memory_order_acquire))
            return false;
        return MF_FindScriptByAmx(amx_) != -1;
    }

protected:
    size_t WriteCallback(char* ptr, size_t size, size_t nmemb) override;
    size_t ReadCallback(char* buffer, size_t size, size_t nitems) override;
    curlioerr IoctlCallback(CURL* handle, int cmd) override;
    int SeekCallback(curl_off_t offset, int origin) override;
    int SockoptCallback(curl_socket_t curlfd, curlsocktype purpose) override;
    int ProgressCallback(double dltotal, double dlnow, double ultotal, double ulnow) override;
    size_t HeaderCallback(char* buffer, size_t size, size_t nitems) override;
    int DebugCallback(CURL* handle, curl_infotype type, char* data, size_t size) override;
    CURLcode SslCtxCallback(CURL* curl, void* ssl_ctx) override;
    size_t InterleaveCallback(void* ptr, size_t size, size_t nmemb) override;

private:
    AMX* amx_;
    std::unordered_map<CURLoption, AmxForward, CurlOptionHash> registered_callbacks_;
    std::unordered_map<CURLoption, void*, CurlOptionHash> data_;
    std::string response_body_;

    std::atomic<bool> interrupt_;
};

#endif // _AMX_CURL_CALLBACK_CLASS_H_
