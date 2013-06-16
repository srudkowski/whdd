#define _GNU_SOURCE
#include <stdio.h>
#include <wchar.h>
#include <curses.h>
#include <dialog.h>
#include <assert.h>

#include "render.h"
#include "utils.h"
#include "ncurses_convenience.h"
#include "dialog_convenience.h"
#include "procedure.h"
#include "vis.h"

typedef struct blk_report {
    uint64_t seqno;
    DC_BlockReport report;
} blk_report_t;

typedef struct {
    WINDOW *legend; // not for updating, just to free afterwards
    WINDOW *vis; // window to print vis-char for each block
    WINDOW *access_time_stats;
    WINDOW *avg_speed;
    //WINDOW *cur_speed;
    WINDOW *eta;
    //WINDOW *progress;
    WINDOW *summary;
    WINDOW *w_end_lba;
    WINDOW *w_cur_lba;
    WINDOW *w_log;

    struct timespec start_time;
    uint64_t access_time_stats_accum[6];
    uint64_t error_stats_accum[6]; // 0th is unused, the rest are as in DC_BlockStatus enum
    uint64_t avg_processing_speed;
    uint64_t eta_time; // estimated time
    uint64_t cur_lba;

    pthread_t render_thread;
    int order_hangup; // if interrupted or completed, render remainings and end render thread

    // lockless ringbuffer
    blk_report_t reports[100*1000];
    uint64_t next_report_seqno_write;
    uint64_t next_report_seqno_read;
} SlidingWindow;



static void *render_thread_proc(void *arg);
static void blk_rep_write_finalize(SlidingWindow *priv, blk_report_t *rep);
static blk_report_t *blk_rep_get_next_for_write(SlidingWindow *priv);
static void render_update_vis(SlidingWindow *priv, blk_report_t *rep);
static void render_update_stats(SlidingWindow *priv);

static blk_report_t *blk_rep_get_next_for_write(SlidingWindow *priv) {
    blk_report_t *rep = &priv->reports[
        (priv->next_report_seqno_write) % (sizeof(priv->reports) / sizeof(priv->reports[0]))
        ];
    //fprintf(stderr, "giving %p for write\n", rep);
    return rep;
}

static void blk_rep_write_finalize(SlidingWindow *priv, blk_report_t *rep) {
    rep->seqno = priv->next_report_seqno_write;
    priv->next_report_seqno_write++;
    //fprintf(stderr, "mark %p with seqno %"PRIu64", go to next\n", rep, rep->seqno);
}

static blk_report_t *blk_rep_get_unread(SlidingWindow *priv) {
    blk_report_t *rep = &priv->reports[
        priv->next_report_seqno_read % (sizeof(priv->reports) / sizeof(priv->reports[0]))
        ];
    return rep;
}

static blk_report_t *blk_rep_read(SlidingWindow *priv) {
    blk_report_t *rep = blk_rep_get_unread(priv);
    priv->next_report_seqno_read++;
    return rep;
}

static int get_queue_length(SlidingWindow *priv) {
    return priv->next_report_seqno_write - priv->next_report_seqno_read;
}

static void render_queued(SlidingWindow *priv) {
    int queue_length = get_queue_length(priv);
    while (queue_length) {
        blk_report_t *cur_rep = blk_rep_read(priv);
        render_update_vis(priv, cur_rep);
        queue_length--;
    }
    render_update_stats(priv);
    wnoutrefresh(priv->vis);
    doupdate();
}

static void *render_thread_proc(void *arg) {
    SlidingWindow *priv = arg;
    // TODO block signals in priv thread
    while (!priv->order_hangup) {
        render_queued(priv);
        usleep(40000);  // 25 Hz should be nice
    }
    render_queued(priv);
    return NULL;
}

static void render_update_vis(SlidingWindow *priv, blk_report_t *rep) {
    if (rep->report.blk_status)
    {
        print_vis(priv->vis, error_vis[rep->report.blk_status]);
        priv->error_stats_accum[rep->report.blk_status]++;
    }
    else
    {
        print_vis(priv->vis, choose_vis(rep->report.blk_access_time));
        unsigned int i;
        for (i = 0; i < 5; i++)
            if (rep->report.blk_access_time < bs_vis[i].access_time) {
                priv->access_time_stats_accum[i]++;
                break;
            }
        if (i == 5)
            priv->access_time_stats_accum[5]++; // of exceed
    }
    wnoutrefresh(priv->vis);
}

static void render_update_stats(SlidingWindow *priv) {
    werase(priv->access_time_stats);
    unsigned int i;
    for (i = 0; i < 6; i++)
        wprintw(priv->access_time_stats, "%d\n", priv->access_time_stats_accum[i]);
    for (i = 1; i < 6; i++)
        wprintw(priv->access_time_stats, "%d\n", priv->error_stats_accum[i]);
    wnoutrefresh(priv->access_time_stats);

    if (priv->avg_processing_speed != 0) {
        werase(priv->avg_speed);
        wprintw(priv->avg_speed, "SPEED %7"PRIu64" kb/s", priv->avg_processing_speed / 1024);
        wnoutrefresh(priv->avg_speed);
    }

    if (priv->eta_time != 0) {
        unsigned int minute, second;
        second = priv->eta_time % 60;
        minute = priv->eta_time / 60;
        werase(priv->eta);
        wprintw(priv->eta, "ETA %11u:%02u", minute, second);
        wnoutrefresh(priv->eta);
    }

    werase(priv->w_cur_lba);
    char comma_lba_buf[30], *comma_lba_p;
    comma_lba_p = commaprint(priv->cur_lba, comma_lba_buf, sizeof(comma_lba_buf));
    wprintw(priv->w_cur_lba, "LBA: %14s", comma_lba_p);
    wnoutrefresh(priv->w_cur_lba);
}

