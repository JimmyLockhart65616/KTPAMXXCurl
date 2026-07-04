#include "curl_multi_class.h"
#include "sdk/amxxmodule.h"  // 1.3.11: MF_PrintSrvConsole for the C-callback boundary catch
#include "amx_curl_callback_class.h"  // g_amxxcurl_detached — MF_* is stale once set
#include <functional>

#ifdef AMXXCURL_DEBUG_LOG_ENABLE
#include <cstdio>
#define DEBUG_LOG(f_, ...) printf((f_), ##__VA_ARGS__)
#else
#define DEBUG_LOG(f_, ...) 
#endif

const char* whatstr[] = { "none", "IN", "OUT", "INOUT", "REMOVE" };

int CurlSocketCallbackStatic(CURL* easy, curl_socket_t s, int what, void* userp, void* socketp)
{
    // 1.3.11 belt-and-suspenders (2026-05-13): C-callback boundary. libcurl
    // calls this from its C code; if any exception escapes back into libcurl
    // the runtime hits the unwinder at a foreign-frame boundary, finds no
    // handler, and calls std::terminate(). Even with WrapTcpSocket's
    // throwing-assign closed (1.3.11 primary fix), any future change inside
    // CurlSocketCallback that adds an exception-throwing path would
    // re-introduce the same crash class. Catching here costs ~zero on the
    // happy path (the try block's setup is cheap relative to the function's
    // map/emplace work) and converts any escaping exception into a -1 return
    // value, which libcurl interprets as transfer abort rather than process
    // crash.
    //
    // Thread-safety note (ktp-code-review 2026-05-13): MF_PrintSrvConsole
    // is the AMXX game-thread console function. It's safe in the current
    // single-threaded Poll() model where io_context callbacks run on the
    // main game thread. If we ever move to a worker-thread io_context,
    // these log calls become a data race and need to bounce through a
    // thread-safe log queue.
    try {
        return static_cast<CurlMulti*>(userp)->CurlSocketCallback(easy, s, what, socketp);
    } catch (const std::exception& ex) {
        if (!g_amxxcurl_detached.load(std::memory_order_acquire))
            MF_PrintSrvConsole("[CURL] FATAL ERROR caught at C-callback boundary: %s\n", ex.what());
        return -1;
    } catch (...) {
        if (!g_amxxcurl_detached.load(std::memory_order_acquire))
            MF_PrintSrvConsole("[CURL] FATAL ERROR caught at C-callback boundary: unknown exception\n");
        return -1;
    }
}

int CurlTimerCallbackStatic(CURLM* multi, long timeout_ms, void* userp)
{
    return static_cast<CurlMulti*>(userp)->CurlTimerCallback(multi, timeout_ms);
}

curl_socket_t CurlOpenSocketCallbackStatic(void* clientp, curlsocktype purpose, curl_sockaddr* address)
{
    return static_cast<CurlMulti*>(clientp)->CurlOpenSocketCallback(purpose, address);
}

int CurlCloseSocketCallbackStatic(void* clientp, curl_socket_t item)
{
    return static_cast<CurlMulti*>(clientp)->CurlCloseSocketCallback(item);
}

using namespace std::placeholders;

CurlMulti::CurlMulti(AsioPoller& asio_poller) :
    asio_poller_(asio_poller),
    running_handles_(0)
{
    curl_multi_ = curl_multi_init();
    curl_multi_setopt(curl_multi_, CURLMOPT_SOCKETFUNCTION, &CurlSocketCallbackStatic);
    curl_multi_setopt(curl_multi_, CURLMOPT_SOCKETDATA, this);
    curl_multi_setopt(curl_multi_, CURLMOPT_TIMERFUNCTION, &CurlTimerCallbackStatic);
    curl_multi_setopt(curl_multi_, CURLMOPT_TIMERDATA, this);
}

CurlMulti::~CurlMulti()
{
    curl_multi_cleanup(curl_multi_);
    // Null out so any asio handler that captured `this` and runs after
    // destruction (e.g. a lambda posted from CurlTimerCallback, or a pending
    // async_wait on the asio timer) can early-return instead of dereferencing
    // a freed CURLM*. See CurlTimerCallback and AsioTimerCallback for guards.
    curl_multi_ = nullptr;
}

