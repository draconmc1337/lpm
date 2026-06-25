#include "llpm/trans.h"
#include "llpm/handle.h"
#include "llpm/error.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

/* ── llpm_trans_init ─────────────────────────────────────────────────── */
llpm_trans_t *llpm_trans_init(llpm_handle_t *h, int flags) {
    if (!h) return NULL;
    llpm_trans_t *t = calloc(1, sizeof(*t));
    if (!t) { llpm_set_errno(h, LLPM_ERR_OOM); return NULL; }
    t->h     = h;
    t->flags = flags;
    llpm_set_errno(h, LLPM_ERR_OK);
    return t;
}

/* ── llpm_trans_add_remove ───────────────────────────────────────────── */
int llpm_trans_add_remove(llpm_trans_t *t, const char *name) {
    if (!t || !name || t->nops >= LLPM_TRANS_OP_MAX) return -1;
    llpm_trans_op_t *op = &t->ops[t->nops++];
    op->type = LLPM_TRANS_TYPE_REMOVE;
    strncpy(op->pkg.name, name, LLPM_NAME_MAX - 1);
    return 0;
}

/* ── llpm_trans_prepare ──────────────────────────────────────────────── */
int llpm_trans_prepare(llpm_trans_t *t) {
    if (!t || !t->h) return -1;
    t->nmissing = 0;

    /* If no cb_is_installed registered, skip the check — caller already
     * verified in cmd_remove before building the transaction.           */
    if (t->h->cb_is_installed) {
        for (int i = 0; i < t->nops; i++) {
            llpm_trans_op_t *op = &t->ops[i];
            if (op->type != LLPM_TRANS_TYPE_REMOVE) continue;
            if (!t->h->cb_is_installed(op->pkg.name)) {
                if (t->nmissing < 64)
                    strncpy(t->missing_deps[t->nmissing++],
                            op->pkg.name, LLPM_NAME_MAX - 1);
            }
        }
        if (t->nmissing > 0) {
            llpm_set_errno(t->h, LLPM_ERR_NOT_FOUND);
            return -1;
        }
    }

    t->prepared = 1;
    llpm_set_errno(t->h, LLPM_ERR_OK);
    return 0;
}

/* ── llpm_trans_commit ───────────────────────────────────────────────── */
int llpm_trans_commit(llpm_trans_t *t) {
    if (!t || !t->h) return -1;
    if (!t->prepared) { llpm_set_errno(t->h, LLPM_ERR_STATE); return -1; }

    int done = 0;
    for (int i = 0; i < t->nops; i++) {
        llpm_trans_op_t *op = &t->ops[i];
        if (op->type != LLPM_TRANS_TYPE_REMOVE) continue;

        const char *name = op->pkg.name;
        int nfiles = 0;

        /* 1. remove files via callback */
        if (t->h->cb_files_remove)
            nfiles = t->h->cb_files_remove(name);

        /* 2. remove from DB via callback */
        if (t->h->cb_remove)
            t->h->cb_remove(name);

        /* 3. clean build workspace — no callback needed, pure fs */
        {
            char cmd[600];
            snprintf(cmd, sizeof(cmd),
                     "rm -rf '/var/cache/lpm/%s' 2>/dev/null", name);
            if (system(cmd)) { /* ignore */ }
        }

        op->nfiles_removed = nfiles < 0 ? 0 : nfiles;
        done++;
    }

    t->committed = 1;
    llpm_set_errno(t->h, LLPM_ERR_OK);
    return done;
}

/* ── llpm_trans_release ──────────────────────────────────────────────── */
int llpm_trans_release(llpm_trans_t *t) {
    if (!t) return -1;
    free(t);
    return 0;
}
