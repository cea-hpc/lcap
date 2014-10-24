/*
 * LCAP - Lustre Changelogs Aggregate and Publish
 *
 * Copyright (C)  2013-2014  CEA/DAM
 * Henri DOREAU <henri.doreau@cea.fr>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3.0 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */


#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>

#include "lcap.h"
#include "lcapd_internal.h"

/**
 * Number of seconds to wait between two checks for incoming signals.
 */
#define SIGSLEEP_DELAY  1

/**
 * Number of milliseconds to wait for workers to connect
 */
#define SLOW_JOINER_PAUSE 200


/**
 * Signal handling flags.
 */
int TerminateSig;
int ReloadSig;
int DumpStatsSig;

static pthread_t sigh_thr;


static int lcap_subtask(struct lcap_ctx *ctx, int idx,
                        void *(subtask_main)(void *), pthread_t *thr_id)
{
    struct wrk_args *args;
    pthread_attr_t   attr;
    pthread_t        thr;
    int              rc;

    rc = pthread_attr_init(&attr);
    if (rc) {
        lcaplog_err("Error initializing thread attr: %s", strerror(rc));
        return -rc;
    }

    rc = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (rc) {
        lcaplog_err("Error setting thread state: %s", strerror(rc));
        return -rc;
    }

    args = (struct wrk_args *)malloc(sizeof(*args));
    if (args == NULL) {
        lcaplog_err("Cannot allocate memory for subtask parameters");
        return -ENOMEM;
    }

    args->wa_ctx = ctx;
    args->wa_idx = idx;

    rc = pthread_create(&thr, &attr, subtask_main, args);
    if (rc) {
        lcaplog_err("Can't start subtask: %s", strerror(rc));
        return -rc;
    }

    if (thr_id != NULL)
        *thr_id = thr;

    return 0;
}

static int lcap_workers_start(struct lcap_ctx *ctx)
{
    const struct lcap_cfg   *cfg = ctx_config(ctx);
    int                      count = cfg->ccf_worker_count;
    int                      i, rc;

    ctx->cc_wrk_info = (struct subtask_info *)calloc(count,
                                                     sizeof(struct subtask_info));
    if (ctx->cc_wrk_info == NULL)
        return -ENOMEM;

    rc = lcap_module_load_external(ctx, cfg->ccf_module);
    if (rc)
        return rc;

    for (i = 0; i < count; i++) {
        lcaplog_dbg("Starting worker #%d/%d", i + 1, count);
        rc = lcap_subtask(ctx, i, worker_main, &ctx->cc_wrk_info[i].si_thread);
        if (rc) {
            lcaplog_err("Cannot start worker thread: %s", strerror(rc));
            return rc;
        }
        ctx->cc_wrk_info[i].si_running = true;
    }
    return 0;
}

static int lcap_readers_start(struct lcap_ctx *ctx)
{
    const struct lcap_cfg   *cfg = ctx_config(ctx);
    int                      count = cfg->ccf_mdtcount;
    int                      i;
    int                      rc;

    ctx->cc_rdr_info = (struct subtask_info *)calloc(count,
                                                     sizeof(struct subtask_info));
    if (ctx->cc_rdr_info == NULL)
        return -ENOMEM;

    for (i = 0; i < count; i++) {
        lcaplog_dbg("Starting changelog reader #%d/%d (%s)",
                    i + 1, count, cfg->ccf_mdt[i]);
        rc = lcap_subtask(ctx, i, reader_main, &ctx->cc_rdr_info[i].si_thread);
        if (rc) {
            lcaplog_err("Cannot start reader thread: %s", strerror(rc));
            return rc;
        }
        ctx->cc_rdr_info[i].si_running = true;
    }
    return 0;
}

