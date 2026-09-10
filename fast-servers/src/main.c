/*
 * One binary, three roles, and one command that is not a role.
 *
 * `gradido2-fast` with no argument is the backend, which is the common case and therefore the
 * default. Each of --backend, --federation and --dht-node selects a role, and several of them
 * select several: they run in one process, on one thread each, sharing one backend-core and one
 * log stream. That is what a community server on a small machine wants, and splitting them
 * across processes on a large one needs no code change -- it is three invocations.
 *
 *   gradido2-fast                          the backend
 *   gradido2-fast --federation             federation only
 *   gradido2-fast --backend --dht-node     both, in one process
 *   gradido2-fast setup                    say who this community is, then stop
 *   gradido2-fast migrate-down             take the database down one migration, then stop
 *
 * The last two are commands rather than roles and serve nothing, for two different reasons.
 *
 * `migrate-down`: a serving start migrates *up* to the version its code needs, so taking the
 * database to N-1 and then serving a build that needs N would undo the step and re-apply it in
 * the same breath. Going down means the next thing started is a different build, and that is a
 * separate act.
 *
 * `setup` asks questions. A process that both answers requests and reads an answer off a
 * terminal is two things at once: it cannot be started unattended, its log and its prompts share
 * a terminal, and under `docker compose up` the questions go where nobody is looking. So a
 * serving start against a database with no community stops and names this command instead. See
 * backend/backend.h for both.
 *
 * Shutdown is one flag. SIGINT or SIGTERM raises it, every run loop notices within
 * SC_RUNTIME_TICK_MS and returns, and main joins the threads. Nothing is cancelled from the
 * outside: a thread stopped mid-request is a thread that leaked whatever it was holding.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <uv.h>

#include "backend/backend.h"
#include "dht_node/dht_node_server.h"
#include "federation/federation.h"
#include "service_core/config.h"
#include "service_core/db.h"
#include "service_core/email/transport.h"
#include "service_core/env_file.h"
#include "service_core/http.h"
#include "service_core/jwt.h"
#include "service_core/log/log.h"
#include "service_core/log/logger.h"
#include "service_core/runtime.h"
#include "service_core/status.h"

#define FS_VERSION "0.0.1"

/* Set by the build -- see build.zig and CMakeLists.txt. A compiler that was handed neither says
 * so rather than claiming a mode it does not know. */
#ifndef FS_OPTIMIZE
#define FS_OPTIMIZE "unknown"
#endif

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

/**
 * A command: the whole process, doing one thing that is not serving.
 *
 * Two of them, and a third would be a line here -- the shape is the one the role registry above
 * has, for the same reason. `quit` is passed on because both of them wait for a database and a
 * Ctrl-C during that wait has to be noticed the way it is everywhere else.
 */
typedef sc_status (*fs_command_fn)(const sc_config *cfg, const sc_quit_flag *quit);

typedef struct fs_command {
    const char *name;
    fs_command_fn run;
    const char *summary;
    /** The second line of the usage entry, where one sentence does not carry it. */
    const char *detail;
} fs_command;

static const fs_command kCommands[] = {
    {"setup", backend_setup, "say who this community is and stop",
     "run it once, with a terminal attached, before the first start"},
    {"migrate-down", backend_migrate_down, "take the database down one migration and stop",
     "on a release only, with DB_MIGRATE_DOWN naming the migration one lower"},
};

#define FS_COMMAND_COUNT ((int)(sizeof(kCommands) / sizeof(kCommands[0])))

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

    /* Its own arena pool and its own return queue, for as long as the role runs -- the logger
     * hands arenas back to the thread that took them and to no one else. Leaving is what
     * releases the pool again, and it has to happen while the logger is still running, which
     * is why it is here and not after the join below. */
    (void)sc_log_thread_join();
    slot->status = slot->role->run(slot->cfg, &g_quit);
    if (slot->status != SC_OK) {
        /* One role that cannot start takes the process down. A half-started server that keeps
         * answering on two ports out of three is the failure mode an operator does not see. */
        sc_log_fatal(SC_CAT_STARTUP, "role.failed", "%s stopped with %s", slot->role->name,
                     sc_status_name(slot->status));
        sc_runtime_request_quit();
    }
    sc_log_thread_leave();
}

