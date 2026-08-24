#include "service_core/runtime.h"

#include <errno.h>
#include <signal.h>

#include "service_core/atomic.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#endif

/* The handler may touch nothing else. One pointer, written before any signal can arrive. */
static sc_quit_flag *g_quit_flag;

static void on_signal(int signal_number)
{
    /* The handler interrupted something that may be about to read errno -- accept(2) in the
     * event loop, for one. Whatever the atomic store does to it, the interrupted code must not
     * see it. */
    int saved_errno = errno;

    (void)signal_number;
    if (g_quit_flag != NULL)
        sc_atomic_store(&g_quit_flag->raised, 1);
    errno = saved_errno;
}

void sc_runtime_install_signal_handlers(sc_quit_flag *flag)
{
    g_quit_flag = flag;
    (void)signal(SIGINT, on_signal);
    (void)signal(SIGTERM, on_signal);
#if defined(SIGPIPE)
    /* A client that disconnects mid-response must cost one failed write, not the process. */
    (void)signal(SIGPIPE, SIG_IGN);
#endif
}

void sc_runtime_request_quit(void)
{
    if (g_quit_flag != NULL)
        sc_atomic_store(&g_quit_flag->raised, 1);
}

int sc_quit_requested(const sc_quit_flag *flag)
{
    return flag != NULL && sc_atomic_load(&flag->raised) != 0;
}

void sc_runtime_sleep_ms(unsigned int milliseconds)
{
#if defined(_WIN32)
    Sleep(milliseconds);
#else
    struct timespec ts;
    ts.tv_sec = (time_t)(milliseconds / 1000u);
    ts.tv_nsec = (long)(milliseconds % 1000u) * 1000000L;
    (void)nanosleep(&ts, NULL);
#endif
}
