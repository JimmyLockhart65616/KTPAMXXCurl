#include "amx_curl_callback_class.h"
#include "curl_utils_class.h"
#include <stdexcept>

CurlCallbackAmx::CurlCallbackAmx(AMX* amx) :
    amx_(amx),
    interrupt_(false)
{ }

CurlCallbackAmx::~CurlCallbackAmx()
{
    // Only unregister forwards if the AMX is still valid — after plugin unload
    // the forward IDs reference freed function tables
    if (IsAmxValid())
        ResetAmxCallbacks();
    else
        registered_callbacks_.clear();

    response_body_.clear();
}

void CurlCallbackAmx::SetData(CURLoption data_option, void* data)
{
    data_[data_option] = data;
}

void CurlCallbackAmx::TryInterrupt()
{
    interrupt_ = true;
}

void CurlCallbackAmx::ResetAmxCallbacks()
{
    std::for_each(registered_callbacks_.begin(), registered_callbacks_.end(), [](std::pair<const CURLoption, AmxForward>& pair) { MF_UnregisterSPForward(pair.second); });
    registered_callbacks_.clear();
    response_body_.clear();
    interrupt_ = false;
}

void CurlCallbackAmx::SetupAmxCallback(CURLoption callback_option, const char* amx_function_name)
{
    if (registered_callbacks_.count(callback_option) > 0)
        MF_UnregisterSPForward(registered_callbacks_[callback_option]);

    AmxForward forwardId = -1;

    switch (callback_option)
    {
    case CURLOPT_WRITEFUNCTION:
        // char *ptr, size_t size, size_t nmemb, void *userdata
        forwardId = MF_RegisterSPForwardByName(amx_, amx_function_name, FP_ARRAY, FP_CELL, FP_CELL, FP_CELL, FP_DONE);
        break;

    case CURLOPT_READFUNCTION:
        // char *buffer, size_t size, size_t nitems, void *instream
        forwardId = MF_RegisterSPForwardByName(amx_, amx_function_name, FP_ARRAY, FP_CELL, FP_CELL, FP_CELL, FP_DONE);
        break;

    case CURLOPT_IOCTLFUNCTION:
        // CURL *handle, int cmd, void *clientp
        forwardId = MF_RegisterSPForwardByName(amx_, amx_function_name, FP_CELL, FP_CELL, FP_CELL, FP_DONE);
        break;

    case CURLOPT_SEEKFUNCTION:
        // void *userp, curl_off_t offset, int origin
        forwardId = MF_RegisterSPForwardByName(amx_, amx_function_name, FP_CELL, FP_CELL /* high offset*/, FP_CELL /* low offset */, FP_CELL, FP_DONE);
        break;

    case CURLOPT_SOCKOPTFUNCTION:
        // void *clientp, curl_socket_t curlfd, curlsocktype purpose
        forwardId = MF_RegisterSPForwardByName(amx_, amx_function_name, FP_CELL, FP_CELL, FP_CELL, FP_DONE);
        break;

    case CURLOPT_PROGRESSFUNCTION:
        // void *clientp, double dltotal, double dlnow, double ultotal, double ulnow
        forwardId = MF_RegisterSPForwardByName(amx_, amx_function_name, FP_CELL, FP_FLOAT, FP_FLOAT, FP_FLOAT, FP_FLOAT, FP_DONE);
        break;

    case CURLOPT_HEADERFUNCTION:
        // char *buffer, size_t size, size_t nitems, void *userdata
        forwardId = MF_RegisterSPForwardByName(amx_, amx_function_name, FP_ARRAY, FP_CELL, FP_CELL, FP_CELL, FP_DONE);
        break;

    case CURLOPT_DEBUGFUNCTION:
        // CURL *handle, curl_infotype type, char *data, size_t size, void *userptr
        forwardId = MF_RegisterSPForwardByName(amx_, amx_function_name, FP_CELL, FP_CELL, FP_ARRAY, FP_CELL, FP_CELL, FP_DONE);
        break;

    case CURLOPT_SSL_CTX_FUNCTION:
        // CURL *curl, void *ssl_ctx, void *userptr
        forwardId = MF_RegisterSPForwardByName(amx_, amx_function_name, FP_CELL, FP_CELL, FP_CELL, FP_DONE);
        break;

    case CURLOPT_INTERLEAVEFUNCTION:
        // void *ptr, size_t size, size_t nmemb, void *userdata
        forwardId = MF_RegisterSPForwardByName(amx_, amx_function_name, FP_ARRAY, FP_CELL, FP_CELL, FP_CELL, FP_DONE);
        break;

    default:
        throw std::runtime_error("Unsupported option");
    }

    // Validate forward registration succeeded
    if (forwardId == -1)
    {
        MF_PrintSrvConsole("[CURL] ERROR: Failed to register callback '%s' - function not found in plugin\n", amx_function_name);
        // Don't store invalid forward - leave callback unregistered so we fall back gracefully
        return;
    }

    MF_PrintSrvConsole("[CURL] Registered callback '%s' (forward=%d, option=%d)\n", amx_function_name, forwardId, callback_option);
    registered_callbacks_[callback_option] = forwardId;
}


