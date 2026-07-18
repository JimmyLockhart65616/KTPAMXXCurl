#include "asio_poller.h"
#include "sdk/amxxmodule.h"          // MF_PrintSrvConsole for the poll boundary catch
#include "amx_curl_callback_class.h" // g_amxxcurl_detached — MF_* is stale once set

using namespace std;
using namespace asio::ip;

AsioPoller::AsioPoller() :
    timer_(io_context_)
{
}

AsioPoller::~AsioPoller()
{
}

tcp::socket AsioPoller::CreateTcpSocket()
{
    return tcp::socket(io_context_);
}

tcp::socket AsioPoller::WrapTcpSocket(const asio::detail::socket_type& native_socket,
                                       const tcp::socket::protocol_type protocol,
                                       asio::error_code& ec)
{
    // Default-construct first, then call the non-throwing assign overload.
    // basic_socket.hpp:389 — assign(protocol, native_socket, ec) returns the
    // error_code by reference without throwing. EBADF on a stale fd populates
    // ec; the returned socket is left default-constructed (not open).
    tcp::socket socket(io_context_);
    socket.assign(protocol, native_socket, ec);
    return socket;
}

void AsioPoller::Poll()
{
    // Outermost game-thread boundary. io_context::poll() rethrows anything an
    // asio handler let escape (bad_alloc, a throwing timer.cancel(), an uncaught
    // std::runtime_error from a curl completion callback). Unguarded, that
    // unwinds through CurlFrameCallback / the OnAmxxDetach drain loop into
    // ReHLDS's -fno-exceptions frame loop = std::terminate. Mirror the
    // C-callback boundary catch (curl_multi_class.cc); io_context stays valid
    // after a handler throws, so the stopped()/restart() check still runs.
    try {
        io_context_.poll();
    } catch (const std::exception& ex) {
        if (!g_amxxcurl_detached.load(std::memory_order_acquire))
            MF_PrintSrvConsole("[CURL] FATAL ERROR caught at asio-poll boundary: %s\n", ex.what());
    } catch (...) {
        if (!g_amxxcurl_detached.load(std::memory_order_acquire))
            MF_PrintSrvConsole("[CURL] FATAL ERROR caught at asio-poll boundary: unknown exception\n");
    }

    if (io_context_.stopped())
        io_context_.restart();
}
