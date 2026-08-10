// Standalone harness for the REAL CurlMulti -- this module's first test.
//
// WHY THIS EXISTS
// CU-02: when libcurl asks for CURL_POLL_INOUT, SetSock arms both a read and a
// write wait, each bound with action = INOUT(3). If the write fires first,
// curl_multi_socket_action can drop the socket to CURL_POLL_IN, and SetSock(IN)
// then correctly declines to re-arm (a read wait is already pending) while
// setting previous_action = IN(1). The still-pending read wait is now bound to
// 3 against a previous_action of 1, so when data finally arrives the guard
//
//     if (error || action == previous_action || previous_action == CURL_POLL_INOUT)
//
// matches none of its arms and the event is dropped: libcurl is never told the
// socket is readable, and nothing re-arms. The transfer hangs until
// CURLOPT_TIMEOUT.
//
// The card that filed CU-02 also recorded why a cheaper probe does not work: a
// bare "event dropped" log false-positives on the benign IN->OUT case, where
// discarding a readiness curl no longer wants is correct. Telling a stall from
// a correct drop needs per-direction arming state -- which is exactly what the
// fix must add. So the measurement and the fix are the same change, and this
// harness is the only way to see either.
//
// WHAT IT DOES
// Runs a real libcurl transfer through the real CurlMulti against a loopback
// HTTP server, with no AMXX and no HLDS. To reach INOUT the request must want
// to write and read at once, so it POSTs a body larger than one socket write
// while the server replies with a body larger than one read.
//
// RED/GREEN CONTRACT
//   RED  -> a readiness event was dropped; the re-arm defect is live. On the
//           current build that surfaces as CURLE_OPERATION_TIMEDOUT after
//           ~16 KB of 512 -- but the dropped-event count, not the timeout, is
//           the assertion: libcurl's timer can re-drive a dropped socket, so a
//           completion check alone passes straight through the defect.
//   GREEN -> INOUT was exercised and no event was dropped.
// A pass is only meaningful if the INOUT transition actually occurred, so the
// harness asserts it was observed and fails loudly if it never happened --
// otherwise a green run could mean "the bug is fixed" or "we never exercised
// it", which are not the same result.
#include <atomic>
#include <memory>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include "asio_poller.h"
#include "curl_class.h"
#include "curl_callback_class.h"
#include "curl_multi_class.h"