// protected

size_t CurlCallbackAmx::WriteCallback(char* ptr, size_t size, size_t nmemb)
{
    if (interrupt_)
    {
        return CurlUtils::GetInterruptCodeForCurlCallback(CURLOPT_WRITEFUNCTION);
    }

    // No Pawn callback registered — buffer response body in C++ automatically
    if (registered_callbacks_.count(CURLOPT_WRITEFUNCTION) == 0)
    {
        // Cap response body at 64KB to prevent unbounded memory growth
        static const size_t MAX_RESPONSE_BODY = 65536;
        size_t incoming = size * nmemb;
        if (response_body_.size() + incoming > MAX_RESPONSE_BODY)
        {
            // Accept remaining capacity, then stop buffering
            size_t remaining = (response_body_.size() < MAX_RESPONSE_BODY)
                ? MAX_RESPONSE_BODY - response_body_.size() : 0;
            if (remaining > 0)
            {
                response_body_.append(ptr, remaining);
                MF_PrintSrvConsole("[CURL] WARNING: Response body reached %zuB cap — truncating further data\n", MAX_RESPONSE_BODY);
            }
            // Still return full size to not abort the transfer
            return incoming;
        }
        response_body_.append(ptr, incoming);
        return incoming;
    }

    // Validate AMX before calling into Pawn — plugin may have been unloaded mid-transfer
    if (!IsAmxValid())
    {
        interrupt_ = true;
        return CurlUtils::GetInterruptCodeForCurlCallback(CURLOPT_WRITEFUNCTION);
    }

    AmxForward forwardId = registered_callbacks_[CURLOPT_WRITEFUNCTION];

    // Double-check forward ID is valid (should never be -1 if properly registered)
    if (forwardId < 0)
    {
        MF_PrintSrvConsole("[CURL] ERROR: WriteCallback has invalid forward ID %d - accepting data to continue transfer\n", forwardId);
        return size * nmemb;
    }

    // char *ptr, size_t size, size_t nmemb, void *userdata
    void* userData = data_.count(CURLOPT_WRITEDATA) ? data_[CURLOPT_WRITEDATA] : nullptr;
    cell result = MF_ExecuteForward(forwardId, MF_PrepareCharArray(ptr, size * nmemb), size, nmemb, userData);

    // Log if callback returned unexpected value (could indicate plugin error)
    size_t expected = size * nmemb;
    if (result != static_cast<cell>(expected) && result != 0)
    {
        MF_PrintSrvConsole("[CURL] WriteCallback returned %d (expected %zu) - transfer may be affected\n", result, expected);
    }

    return static_cast<size_t>(result);
}

