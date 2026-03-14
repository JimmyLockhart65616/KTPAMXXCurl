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
    AmxCurl(AMX* amx, CurlMulti& curl_multi) :
        amx_(amx),
        curl_callback_(std::make_shared<CurlCallbackAmx>(amx)),
        curl_(curl_callback_),
        curl_multi_(curl_multi),
        amx_callback_fun_(0),
        task_handle_(0),
        amx_callback_data_(nullptr),
        amx_callback_data_len_(0),
        is_transfer_in_progress_(false),
        cleanup_deferred_(false)
    { }

    AmxCurl(AmxCurl&& other) :
        amx_(other.amx_),
        curl_callback_(other.curl_callback_),
        curl_(std::move(other.curl_)),
        amx_callback_fun_(other.amx_callback_fun_),
        task_handle_(other.task_handle_),
        amx_callback_data_(other.amx_callback_data_),
        amx_callback_data_len_(other.amx_callback_data_len_),
        curl_multi_(other.curl_multi_),
        is_transfer_in_progress_(other.is_transfer_in_progress_),
        cleanup_deferred_(other.cleanup_deferred_)
    { }

    void Perform(const char* complete_callback, int task_handle, cell* data, int data_len)
    {
        if (MF_AmxFindPublic(amx_, complete_callback, &amx_callback_fun_) != AMX_ERR_NONE)
        {
            throw CurlTaskCallbackNotFoundException();
        }

        amx_callback_data_ = data;
        amx_callback_data_len_ = data_len;
        task_handle_ = task_handle;
        is_transfer_in_progress_ = true;

        CurlMulti::CurlPerformComplete callback = std::bind(&AmxCurl::OnPerformComplete, this, std::placeholders::_1);
        curl_multi_.AddCurl(curl_, std::move(callback));
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

        cell* cb_data = amx_callback_data_;
        int cb_data_len = amx_callback_data_len_;
        int cb_id = amx_callback_fun_;
        int task_handle = task_handle_;

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
