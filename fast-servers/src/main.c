/*
 * One binary, three roles.
 *
 * `fast-servers` with no argument is the backend, which is the common case and therefore the
 * default. Each of --backend, --federation and --dht-node selects a role, and several of them
 * select several: they run in one process, on one thread each, sharing one backend-core and one
 * log stream. That is what a community server on a small machine wants, and splitting them
 * across processes on a large one needs no code change -- it is three invocations.
 *
 *   fast-servers                          the backend
 *   fast-servers --federation             federation only
 *   fast-servers --backend --dht-node     both, in one process
 *
 * Shutdown is one flag. SIGINT or SIGTERM raises it, every run loop notices within
 * SC_RUNTIME_TICK_MS and returns, and main joins the threads. Nothing is cancelled from the
 * outside: a thread stopped mid-request is a thread that leaked whatever it was holding.
 */
#include <stdio.h>
#include <string.h>

#include <uv.h>

#include "backend/backend.h"
#include "dht_node/dht_node_server.h"
#include "federation/federation.h"
#include "service_core/config.h"
#include "service_core/http.h"
#include "service_core/jwt.h"
#include "service_core/log.h"
#include "service_core/runtime.h"
#include "service_core/status.h"

#define FS_VERSION "0.0.1"

typedef sc_status (*fs_role_fn)(const sc_config *cfg, const sc_quit_flag *quit);

typedef struct fs_role {
    const char *flag;
    const char *name;
    fs_role_fn run;
    const char *summary;
} fs_role;

/* The whole registry. A fourth role is a line here plus a module beside the other three --
 * and, before either, a change to Architecture.md, because what a fast server is for is a
 * design decision and not a command line option. */
static const fs_role kRoles[] = {
    {"--backend", "backend", backend_run, "HTTP server the frontend talks to (default)"},
    {"--federation", "federation", federation_run, "HTTP server other communities talk to"},
    {"--dht-node", "dht-node", dht_node_server_run, "peer discovery, needs FEDERATION_DHT_TOPIC"},
};

#define FS_ROLE_COUNT ((int)(sizeof(kRoles) / sizeof(kRoles[0])))

static sc_quit_flag g_quit;

typedef struct fs_role_thread {
    const fs_role *role;
    const sc_config *cfg;
    sc_status status;
    uv_thread_t handle;
    int started;
} fs_role_thread;

/* uv_thread_cb answers nothing, so the outcome goes into the slot the caller already owns --
 * which is where main reads it after the join anyway. */
static void run_role(void *arg)
{
    fs_role_thread *slot = (fs_role_thread *)arg;

    slot->status = slot->role->run(slot->cfg, &g_quit);
    if (slot->status != SC_OK) {
        /* One role that cannot start takes the process down. A half-started server that keeps
         * answering on two ports out of three is the failure mode an operator does not see. */
        sc_log_fatal(SC_CAT_STARTUP, "role.failed", "%s stopped with %s", slot->role->name,
                     sc_status_name(slot->status));
        sc_runtime_request_quit();
    }
}

static void print_usage(FILE *out)
{
    int i;

    fprintf(out, "fast-servers %s -- the C implementation of the gradido2 servers\n\n", FS_VERSION);
    fprintf(out, "usage: fast-servers [role...]\n\n");
    fprintf(out, "roles (several may be combined; none means --backend):\n");
    for (i = 0; i < FS_ROLE_COUNT; ++i)
        fprintf(out, "  %-14s %s\n", kRoles[i].flag, kRoles[i].summary);
    fprintf(out, "\noptions:\n");
    fprintf(out, "  %-14s this text\n", "-h, --help");
    fprintf(out, "  %-14s version and build features\n", "-v, --version");
    fprintf(out, "\nconfiguration is read from the environment: LISTEN_HOST, BACKEND_PORT,\n");
    fprintf(out,
            "FEDERATION_PORT, DHT_PORT, FEDERATION_DHT_TOPIC, FEDERATION_DHT_SEED, LOG_LEVEL\n");
}

static void print_version(void)
{
    printf("fast-servers %s\n", FS_VERSION);
    printf("http backend: %s\n", sc_http_backend_name());
}

int main(int argc, char **argv)
{
    int selected[FS_ROLE_COUNT];
    fs_role_thread threads[FS_ROLE_COUNT];
    sc_config cfg;
    sc_status status;
    int any_selected = 0;
    int exit_code = 0;
    int i;

    memset(selected, 0, sizeof(selected));
    memset(threads, 0, sizeof(threads));

    for (i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        int matched = 0;
        int r;

        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            print_usage(stdout);
            return 0;
        }
        if (strcmp(arg, "-v") == 0 || strcmp(arg, "--version") == 0) {
            print_version();
            return 0;
        }
        for (r = 0; r < FS_ROLE_COUNT; ++r) {
            if (strcmp(arg, kRoles[r].flag) == 0) {
                selected[r] = 1;
                any_selected = 1;
                matched = 1;
                break;
            }
        }
        if (!matched) {
            fprintf(stderr, "fast-servers: unknown argument '%s'\n\n", arg);
            print_usage(stderr);
            return 2;
        }
    }
    if (!any_selected)
        selected[0] = 1; /* --backend, the default */

    status = sc_config_load(&cfg);
    /* The log is initialised after the config, which is why sc_config_load's own failures are
     * logged at the default level -- a configuration that cannot be read cannot say how it
     * wanted to be logged about. */
    if (status != SC_OK) {
        sc_log_init(SC_LOG_INFO);
        sc_log_fatal(SC_CAT_STARTUP, "config.failed", "configuration is unusable: %s",
                     sc_status_name(status));
        return 1;
    }
    sc_log_init(cfg.log_level);
    sc_config_log(&cfg);
    sc_log_info(SC_CAT_STARTUP, "process.start", "fast-servers %s, http backend %s", FS_VERSION,
                sc_http_backend_name());

    /* libsodium wants to be initialised once, from one thread, before anything asks it for a
     * digest. Here is that thread and this is that moment: no role has started yet. */
    sc_jwt_init();
    sc_runtime_install_signal_handlers(&g_quit);

    for (i = 0; i < FS_ROLE_COUNT; ++i) {
        if (!selected[i])
            continue;
        threads[i].role = &kRoles[i];
        threads[i].cfg = &cfg;
        threads[i].status = SC_OK;
        if (uv_thread_create(&threads[i].handle, run_role, &threads[i]) != 0) {
            sc_log_fatal(SC_CAT_STARTUP, "role.thread_failed", "cannot start a thread for %s",
                         kRoles[i].name);
            sc_runtime_request_quit();
            exit_code = 1;
        } else {
            threads[i].started = 1;
        }
    }

    /* main owns nothing but the flag. Every role polls it; this loop waits for it. */
    while (!sc_quit_requested(&g_quit))
        sc_runtime_sleep_ms(SC_RUNTIME_TICK_MS);
    sc_log_info(SC_CAT_STARTUP, "process.stopping", "shutting down");

    for (i = 0; i < FS_ROLE_COUNT; ++i) {
        if (!threads[i].started)
            continue;
        (void)uv_thread_join(&threads[i].handle);
        if (threads[i].status != SC_OK)
            exit_code = 1;
    }
    return exit_code;
}