size_t CurlCallbackAmx::ReadCallback(char* buffer, size_t size, size_t nitems)
{
    if (interrupt_)
    {
        return CurlUtils::GetInterruptCodeForCurlCallback(CURLOPT_READFUNCTION);
    }

    if (registered_callbacks_.count(CURLOPT_READFUNCTION) == 0)
    {
        return 0;  // No data to read
    }

    if (!IsAmxValid())
    {
        interrupt_ = true;
        return CurlUtils::GetInterruptCodeForCurlCallback(CURLOPT_READFUNCTION);
    }

    void* userData = data_.count(CURLOPT_READDATA) ? data_[CURLOPT_READDATA] : nullptr;
    return MF_ExecuteForward(registered_callbacks_[CURLOPT_READFUNCTION], MF_PrepareCharArrayA(buffer, size * nitems, true), size, nitems, userData);
}

curlioerr CurlCallbackAmx::IoctlCallback(CURL* handle, int cmd)
{
    if (interrupt_)
    {
        return static_cast<curlioerr>(CurlUtils::GetInterruptCodeForCurlCallback(CURLOPT_IOCTLFUNCTION));
    }

    if (registered_callbacks_.count(CURLOPT_IOCTLFUNCTION) == 0)
    {
        return CURLIOE_OK;
    }

    if (!IsAmxValid())
    {
        interrupt_ = true;
        return CURLIOE_FAILRESTART;
    }

    void* userData = data_.count(CURLOPT_IOCTLDATA) ? data_[CURLOPT_IOCTLDATA] : nullptr;
    return static_cast<curlioerr>(MF_ExecuteForward(registered_callbacks_[CURLOPT_IOCTLFUNCTION], handle, cmd, userData));
}

int CurlCallbackAmx::SeekCallback(curl_off_t offset, int origin)
{
    if (interrupt_)
    {
        return CurlUtils::GetInterruptCodeForCurlCallback(CURLOPT_SEEKFUNCTION);
    }

    if (registered_callbacks_.count(CURLOPT_SEEKFUNCTION) == 0)
    {
        return CURL_SEEKFUNC_CANTSEEK;
    }

    if (!IsAmxValid())
    {
        interrupt_ = true;
        return CURL_SEEKFUNC_FAIL;
    }

    void* userData = data_.count(CURLOPT_SEEKDATA) ? data_[CURLOPT_SEEKDATA] : nullptr;
    return MF_ExecuteForward(registered_callbacks_[CURLOPT_SEEKFUNCTION], userData, offset >> 32, static_cast<int>(offset), origin);
}

int CurlCallbackAmx::SockoptCallback(curl_socket_t curlfd, curlsocktype purpose)
{
    if (interrupt_)
    {
        return CurlUtils::GetInterruptCodeForCurlCallback(CURLOPT_SOCKOPTFUNCTION);
    }

    if (registered_callbacks_.count(CURLOPT_SOCKOPTFUNCTION) == 0)
    {
        return CURL_SOCKOPT_OK;
    }

    if (!IsAmxValid())
    {
        interrupt_ = true;
        return CURL_SOCKOPT_ERROR;
    }

    void* userData = data_.count(CURLOPT_SOCKOPTDATA) ? data_[CURLOPT_SOCKOPTDATA] : nullptr;
    return MF_ExecuteForward(registered_callbacks_[CURLOPT_SOCKOPTFUNCTION], userData, curlfd, purpose);
}

int CurlCallbackAmx::ProgressCallback(double dltotal, double dlnow, double ultotal, double ulnow)
{
    if (interrupt_)
    {
        return CurlUtils::GetInterruptCodeForCurlCallback(CURLOPT_PROGRESSFUNCTION);
    }

    if (registered_callbacks_.count(CURLOPT_PROGRESSFUNCTION) == 0)
    {
        return 0;  // Continue transfer
    }

    if (!IsAmxValid())
    {
        interrupt_ = true;
        return 1;  // Non-zero aborts transfer
    }

    void* userData = data_.count(CURLOPT_PROGRESSDATA) ? data_[CURLOPT_PROGRESSDATA] : nullptr;
    return MF_ExecuteForward(registered_callbacks_[CURLOPT_PROGRESSFUNCTION], userData, dltotal, dlnow, ultotal, ulnow);
}