static void print_usage(FILE *out)
{
    int i;

    fprintf(out, "gradido2-fast %s -- the C implementation of the gradido2 servers\n\n",
            FS_VERSION);
    fprintf(out, "usage: gradido2-fast [role...]\n\n");
    fprintf(out, "roles (several may be combined; none means --backend):\n");
    for (i = 0; i < FS_ROLE_COUNT; ++i)
        fprintf(out, "  %-14s %s\n", kRoles[i].flag, kRoles[i].summary);
    fprintf(out, "\ncommands (instead of a role):\n");
    for (i = 0; i < FS_COMMAND_COUNT; ++i) {
        fprintf(out, "  %-14s %s\n", kCommands[i].name, kCommands[i].summary);
        fprintf(out, "  %-14s %s\n", "", kCommands[i].detail);
    }
    fprintf(out, "\noptions:\n");
    fprintf(out, "  %-14s this text\n", "-h, --help");
    fprintf(out, "  %-14s version and build features\n", "-v, --version");
    fprintf(out, "\nconfiguration is the environment, and a %s beside the binary fills what\n",
            SC_ENV_FILE_NAME);
    fprintf(out, "nobody exported -- what is already set wins. `setup` writes that file.\n\n");
    fprintf(out, "  %-14s LISTEN_HOST, BACKEND_PORT, FEDERATION_PORT, DHT_PORT,\n", "server");
    fprintf(out, "  %-14s FEDERATION_DHT_TOPIC, FEDERATION_DHT_SEED, SERVER_THREADS\n", "");
    fprintf(out, "  %-14s DB_TYPE (sqlite or postgresql), DB_FILE, and for postgresql\n",
            "database");
    fprintf(out, "  %-14s DB_HOST, DB_PORT, DB_USER, DB_PASSWORD, DB_DATABASE,\n", "");
    fprintf(out, "  %-14s DB_POOL_SIZE (default %d: connections, one database worker each)\n", "",
            SC_DB_POOL_SIZE_DEFAULT);
    fprintf(out, "  %-14s a DB_HOST beginning with / is a unix socket directory\n", "");
    fprintf(out, "  %-14s EMAIL_SMTP_HOST, EMAIL_SMTP_PORT, EMAIL_SMTP_TLS,\n", "email");
    fprintf(out, "  %-14s EMAIL_USERNAME, EMAIL_PASSWORD, EMAIL_SENDER,\n", "");
    fprintf(out, "  %-14s EMAIL_SENDER_NAME, EMAIL_CHANGE_SUPPORT\n", "");
    fprintf(out, "  %-14s LOG_LEVEL, NODE_ENV\n", "other");
    fprintf(out, "\nDB_PASSWORD and EMAIL_PASSWORD are secrets: before either variable is\n");
    fprintf(out, "read, the systemd credential of that name and then the file <NAME>_FILE\n");
    fprintf(out, "names are looked at. See contracts/secrets.json.\n");
}

static void print_version(void)
{
    printf("gradido2-fast %s\n", FS_VERSION);
    printf("http backend: %s\n", sc_http_backend_name());
    /* Which of the release modes this is. ReleaseFast is what a bundle ships by default;
     * ReleaseSafe keeps the checks that trap on undefined behaviour, and a Debug binary on a
     * server is a mistake worth being able to see. */
    printf("optimize: %s\n", FS_OPTIMIZE);
    /* Which databases this binary could open. No role opens one yet -- service_core/db.h says
     * what is missing before one can -- and which drivers are in is still a property of the
     * build that is worth being able to read off it rather than infer from how it was made. */
    printf("database drivers: %s\n", sc_db_drivers());
}