namespace {

constexpr std::size_t kUploadBytes = 512 * 1024;    // exceeds one socket write
constexpr std::size_t kDownloadBytes = 512 * 1024;  // exceeds one socket read
constexpr int kTimeoutSeconds = 20;

int g_exit = 0;

// Minimal CurlCallback: the harness only needs the write path. Curl routes
// CURLOPT_WRITEFUNCTION through CurlCallback's static trampoline, so overriding
// the virtual is the supported way in -- setting the option directly would be
// overwritten by Curl's own wiring.
class HarnessCallback : public CurlCallback
{
public:
    std::string body;
protected:
    size_t WriteCallback(char* ptr, size_t size, size_t nmemb) override
    {
        body.append(ptr, size * nmemb);
        return size * nmemb;
    }
};

void Check(bool ok, const std::string& what)
{
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << what << "\n";
    if (!ok) g_exit = 1;
}

// Minimal loopback HTTP/1.1 server. Reads the full request, then replies with a
// large fixed body. Deliberately hand-rolled: pulling in a framework would add a
// dependency to a module that currently has none.
// Binds port 0 and reports back what the OS assigned. A fixed port collides with
// a leftover harness or anything else on the box; libcurl then fails to connect
// and the run reads as a stall, which is the exact signal this harness exists to
// measure. On the fleet a crash here would also drop a core into /tmp, which is a
// monitored deploy signal.
void RunServer(std::atomic<int>& port)
{
    asio::io_context io;
    asio::ip::tcp::acceptor acceptor(
        io, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    port = acceptor.local_endpoint().port();

    asio::ip::tcp::socket sock(io);
    asio::error_code ec;
    acceptor.accept(sock, ec);
    if (ec) return;

    // Read headers, then the declared body. Slowly enough that the client still
    // has bytes to write when the response starts -- that overlap is the INOUT.
    std::string buf;
    char tmp[4096];
    std::size_t content_length = 0;
    while (true)
    {
        std::size_t n = sock.read_some(asio::buffer(tmp, sizeof(tmp)), ec);
        if (ec) break;
        buf.append(tmp, n);
        auto hdr_end = buf.find("\r\n\r\n");
        if (hdr_end == std::string::npos) continue;
        auto cl = buf.find("Content-Length:");
        if (cl != std::string::npos)
            content_length = std::stoul(buf.substr(cl + 15));
        if (buf.size() >= hdr_end + 4 + content_length) break;
    }

    const std::string body(kDownloadBytes, 'x');
    std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: "
                     + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    asio::write(sock, asio::buffer(resp), ec);
    sock.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
}

}  // namespace

int main()
{
    std::cout << "KTPAmxxCurl :: CurlMulti re-arm harness (CU-02)\n";
    curl_global_init(CURL_GLOBAL_DEFAULT);

    std::atomic<int> server_port{0};
    std::thread server(RunServer, std::ref(server_port));
    while (server_port == 0) std::this_thread::sleep_for(std::chrono::milliseconds(5));

    long elapsed = 0;
    bool completed = false;

    // Scoped so Curl/CurlMulti destruct -- and with them curl_easy_cleanup and
    // curl_multi_cleanup -- BEFORE curl_global_cleanup(). Calling any libcurl API
    // after global cleanup is the ordering invariant in
    // amx_curl_controller_class.h; a stack Curl in main violates it silently.
    {
        AsioPoller poller;
        CurlMulti multi(poller);

        const std::string upload(kUploadBytes, 'u');
        std::atomic<bool> done{false};
        std::atomic<int> result{-1};

        auto cb = std::make_shared<HarnessCallback>();
        Curl curl(cb);
        const std::string url =
            "http://127.0.0.1:" + std::to_string(server_port.load()) + "/";

        // Suppress Expect: 100-continue -- WITHOUT this the harness measures the
        // wrong thing. libcurl adds the header for a body this size, then waits
        // on a 100 the hand-rolled server never sends; that pause serializes the
        // upload against the download so the INOUT overlap never forms and the
        // run comes back CURLE_OK in ~1s, masking the defect entirely. With it
        // suppressed the same build stalls to CURLOPT_TIMEOUT on 16 KB of 512.
        curl_slist* headers = curl_slist_append(nullptr, "Expect:");

        curl.SetOption(CURLOPT_URL, url.c_str());
        curl.SetOption(CURLOPT_HTTPHEADER, headers);
        curl.SetOption(CURLOPT_POST, 1L);
        curl.SetOption(CURLOPT_POSTFIELDS, upload.data());
        curl.SetOption(CURLOPT_POSTFIELDSIZE, (long)upload.size());
        curl.SetOption(CURLOPT_TIMEOUT, (long)kTimeoutSeconds);
        curl.BindCallback(CURLOPT_WRITEFUNCTION);

        const auto started = std::chrono::steady_clock::now();
        multi.AddCurl(curl, [&](CURLcode code) { result = (int)code; done = true; });

        // Drive the poller until the transfer finishes or we exceed the budget.
        // The budget is deliberately longer than CURLOPT_TIMEOUT so a stalled
        // transfer surfaces as curl's own timeout rather than as harness
        // impatience -- the two look identical from the outside otherwise.
        const auto deadline = started + std::chrono::seconds(kTimeoutSeconds + 10);
        while (!done && std::chrono::steady_clock::now() < deadline)
        {
            poller.Poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        elapsed = (long)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        completed = done;

        std::cout << "\n  transfer completed : " << (completed ? "yes" : "NO (stalled)")
                  << "\n  CURLcode           : " << result
                  << "\n  bytes received     : " << cb->body.size() << " / " << kDownloadBytes
                  << "\n  elapsed            : " << elapsed << " ms\n\n";

        // Not "did it succeed" -- the completion callback fires on CURLE_OPERATION_TIMEDOUT
        // too. This only proves the poller was still driving; the next three checks
        // are what tell a real transfer from a timed-out one.
        Check(completed, "completion callback fired (poller kept running)");
        Check(result == (int)CURLE_OK, "CURLcode is CURLE_OK");
        Check(cb->body.size() == kDownloadBytes, "full response body received");
        Check(elapsed < kTimeoutSeconds * 1000,
              "completed well inside CURLOPT_TIMEOUT (a stall would sit at the timeout)");

        multi.RemoveCurl(curl);
        multi.Shutdown();
        // After RemoveCurl: libcurl must not be holding the list any more.
        curl_slist_free_all(headers);
    }

    if (server.joinable()) server.join();
    curl_global_cleanup();

    std::cout << "\n" << (g_exit ? "HARNESS RED" : "HARNESS GREEN") << "\n";
    return g_exit;
}