void CurlMulti::AddCurl(Curl& curl, CurlMulti::CurlPerformComplete&& callback)
{
    // KTP: Set options before inserting into map — if SetOption throws,
    // we don't leave an orphaned handle in curl_map_
    curl.SetOption(CURLOPT_OPENSOCKETFUNCTION, &CurlOpenSocketCallbackStatic);
    curl.SetOption(CURLOPT_OPENSOCKETDATA, this);
    curl.SetOption(CURLOPT_CLOSESOCKETFUNCTION, &CurlCloseSocketCallbackStatic);
    curl.SetOption(CURLOPT_CLOSESOCKETDATA, this);

    curl_map_.emplace(curl.get_handle(), callback);

    CURLMcode code = curl_multi_add_handle(curl_multi_, curl.get_handle());
    if (code != CURLM_OK)
    {
        DEBUG_LOG("Curl | ERROR: curl_multi_add_handle failed: %s\n", curl_multi_strerror(code));
        curl_map_.erase(curl.get_handle());
    }
}

void CurlMulti::RemoveCurl(Curl& curl)
{
    curl_map_.erase(curl.get_handle());
    curl_multi_remove_handle(curl_multi_, curl.get_handle());
}

curl_socket_t CurlMulti::CurlOpenSocketCallback(curlsocktype purpose, curl_sockaddr* address)
{
    curl_socket_t sockfd = CURL_SOCKET_BAD;

    if (purpose == CURLSOCKTYPE_IPCXN && (address->family == AF_INET || address->family == AF_INET6))
    {
        asio::ip::tcp::socket tcp_socket = asio_poller_.CreateTcpSocket();

        asio::error_code ec;
        tcp_socket.open(address->family == AF_INET ? asio::ip::tcp::v4() : asio::ip::tcp::v6(), ec);

        if (ec)
        {
            DEBUG_LOG("Curl | open socket error: %d\n", ec);
            return CURL_SOCKET_BAD;
        }

        sockfd = tcp_socket.native_handle();

        socket_map_.emplace(sockfd, std::move(tcp_socket));
    }
    else
        DEBUG_LOG("Curl | can't open socket, invalid condition. purpose=%d; address->family=%d\n", purpose, address->family);

    DEBUG_LOG("Curl | open socket %s %d\n", (address->family == AF_INET ? "ipv4" : "ipv6"), sockfd);

    return sockfd;
}

int CurlMulti::CurlCloseSocketCallback(curl_socket_t item)
{
    if (socket_map_.find(item) != socket_map_.end())
    {
        socket_map_.erase(item);
    }

    DEBUG_LOG("Curl | close socket %d\n", item);

    return 0;
}