static int lcap_setup_tasks_ventilator(struct lcap_ctx *ctx)
{
    int rc;

    ctx->cc_zctx = zmq_ctx_new();
    if (ctx->cc_zctx == NULL)
        return -ENOMEM;

    ctx->cc_wrk = zmq_socket(ctx->cc_zctx, ZMQ_DEALER);
    if (ctx->cc_wrk == NULL) {
        rc = -errno;
        lcaplog_err("Opening publication socket: %s", strerror(-rc));
        return rc;
    }

    rc = zmq_bind(ctx->cc_wrk, WORKERS_URL);
    if (rc) {
        rc = -errno;
        lcaplog_err("Binding to %s: %s", WORKERS_URL, strerror(-rc));
        return rc;
    }

    lcaplog_dbg("Ready to accept workers on %s", WORKERS_URL);
    return 0;
}

static int lcap_bind_server(struct lcap_ctx *ctx)
{
    int rc;

    ctx->cc_cli = zmq_socket(ctx->cc_zctx, ZMQ_ROUTER);
    if (ctx->cc_cli == NULL) {
        rc = -errno;
        lcaplog_err("Opening external publication socket: %s", strerror(-rc));
        return rc;
    }

    rc = zmq_bind(ctx->cc_cli, SERVER_BIND_URL);
    if (rc) {
        rc = -errno;
        lcaplog_err("Binding to %s: %s", SERVER_BIND_URL, strerror(-rc));
        return rc;
    }

    lcaplog_dbg("Ready to accept requests on %s", SERVER_BIND_URL);
    return 0;
}

/**
 * Setup a lcapd context. This actually means start the whole lcapd server. The
 * start sequence is the following:
 *  1. Start the signal handler (done before calling this function)
 *  2. Initialize the logging infrastructure
 *  3. Prepare to accepting worker threads
 *  4. Start worker threads
 *  5. Start changelog reader threads
 *  6. Prepare to accepting clients
 *
 * A short artificial delay is introduced between steps 4 and 5 to make sure
 * all internal changelog processing threads have been started by the time we
 * start reading the records.
 * There is no need for such a delay between 5 and 6 as we can simply block
 * clients if no record have been read yet.
 */
static int lcap_ctx_init(struct lcap_ctx *ctx, struct lcap_cfg *config)
{
    int rc;

    memset(ctx, 0x00, sizeof(*ctx));

    ctx->cc_config = config;

    lcap_set_loglevel(config->ccf_verbosity);

    rc = lcap_set_logger(config->ccf_loggername);
    if (rc) {
        fprintf(stderr, "Invalid logger type: %s\n", config->ccf_loggername);
        return rc;
    }

    lcap_log_open();

    rc = lcap_setup_tasks_ventilator(ctx);
    if (rc)
        return rc;

    rc = lcap_workers_start(ctx);
    if (rc)
        return rc;

    lcaplog_dbg("Sleeping %u msec to let workers join", SLOW_JOINER_PAUSE);
    usleep(SLOW_JOINER_PAUSE * 1000);

    rc = lcap_readers_start(ctx);
    if (rc)
        return rc;

    rc = lcap_bind_server(ctx);
    if (rc)
        return rc;

    return 0;
}

static int lcap_ctx_release(struct lcap_ctx *ctx)
{
    int rc;

    if (ctx->cc_cli)
        zmq_close(ctx->cc_cli);

    rc = lcap_module_unload_external(ctx);
    if (rc)
        return rc;

    if (ctx->cc_wrk)
        zmq_close(ctx->cc_wrk);

    if (ctx->cc_zctx)
        zmq_ctx_destroy(ctx->cc_zctx);

    free(ctx->cc_wrk_info);
    free(ctx->cc_rdr_info);

    lcap_log_close();

    return 0;
}

void usage(void)
{
    fprintf(stderr, "Usage: lcap [opt]\n");
    fprintf(stderr, "  -c <filename>    alternative configuration file\n");
    fprintf(stderr, "  -o               exit at changelog EOF\n");
    fprintf(stderr, "  -v               increase verbosity level\n");
    fprintf(stderr, "  -h               display this help and exit\n");
}

static int lcap_workers_terminate(struct lcap_ctx *ctx)
{
    const struct lcap_cfg   *cfg = ctx_config(ctx);
    int                      i;

    for (i = 0; i < cfg->ccf_worker_count; i++)
        ctx->cc_wrk_info[i].si_running = false;

    lcaplog_dbg("Signaled termination to all workers");

    for (i = 0; i < cfg->ccf_worker_count; i++)
        pthread_join(ctx->cc_wrk_info[i].si_thread, NULL);

    return 0;
}