size_t CurlCallbackAmx::HeaderCallback(char* buffer, size_t size, size_t nitems)
{
    if (interrupt_)
    {
        return CurlUtils::GetInterruptCodeForCurlCallback(CURLOPT_HEADERFUNCTION);
    }

    if (registered_callbacks_.count(CURLOPT_HEADERFUNCTION) == 0)
    {
        return size * nitems;  // Continue transfer
    }

    if (!IsAmxValid())
    {
        interrupt_ = true;
        return CurlUtils::GetInterruptCodeForCurlCallback(CURLOPT_HEADERFUNCTION);
    }

    void* userData = data_.count(CURLOPT_HEADERDATA) ? data_[CURLOPT_HEADERDATA] : nullptr;
    return MF_ExecuteForward(registered_callbacks_[CURLOPT_HEADERFUNCTION], MF_PrepareCharArray(buffer, size * nitems), size, nitems, userData);
}

int CurlCallbackAmx::DebugCallback(CURL* handle, curl_infotype type, char* debugData, size_t size)
{
    if (interrupt_)
    {
        return CurlUtils::GetInterruptCodeForCurlCallback(CURLOPT_DEBUGFUNCTION);
    }

    if (registered_callbacks_.count(CURLOPT_DEBUGFUNCTION) == 0)
    {
        return 0;
    }

    if (!IsAmxValid())
    {
        interrupt_ = true;
        return 0;
    }

    void* userData = data_.count(CURLOPT_DEBUGDATA) ? data_[CURLOPT_DEBUGDATA] : nullptr;
    return MF_ExecuteForward(registered_callbacks_[CURLOPT_DEBUGFUNCTION], handle, type, MF_PrepareCharArray(debugData, size), size, userData);
}

CURLcode CurlCallbackAmx::SslCtxCallback(CURL* curl, void* ssl_ctx)
{
    if (interrupt_)
    {
        return static_cast<CURLcode>(CurlUtils::GetInterruptCodeForCurlCallback(CURLOPT_SSL_CTX_FUNCTION));
    }

    if (registered_callbacks_.count(CURLOPT_SSL_CTX_FUNCTION) == 0)
    {
        return CURLE_OK;
    }

    if (!IsAmxValid())
    {
        interrupt_ = true;
        return CURLE_ABORTED_BY_CALLBACK;
    }

    void* userData = data_.count(CURLOPT_SSL_CTX_DATA) ? data_[CURLOPT_SSL_CTX_DATA] : nullptr;
    return static_cast<CURLcode>(MF_ExecuteForward(registered_callbacks_[CURLOPT_SSL_CTX_FUNCTION], curl, ssl_ctx, userData));
}

size_t CurlCallbackAmx::InterleaveCallback(void* ptr, size_t size, size_t nmemb)
{
    if (interrupt_)
    {
        return static_cast<size_t>(CurlUtils::GetInterruptCodeForCurlCallback(CURLOPT_INTERLEAVEFUNCTION));
    }

    if (registered_callbacks_.count(CURLOPT_INTERLEAVEFUNCTION) == 0)
    {
        return size * nmemb;  // Continue transfer
    }

    if (!IsAmxValid())
    {
        interrupt_ = true;
        return static_cast<size_t>(CurlUtils::GetInterruptCodeForCurlCallback(CURLOPT_INTERLEAVEFUNCTION));
    }

    void* userData = data_.count(CURLOPT_INTERLEAVEDATA) ? data_[CURLOPT_INTERLEAVEDATA] : nullptr;
    return MF_ExecuteForward(registered_callbacks_[CURLOPT_INTERLEAVEFUNCTION], MF_PrepareCharArray(static_cast<char*>(ptr), size * nmemb), size, nmemb, userData);
}