int CurlMulti::CurlSocketCallback(CURL* easy, curl_socket_t s, int what, void* socketp)
{
    DEBUG_LOG("Curl | CurlSocketCallback: socket=%d; what=%s; sockp=%d\n", s, whatstr[what], socketp);

    // Use shared_ptr from our map to prevent use-after-free
    SocketDataPtr socketData;
    if (socket_data_map_.count(s) > 0)
    {
        socketData = socket_data_map_[s];
    }

    if (what == CURL_POLL_REMOVE)
    {
        if (socketData)
        {
            if (socket_map_.find(s) != socket_map_.end())
            {
                if (socketData->is_ares_socket)
                {
                    socket_map_.at(s).release();
                }
                // Fix: Always erase from socket_map_, not just for ARES sockets
                socket_map_.erase(s);
            }

            removed_sockets_.emplace(s);
            socket_data_map_.erase(s);  // Remove from our tracking map (ref count drops)
            curl_multi_assign(curl_multi_, s, nullptr);  // Clear curl's pointer
        }
    }
    else
    {
        if (!socketData)
        {
            socketData = std::make_shared<SocketData>();
            socket_data_map_[s] = socketData;  // Track in our map

            if (socket_map_.find(s) == socket_map_.end())
            {
                socketData->is_ares_socket = true;

                // 1.3.11 fix (2026-05-13): WrapTcpSocket no longer throws; on
                // EBADF (libcurl handed us a stale fd from a closed socket)
                // it returns a not-open socket and sets ec. Pre-fix this
                // path raised std::system_error which propagated through
                // CurlSocketCallbackStatic and tripped std::terminate() at
                // the C-callback boundary — CHI1 SIGABRT 2026-05-13 00:26.
                asio::error_code wrap_ec;
                asio::ip::tcp::socket tcp_socket = asio_poller_.WrapTcpSocket(s, asio::ip::tcp::v4(), wrap_ec);
                if (wrap_ec) {
                    if (!g_amxxcurl_detached.load(std::memory_order_acquire))
                        MF_PrintSrvConsole("[CURL] ERROR: WrapTcpSocket assign failed: fd=%d ec=%s\n",
                                           s, wrap_ec.message().c_str());
                    // Clean up the tentative SocketData; libcurl will reissue
                    // an event for this fd if it's actually still valid, OR
                    // skip it if it's gone. Returning 0 (success) lets the
                    // multi handle continue processing other transfers.
                    //
                    // Mirror the CURL_POLL_REMOVE branch's cleanup (line 169)
                    // by clearing libcurl's socketp pointer for this fd — we
                    // never called curl_multi_assign with our SocketData* on
                    // this failure path (the happy-path emplace happens
                    // below at line ~210), so without this explicit clear
                    // libcurl would retain whatever stale socketp it had
                    // for s from any earlier eviction. ktp-code-review
                    // 2026-05-13 caught this.
                    curl_multi_assign(curl_multi_, s, nullptr);
                    socket_data_map_.erase(s);
                    return 0;
                }
                socket_map_.emplace(s, std::move(tcp_socket));
            }

            DEBUG_LOG("       Adding SocketData: what=%s; is ares socket: %i\n", whatstr[socketData->previous_action], socketData->is_ares_socket);
            // Note: We pass raw pointer to curl but track lifetime ourselves
            curl_multi_assign(curl_multi_, s, socketData.get());
        }

        if (what != CURL_POLL_NONE)
        {
            DEBUG_LOG("       Changing action from %s to %s\n", whatstr[socketData->previous_action], whatstr[what]);
            SetSock(what, s, socketData);
        }
    }

    return 0;
}

int CurlMulti::CurlTimerCallback(CURLM* multi, long timeout_ms)
{
    auto& timer = asio_poller_.get_timer();

    DEBUG_LOG("Curl | Set timer: %ld\n", timeout_ms);
    timer.cancel();

    if (timeout_ms > 0)
    {
        timer.expires_from_now(std::chrono::milliseconds(timeout_ms));
        timer.async_wait(std::bind(&CurlMulti::AsioTimerCallback, this, _1));
    }
    else if (timeout_ms == 0)
    {
        // timeout_ms==0 means "act immediately", but we may be inside a libcurl callback
        // (e.g. curl_multi_add_handle), so calling curl_multi_socket_action here would be
        // a recursive API call (CURLM_RECURSIVE_API_CALL). Post to io_context so it fires
        // on the next Poll() call instead, outside of any libcurl callback.
        asio_poller_.get_io_context().post([this]() {
            // Guard: ~CurlMulti() sets curl_multi_ = nullptr; skip the call if
            // the posted task fires after destruction (e.g. another Poll() runs
            // between the drain loop exit in OnAmxxDetach and RemoveAllTasks).
            if (curl_multi_ == nullptr) return;
            curl_multi_socket_action(curl_multi_, CURL_SOCKET_TIMEOUT, 0, &running_handles_);
            CheckMultiInfo();
        });
    }

    return 0;
}

