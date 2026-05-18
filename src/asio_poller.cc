#include "asio_poller.h"

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
    io_context_.poll();

    if (io_context_.stopped())
        io_context_.restart();
}
