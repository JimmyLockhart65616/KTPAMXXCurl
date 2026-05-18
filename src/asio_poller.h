#ifndef _ASIO_POLLER_CLASS_H_
#define _ASIO_POLLER_CLASS_H_

#include <asio.hpp>
#include <memory>

class AsioPoller
{
public:
    AsioPoller();
    ~AsioPoller();

    void Poll();

    asio::ip::tcp::socket CreateTcpSocket();

    // Wrap a libcurl-supplied native socket fd into an asio TCP socket.
    //
    // 1.3.11 fix (2026-05-13): the prior implementation used the throwing
    // basic_socket(io_context, protocol, native_socket) constructor, which
    // calls asio::detail::throw_error(ec, "assign") in basic_socket.hpp:164
    // if the kernel rejects the fd (EBADF). CHI1 SIGABRT'd at 2026-05-13
    // 00:26 ET when libcurl's multi-handle dispatch handed us a stale fd
    // (closed by another curl event between socket-callback issuance and
    // our dispatch) — std::system_error("assign: Bad file descriptor")
    // propagated through CurlSocketCallbackStatic, hit the C-callback
    // boundary with no handler, std::terminate(), process crash.
    //
    // New signature takes an out-param error_code; on EBADF (or any other
    // kernel error), the returned socket is default-constructed (not open),
    // ec is populated, caller decides how to recover. Standard asio
    // defensive pattern for code reachable from C callbacks.
    asio::ip::tcp::socket WrapTcpSocket(const asio::detail::socket_type& native_socket,
                                         const asio::ip::tcp::socket::protocol_type protocol,
                                         asio::error_code& ec);

    asio::io_context& get_io_context() { return io_context_; }
    asio::steady_timer& get_timer() { return timer_; }

private:
    asio::io_context io_context_;
    asio::steady_timer timer_;
};

#endif // _ASIO_POLLER_CLASS_H_