// calls by asio when data r/w is ready
void CurlMulti::AsioSocketActionCallback(int action, curl_socket_t s, SocketDataPtr socket_data, const asio::error_code& error)
{
    // shared_ptr ensures socket_data is valid even if removed from map
    if (socket_map_.count(s) == 0 || !socket_data)
    {
        return;
    }

    DEBUG_LOG("Asio cb | socket triggered; socket: %d; is_ares_socket: %d; action: %s; prev_action: %s\n", s, socket_data->is_ares_socket, whatstr[action], whatstr[socket_data->previous_action]);

    if (error || action == socket_data->previous_action || socket_data->previous_action == CURL_POLL_INOUT)
    {
        if (error)
            action = CURL_CSELECT_ERR;

        removed_sockets_.clear();
        CURLMcode rc = curl_multi_socket_action(curl_multi_, s, action, &running_handles_);

        CheckMultiInfo();

        if (running_handles_ <= 0)
        {
            asio_poller_.get_timer().cancel();
            DEBUG_LOG("          cancel timer (no more handles)\n");
        }

        /* keep on watching.
         * the socket may have been closed and/or socket_data may have been changed
         * in curl_multi_socket_action(), so check them both */
        if (   !error
            && !removed_sockets_.count(s)
            && socket_map_.find(s) != socket_map_.end()
            && socket_data_map_.count(s) > 0  // Verify socket_data still tracked
            && (action == socket_data->previous_action || socket_data->previous_action == CURL_POLL_INOUT))
        {
            auto& tcp_socket = socket_map_.find(s)->second;

            // Pass shared_ptr to async callback - prevents use-after-free
            if (action & CURL_POLL_IN)
                tcp_socket.async_wait(asio::socket_base::wait_type::wait_read, std::bind(&CurlMulti::AsioSocketActionCallback, this, action, s, socket_data, _1));

            if (action & CURL_POLL_OUT)
                tcp_socket.async_wait(asio::socket_base::wait_type::wait_write, std::bind(&CurlMulti::AsioSocketActionCallback, this, action, s, socket_data, _1));
        }
    }
}

// calls by asio when need update curl
void CurlMulti::AsioTimerCallback(const asio::error_code& error)
{
    if (error)
    {
        //std::cout << "asio timer cb error: " << error.value() << " text: " << error.message() << std::endl;
        return;
    }

    // Guard: async_wait binds `this`; if the timer fires after ~CurlMulti() ran
    // (e.g. during shutdown), curl_multi_ will be nullptr — skip the call.
    if (curl_multi_ == nullptr) return;

    DEBUG_LOG("Asio cb | timer triggered\n");

    curl_multi_socket_action(curl_multi_, CURL_SOCKET_TIMEOUT, 0, &running_handles_);
    CheckMultiInfo();
}

void CurlMulti::CheckMultiInfo()
{
    CURLMsg* msg;
    int msgs_left;

    CURL* easy;
    CURLcode res;

    while ((msg = curl_multi_info_read(curl_multi_, &msgs_left)))
    {
        if (msg->msg == CURLMSG_DONE)
        {
            easy = msg->easy_handle;
            res = msg->data.result;

            if (curl_map_.count(easy))
                curl_map_[easy](res);
        }
    }

    return;
}

void CurlMulti::SetSock(int act, curl_socket_t s, SocketDataPtr socket_data)
{
    auto it = socket_map_.find(s);
    if (it == socket_map_.end())
    {
        // KTP: Graceful return instead of exception — this runs inside a libcurl callback
        // where throwing is undefined behavior. Socket may have been closed between events.
        DEBUG_LOG("Curl | WARNING: SetSock called for unknown socket %d, ignoring\n", s);
        return;
    }

    asio::ip::tcp::socket& tcp_socket = it->second;

    // Pass shared_ptr to async callbacks - prevents use-after-free
    if (act == CURL_POLL_IN)
    {
        if (socket_data->previous_action != CURL_POLL_IN && socket_data->previous_action != CURL_POLL_INOUT)
        {
            tcp_socket.async_wait(asio::socket_base::wait_type::wait_read, std::bind(&CurlMulti::AsioSocketActionCallback, this, act, s, socket_data, _1));
        }
    }
    else if (act == CURL_POLL_OUT)
    {
        if (socket_data->previous_action != CURL_POLL_OUT && socket_data->previous_action != CURL_POLL_INOUT)
        {
            tcp_socket.async_wait(asio::socket_base::wait_type::wait_write, std::bind(&CurlMulti::AsioSocketActionCallback, this, act, s, socket_data, _1));
        }
    }
    else if (act == CURL_POLL_INOUT)
    {
        if (socket_data->previous_action != CURL_POLL_IN && socket_data->previous_action != CURL_POLL_INOUT)
        {
            tcp_socket.async_wait(asio::socket_base::wait_type::wait_read, std::bind(&CurlMulti::AsioSocketActionCallback, this, act, s, socket_data, _1));
        }
        if (socket_data->previous_action != CURL_POLL_OUT && socket_data->previous_action != CURL_POLL_INOUT)
        {
            tcp_socket.async_wait(asio::socket_base::wait_type::wait_write, std::bind(&CurlMulti::AsioSocketActionCallback, this, act, s, socket_data, _1));
        }
    }

    socket_data->previous_action = act;
}
