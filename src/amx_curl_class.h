#ifndef _AMX_CURL_CLASS_H_
#define _AMX_CURL_CLASS_H_

#include <functional>
#include "curl_class.h"
#include "amx_curl_callback_class.h"

class CurlTaskCallbackNotFoundException : std::exception
{ };

class AmxCurl
{
    using AmxCallback = int;

public:
    // Init lists follow declaration order (members initialize in that order
    // regardless of what the list says — mismatching it just invites -Wreorder
    // and, the day an initializer reads another member, a real bug).
    AmxCurl(AMX* amx, CurlMulti& curl_multi) :
        amx_(amx),
        curl_multi_(curl_multi),
        curl_callback_(std::make_shared<CurlCallbackAmx>(amx)),
        curl_(curl_callback_),
        amx_callback_fun_(0),
        task_handle_(0),
        amx_callback_data_(nullptr),
        amx_callback_data_len_(0),
        is_transfer_in_progress_(false),
        cleanup_deferred_(false)
    { }

    AmxCurl(AmxCurl&& other) :
        amx_(other.amx_),
        curl_multi_(other.curl_multi_),
        curl_callback_(other.curl_callback_),
        curl_(std::move(other.curl_)),
        amx_callback_fun_(other.amx_callback_fun_),
        task_handle_(other.task_handle_),
        amx_callback_data_(other.amx_callback_data_),
        amx_callback_data_len_(other.amx_callback_data_len_),
        is_transfer_in_progress_(other.is_transfer_in_progress_),
        cleanup_deferred_(other.cleanup_deferred_)
    {
        // We now own the buffer — the moved-from object must not free it too.
        other.amx_callback_data_ = nullptr;
        other.amx_callback_data_len_ = 0;
    }

    // amx_callback_data_ is a new[]-allocated cell array handed over by
    // curl_perform. OnPerformComplete frees it on the normal path, but a task
    // destroyed while still in flight (RemoveAllTasks at shutdown/detach) never
    // reaches that path and leaked the buffer. Exit-only today, but the manager
    // is free to drop tasks at any time, so own it properly.
    ~AmxCurl()
    {
        delete[] amx_callback_data_;
    }

    void Perform(const char* complete_callback, int task_handle, cell* data, int data_len)
    {
        if (MF_AmxFindPublic(amx_, complete_callback, &amx_callback_fun_) != AMX_ERR_NONE)
        {
            throw CurlTaskCallbackNotFoundException();
        }

        // A re-Perform before the previous completion would strand the old buffer.
        delete[] amx_callback_data_;
        amx_callback_data_ = data;
        amx_callback_data_len_ = data_len;
        task_handle_ = task_handle;
        is_transfer_in_progress_ = true;

        // Drop the previous transfer's auto-buffered body HERE, not in the
        // completion path -- it must survive into curl_get_response_body.
        curl_callback_->ClearResponseBody();

        CurlMulti::CurlPerformComplete callback = std::bind(&AmxCurl::OnPerformComplete, this, std::placeholders::_1);
        if (curl_multi_.AddCurl(curl_, std::move(callback)) != CURLM_OK)
        {
            // AddCurl (OOM-class curl_multi_add_handle failure) already erased the
            // completion callback, so OnPerformComplete can never fire to clear
            // these. Undo the in-progress state here or the handle is a permanent
            // zombie: IsAllTransfersCompleted() stays false forever (every shutdown
            // drain burns the full 5s) and amx_callback_data_ leaks.
            is_transfer_in_progress_ = false;
            delete[] amx_callback_data_;
            amx_callback_data_ = nullptr;
            amx_callback_data_len_ = 0;
        }
    }

    bool get_is_transfer_in_progress() { return is_transfer_in_progress_; }
    bool get_cleanup_deferred() { return cleanup_deferred_; }
    void set_cleanup_deferred() { cleanup_deferred_ = true; }
    Curl& get_curl() { return curl_; }

    CurlCallbackAmx& get_curl_callback_amx() const { return *curl_callback_; }
    
private:
    void OnPerformComplete(CURLcode result)
    {
        curl_multi_.RemoveCurl(curl_);

        is_transfer_in_progress_ = false;

        // Take ownership out of the member: everything below deletes cb_data on
        // every path, so the member must not also free it in ~AmxCurl.
        cell* cb_data = amx_callback_data_;
        int cb_data_len = amx_callback_data_len_;
        amx_callback_data_ = nullptr;
        amx_callback_data_len_ = 0;

        int cb_id = amx_callback_fun_;
        int task_handle = task_handle_;

        // Short-circuit if the module has detached — MF_FindScriptByAmx
        // itself is an indirect call through a function pointer that goes
        // stale once KTPAMXX core is unmapped during shutdown. Drop the
        // request silently; we cannot even print a warning, since
        // MF_PrintSrvConsole is also stale at this point.
        if (g_amxxcurl_detached.load(std::memory_order_acquire))
        {
            if (cb_data != nullptr)
                delete[] cb_data;
            return;
        }

        // Validate AMX pointer before calling registerSPForward.
        // If the plugin that started this async request was unloaded (e.g., during
        // a map change), amx_ is stale — amx->base points to freed memory, and
        // MF_RegisterSPForward would segfault in amx_GetPublic.
        // MF_FindScriptByAmx does a safe pointer comparison (no dereference) against
        // the loaded scripts list, returning -1 if the AMX is no longer valid.
        if (MF_FindScriptByAmx(amx_) == -1)
        {
            MF_PrintSrvConsole("[CURL] WARNING: Plugin unloaded during async transfer (AMX %p no longer valid) - skipping completion callback\n", amx_);
            if (cb_data != nullptr)
                delete[] cb_data;
            return;
        }

        int forward_id;
        if (cb_data != nullptr)
        {
            forward_id = MF_RegisterSPForward(amx_, cb_id, FP_CELL /* handle */, FP_CELL /* CURLcode */, FP_ARRAY /* data */, FP_DONE);
            MF_ExecuteForward(forward_id, task_handle, result, MF_PrepareCellArray(cb_data, cb_data_len));
            delete[] cb_data;
        }
        else
        {
            forward_id = MF_RegisterSPForward(amx_, cb_id, FP_CELL /* handle */, FP_CELL /* CURLcode */, FP_DONE);
            MF_ExecuteForward(forward_id, task_handle, result);
        }

        MF_UnregisterSPForward(forward_id);
    }

    AMX* amx_;
    CurlMulti& curl_multi_;
    std::shared_ptr<CurlCallbackAmx> curl_callback_;
    Curl curl_;

    AmxCallback amx_callback_fun_;
    int task_handle_;
    cell* amx_callback_data_;
    int amx_callback_data_len_;

    bool is_transfer_in_progress_;
    bool cleanup_deferred_;
};

#endif // _AMX_CURL_CLASS_H_