static int lcap_readers_terminate(struct lcap_ctx *ctx)
{
    const struct lcap_cfg   *cfg = ctx_config(ctx);
    int                      i;

    for (i = 0; i < cfg->ccf_mdtcount; i++)
        ctx->cc_rdr_info[i].si_running = false;

    lcaplog_dbg("Signaled termination to all readers");

    for (i = 0; i < cfg->ccf_mdtcount; i++)
        pthread_join(ctx->cc_rdr_info[i].si_thread, NULL);

    return 0;
}

static void lcap_sigterm(int signo)
{
    TerminateSig = 1;
}

static void lcap_sigreload(int signo)
{
    ReloadSig = 1;
}

static void lcap_sigdumpstats(int signo)
{
    DumpStatsSig = 1;
}

struct sigmap {
    int           signo;
    const char   *signame;
    void        (*handler)(int);
};

static void *setup_sighandlers(void *args)
{
    struct lcap_ctx *ctx = (struct lcap_ctx *)args;
    struct sigmap    handlers[] = {
        {SIGTERM, "SIGTERM", lcap_sigterm},
        {SIGINT,  "SIGINT",  lcap_sigterm},
        {SIGHUP,  "SIGHUP",  lcap_sigreload},
        {SIGUSR1, "SIGUSR1", lcap_sigdumpstats},
        {-1, NULL, NULL}
    };
    int              i;
    int              rc;

    /* Reset signal handling flags. */
    TerminateSig = 0;
    ReloadSig = 0;
    DumpStatsSig = 0;

    for (i = 0; handlers[i].signo != -1; i++) {
        struct sigaction sa;
    
        memset(&sa, 0, sizeof(sa));
        sa.sa_flags = 0;
        sa.sa_handler = handlers[i].handler;
        sigemptyset(&sa.sa_mask);

        rc = sigaction(handlers[i].signo, &sa, NULL);
        if (rc) {
            rc = -errno;
            fprintf(stderr, "Can't setup signal handler for %s: %s\n",
                    handlers[i].signame, strerror(-rc));
            exit(1);
        }
    }

    sleep(1);

    for (;;) {
        if (TerminateSig) {
            lcaplog_dbg("Received termination signal: quitting.");
            lcap_readers_terminate(ctx);   /* stop reading changelog rec */
            lcap_workers_terminate(ctx);   /* stop delivering changelogs */
            exit(1);
        } else if (ReloadSig) {
            lcaplog_dbg("Received SIGHUP: reloading configuration");
            ReloadSig = 0;
        } else if (DumpStatsSig) {
            lcaplog_dbg("Received SIGUSR1: dumping current statistics "
                        "(not implemented)");
            DumpStatsSig = 0;
        }
        sleep(SIGSLEEP_DELAY);
    }

    return NULL;
}

static int setup_sigthread(struct lcap_ctx *ctx)
{
    int rc;

    rc = pthread_create(&sigh_thr, NULL, setup_sighandlers, ctx);
    if (rc) {
        fprintf(stderr, "Can't start signal handling thread: %s\n",
                strerror(rc));
        return -rc;
    }
    return 0;
}

int main(int ac, char **av)
{
    int rc;
    struct lcap_cfg config;
    struct lcap_ctx ctx;

    if (ac < 2) {
        usage();
        return 1;
    }

    rc = lcap_cfg_init(ac, av, &config);
    if (rc) {
        fprintf(stderr, "Can't load configuration: %s\n", strerror(-rc));
        return rc;
    }

    rc = setup_sigthread(&ctx);
    if (rc)
        return rc;

    rc = lcap_ctx_init(&ctx, &config);
    if (rc)
        return rc;

    zmq_proxy(ctx.cc_cli, ctx.cc_wrk, NULL);

    sleep(1);
    lcap_ctx_release(&ctx);
    lcap_cfg_release(&config);

    return rc;
}

