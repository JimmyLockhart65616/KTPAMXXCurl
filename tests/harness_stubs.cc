// AMXX seam stubs for the standalone CurlMulti harness.
//
// curl_multi_class.cc touches AMXX in exactly two places, so the whole module
// can be exercised without AMXX or HLDS by supplying just these symbols:
//
//   MF_PrintSrvConsole  -- a MACRO for the function POINTER g_fn_PrintSrvConsole
//                          (amxxmodule.h:2373), not a function. Stubbing a
//                          function named MF_PrintSrvConsole links against
//                          nothing; the pointer is what the call site resolves.
//   g_amxxcurl_detached -- the atomic that says MF_* pointers are stale. Nothing
//                          detaches in the harness, so it stays false.
//
// That narrowness is what makes a real harness possible at all; anything wider
// would have meant linking the game.
//
// Deliberately NOT a mock of the arming logic: the point is to drive the real
// CurlMulti. A harness that reimplemented SetSock would pass on the broken code
// and prove nothing.
#include <atomic>
#include <cstdarg>
#include <cstdio>

typedef void (*PFN_PRINT_SRVCONSOLE)(const char* /*format*/, ...);

static void HarnessPrintSrvConsole(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
}

// The symbol curl_multi_class.cc actually calls, once the macro expands.
PFN_PRINT_SRVCONSOLE g_fn_PrintSrvConsole = &HarnessPrintSrvConsole;

// Mirrors amx_curl_callback_class.h.
std::atomic<bool> g_amxxcurl_detached{false};