int main(int argc, char **argv)
{
    int selected[FS_ROLE_COUNT];
    fs_role_thread threads[FS_ROLE_COUNT];
    sc_config cfg;
    sc_log_config log_cfg;
    char env_error[256];
    sc_status env_status;
    sc_status status;
    int any_selected = 0;
    const fs_command *command = NULL;
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
        for (r = 0; r < FS_COMMAND_COUNT; ++r) {
            if (strcmp(arg, kCommands[r].name) == 0) {
                if (command != NULL && command != &kCommands[r]) {
                    fprintf(stderr,
                            "gradido2-fast: %s and %s are both commands; the process "
                            "does one thing or the other\n\n",
                            command->name, kCommands[r].name);
                    print_usage(stderr);
                    return 2;
                }
                command = &kCommands[r];
                matched = 1;
                break;
            }
        }
        if (matched)
            continue;
        for (r = 0; r < FS_ROLE_COUNT; ++r) {
            if (strcmp(arg, kRoles[r].flag) == 0) {
                selected[r] = 1;
                any_selected = 1;
                matched = 1;
                break;
            }
        }
        if (!matched) {
            fprintf(stderr, "gradido2-fast: unknown argument '%s'\n\n", arg);
            print_usage(stderr);
            return 2;
        }
    }
    if (command != NULL && any_selected) {
        fprintf(stderr,
                "gradido2-fast: %s is a command, not a role; it serves nothing and "
                "cannot be combined with one\n\n",
                command->name);
        print_usage(stderr);
        return 2;
    }
    if (!any_selected)
        selected[0] = 1; /* --backend, the default */

    /*
     * The `.env` before anything reads a variable, LOG_LEVEL included, and before any thread
     * exists: setenv is not thread safe against a concurrent getenv. What is already in the
     * environment wins, so a file in the working directory never overrides what systemd or
     * docker set. It is the same file the reference path reads with dotenv, which is what lets
     * an operator switch between the two implementations without reconfiguring anything.
     *
     * A file that cannot be parsed is reported once the logger exists, a few lines down: this
     * process has no way to say anything yet, and a configuration read to the middle is worse
     * than one not read at all.
     */
    env_status = sc_env_file_load(SC_ENV_FILE_NAME, env_error, sizeof(env_error));

    /*
     * The logger before the configuration, and its level straight out of the environment rather
     * than out of `cfg`: sc_config_load() logs about what it cannot read, and it would be a poor
     * arrangement in which the variable that says how loud to be is only honoured once the whole
     * environment has parsed. sc_config_load() reads the same variable into cfg.log_level, which
     * is what config.loaded reports back.
     *
     * A failure here is not fatal and is not reported: without a logger thread every line takes
     * the synchronous path instead, which is the shape this whole process had until the ring
     * existed. Losing the throughput is worth a great deal less than losing the lines.
     *
     * **A command gets that synchronous path on purpose.** The ring exists to keep a request
     * from waiting on a write, and a command serves no requests: `setup` reads an answer off the
     * terminal it is logging to, where a line still sitting in a 64 KiB buffer would surface
     * between two questions, and `migrate-down` writes a handful of lines and stops. It is the
     * same call the reference path makes with pino's `sync` when stdout is a terminal.
     */
    sc_log_default_config(&log_cfg);
    log_cfg.min_level = sc_log_level_from_name(getenv("LOG_LEVEL"), SC_LOG_INFO);
    log_cfg.synchronous = command != NULL;
    (void)sc_log_init(&log_cfg);
    (void)sc_log_thread_join();

    if (env_status != SC_OK) {
        sc_log_fatal(SC_CAT_STARTUP, "config.env_file_invalid", "%s", env_error);
        sc_log_thread_leave();
        sc_log_shutdown();
        return 1;
    }

    status = sc_config_load(&cfg);
    if (status != SC_OK) {
        sc_log_fatal(SC_CAT_STARTUP, "config.failed", "configuration is unusable: %s",
                     sc_status_name(status));
        sc_log_thread_leave();
        sc_log_shutdown();
        return 1;
    }
    sc_config_log(&cfg);
    sc_log_info(SC_CAT_STARTUP, "process.start", "gradido2-fast %s, http backend %s", FS_VERSION,
                sc_http_backend_name());

    /* libsodium wants to be initialised once, from one thread, before anything asks it for a
     * digest. Here is that thread and this is that moment: no role has started yet. And curl
     * wants the same, for the same reason -- curl_easy_init() would do it on its own, and its
     * own documentation says doing it that way is not thread safe. */
    sc_jwt_init();
    (void)sc_mail_global_init();
    sc_runtime_install_signal_handlers(&g_quit);

    /* The command, and then the process is over: nothing listens, no role starts, and the flag
     * is installed above only so that a Ctrl-C while the database is being waited for is noticed
     * the way it is everywhere else. */
    if (command != NULL) {
        exit_code = command->run(&cfg, &g_quit) == SC_OK ? 0 : 1;
        sc_log_thread_leave();
        sc_log_shutdown();
        return exit_code;
    }

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

    /* Last, and in this order: every role thread has been joined, so nothing is still writing a
     * line, and this thread gives its own pool back before the logger that holds the other end
     * of it stops. Without the shutdown the lines still in the ring -- process.stopping among
     * them -- would never be written at all. */
    sc_log_thread_leave();
    sc_log_shutdown();
    return exit_code;
}
