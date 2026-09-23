/*
 * ui.c — shared, thread-safe output writer for lpm's runtime UX.
 *
 * Design: every logical output block (a line, or a multi-line checkpoint) is
 * formatted into a private buffer and emitted under a single global mutex with
 * one fwrite. Concurrent workers therefore never interleave character-by-
 * character: each job's line appears atomically at its checkpoint.
 *
 * This ONE writer backs both:
 *   - the "(N/M) Installing/Building" execution lines (ui_out), and
 *   - the --debug=N output (DBG -> ui_dbg -> ui_err),
 * so the install-interleaving fix and the debug-interleaving fix share a
 * single implementation (they are the same underlying writer).
 */
#include "lpm.h"
#include <pthread.h>
#include <stdarg.h>

static pthread_mutex_t g_out_mu = PTHREAD_MUTEX_INITIALIZER;

/* Format into a private buffer, then emit the whole block atomically. */
static void ui_emit(FILE *stream, const char *fmt, va_list ap) {
    char buf[8192];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n < 0) return;
    size_t len = ((size_t)n >= sizeof(buf)) ? sizeof(buf) - 1 : (size_t)n;
    pthread_mutex_lock(&g_out_mu);
    fwrite(buf, 1, len, stream);
    fflush(stream);
    pthread_mutex_unlock(&g_out_mu);
}

void ui_out(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); ui_emit(stdout, fmt, ap); va_end(ap);
}

void ui_err(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); ui_emit(stderr, fmt, ap); va_end(ap);
}

/* Debug sink used by the DBG macro: one atomic block per message. */
void ui_dbg(int level, const char *prefix, const char *fmt, ...) {
    if (g_debug < level) return;
    char msg[8192];
    va_list ap; va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    ui_err("%s%s\n", prefix, msg);
}

/* ── shared transaction output fragments (used by install/remove/upgrade/
 *    bootstrap so the vocabulary stays identical across commands) ──────── */

void ui_sig_block(void) {
    ui_out(":: Importing key...\n");
    ui_out(":: Verifying signatures...\n");
    ui_out(":: Signatures verified.\n\n");
}

/* One row of the pre-confirm summary table. tag is e.g. "binary N",
 * "source U", "remove", "orphan". repo may be "" (then omitted). */
void ui_pkg_row(const char *tag, const char *repo,
                const char *name, const char *ver) {
    if (repo && repo[0])
        ui_out("[%s] %s/%s-%s\n", tag, repo, name, ver);
    else if (ver && ver[0])
        ui_out("[%s] %s-%s\n", tag, name, ver);
    else
        ui_out("[%s] %s\n", tag, name);
}