static int Open(DC_RendererCtx *ctx) {
    SlidingWindow *priv = ctx->priv;
    DC_ProcedureCtx *actctx = ctx->procedure_ctx;

    priv->legend = derwin(stdscr, 11 /* legend win height */, LEGEND_WIDTH/2, 4, COLS-LEGEND_WIDTH); // leave 1st and last lines untouched
    assert(priv->legend);
    priv->access_time_stats = derwin(stdscr, 11 /* height */, LEGEND_WIDTH/2, 4, COLS-LEGEND_WIDTH/2);
    assert(priv->access_time_stats);
    show_legend(priv->legend);
    priv->vis = derwin(stdscr, LINES-5, COLS-LEGEND_WIDTH-1, 2, 0); // leave 1st and last lines untouched
    assert(priv->vis);
    scrollok(priv->vis, TRUE);
    wrefresh(priv->vis);

    priv->avg_speed = derwin(stdscr, 1, LEGEND_WIDTH, 2, COLS-LEGEND_WIDTH);
    assert(priv->avg_speed);

    priv->eta = derwin(stdscr, 1, LEGEND_WIDTH, 1, COLS-LEGEND_WIDTH);
    assert(priv->eta);

    priv->summary = derwin(stdscr, 10, LEGEND_WIDTH, 16, COLS-LEGEND_WIDTH);
    assert(priv->summary);

    priv->w_end_lba = derwin(stdscr, 1, 20, 1, COLS-41);
    assert(priv->w_end_lba);

    priv->w_cur_lba = derwin(stdscr, 1, 20, 1, COLS-61);
    assert(priv->w_cur_lba);

    priv->w_log = derwin(stdscr, 2, COLS, LINES-3, 0);
    assert(priv->w_log);
    scrollok(priv->w_log, TRUE);

    priv->reports[0].seqno = 1; // anything but zero

    char comma_lba_buf[30], *comma_lba_p;
    comma_lba_p = commaprint(actctx->dev->capacity / 512, comma_lba_buf, sizeof(comma_lba_buf));
    wprintw(priv->w_end_lba, "/ %s", comma_lba_p);
    wnoutrefresh(priv->w_end_lba);
    wprintw(priv->summary,
            "%s %s bs=%d\n"
            "Ctrl+C to abort\n",
            actctx->procedure->name, actctx->dev->dev_path, actctx->blk_size);
    wrefresh(priv->summary);
    int r = pthread_create(&priv->render_thread, NULL, render_thread_proc, priv);
    if (r)
        return r; // FIXME leak
    return 0;
}

static int HandleReport(DC_RendererCtx *ctx) {
    int r;
    SlidingWindow *priv = ctx->priv;
    DC_ProcedureCtx *actctx = ctx->procedure_ctx;

    uint64_t bytes_processed = actctx->report.lba * 512;
    if (bytes_processed > actctx->dev->capacity)
        bytes_processed = actctx->dev->capacity;
    priv->cur_lba = actctx->report.lba;

    if (actctx->progress.num == 1) {  // TODO fix priv hack
        r = clock_gettime(DC_BEST_CLOCK, &priv->start_time);
        assert(!r);
    } else {
        if ((actctx->progress.num % 10) == 0) {
            struct timespec now;
            r = clock_gettime(DC_BEST_CLOCK, &now);
            assert(!r);
            uint64_t time_elapsed_ms = now.tv_sec * 1000 + now.tv_nsec / (1000*1000)
                - priv->start_time.tv_sec * 1000 - priv->start_time.tv_nsec / (1000*1000);
            if (time_elapsed_ms > 0) {
                priv->avg_processing_speed = bytes_processed * 1000 / time_elapsed_ms; // Byte/s
                // capacity / speed = total_time
                // total_time = elapsed + eta
                // eta = total_time - elapsed
                // eta = capacity / speed  -  elapsed
                priv->eta_time = actctx->dev->capacity / priv->avg_processing_speed - time_elapsed_ms / 1000;

            }
        }
    }

    // enqueue block report
    blk_report_t *rep = blk_rep_get_next_for_write(priv);
    assert(rep);
    rep->report = actctx->report;
    blk_rep_write_finalize(priv, rep);
    //fprintf(stderr, "finalized %"PRIu64"\n", priv->next_report_seqno_write-1);
    return 0;
}

static void Close(DC_RendererCtx *ctx) {
    SlidingWindow *priv = ctx->priv;
    DC_ProcedureCtx *actctx = ctx->procedure_ctx;

    priv->order_hangup = 1;
    pthread_join(priv->render_thread, NULL);
    if (actctx->interrupt)
        wprintw(priv->summary, "Aborted.\n");
    else
        wprintw(priv->summary, "Completed.\n");
    wprintw(priv->summary, "Press any key");
    wrefresh(priv->summary);
    beep();
    getch();
    delwin(priv->legend);
    delwin(priv->access_time_stats);
    delwin(priv->vis);
    delwin(priv->avg_speed);
    delwin(priv->eta);
    delwin(priv->summary);
    delwin(priv->w_end_lba);
    delwin(priv->w_cur_lba);
    delwin(priv->w_log);
    clear_body();
}

DC_Renderer sliding_window = {
    .name = "sliding_window",
    .open = Open,
    .handle_report = HandleReport,
    .close = Close,
    .priv_data_size = sizeof(SlidingWindow),
};