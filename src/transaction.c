#include "lpm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

/* ── create new transaction ──────────────────────────────────────────────── */
Transaction *tx_new(void) {
    Transaction *tx = calloc(1, sizeof(Transaction));
    if (!tx) return NULL;
    tx->merged_files = calloc(LPM_MAX_FILES, sizeof(*tx->merged_files));
    if (!tx->merged_files) { free(tx); return NULL; }
    tx->committed = 0;
    return tx;
}

void tx_free(Transaction *tx) {
    if (!tx) return;
    free(tx->merged_files);
    free(tx->install);
    free(tx->remove);
    free(tx->upgrade);
    free(tx);
}

int tx_add_install(Transaction *tx, Package *pkg) {
    tx->install = realloc(tx->install,
                          (tx->ninstall + 1) * sizeof(Package *));
    tx->install[tx->ninstall++] = pkg;
    return 0;
}

int tx_add_remove(Transaction *tx, Package *pkg) {
    tx->remove = realloc(tx->remove,
                         (tx->nremove + 1) * sizeof(Package *));
    tx->remove[tx->nremove++] = pkg;
    return 0;
}

/* ── commit: merge all staged packages into root ─────────────────────────── */
int tx_commit(Transaction *tx, const char *root) {
    if (tx->committed) return 0;

    for (int i = 0; i < tx->ninstall; i++) {
        Package *pkg = tx->install[i];

        /* run pre_install hook */
        if (pkg->has_pre_install)
            pkg_run_hook("pre_install", pkg);

        /* backup config files */
        safety_backup_configs(pkg, root);

        /* merge pkgdir into root */
        if (pkg_merge(pkg, root, tx) != 0) {
            fprintf(stderr,
                C_RED "error:" C_RESET
                " merge failed for %s — rolling back\n", pkg->name);
            tx_rollback(tx, root);
            return -1;
        }

        /* record install in db */
        if (db_record_install(pkg, root) != 0) {
            fprintf(stderr,
                C_RED "error:" C_RESET
                " db record failed for %s\n", pkg->name);
            tx_rollback(tx, root);
            return -1;
        }

        pkg->state = PKG_STATE_MERGED;

        /* run post_install hook */
        if (pkg->has_post_install)
            pkg_run_hook("post_install", pkg);

        printf(C_GREEN "==> Installed" C_RESET " %s %s-%s\n",
               pkg->name, pkg->version, pkg->release);
    }

    tx->committed = 1;
    return 0;
}

/* ── rollback: undo everything merged so far ─────────────────────────────── */
int tx_rollback(Transaction *tx, const char *root) {
    if (tx->committed) return 0;
    if (tx->nmerged == 0) return 0;

    printf(C_YELLOW "::" C_RESET " Rolling back transaction (%d files)...\n",
           tx->nmerged);

    /* remove in reverse order */
    for (int i = tx->nmerged - 1; i >= 0; i--) {
        char full[LPM_PATH_MAX * 2];
        snprintf(full, sizeof(full), "%s/%s", root, tx->merged_files[i]);

        if (unlink(full) != 0 && errno != ENOENT) {
            fprintf(stderr,
                C_YELLOW "warning:" C_RESET
                " rollback: cannot remove %s: %s\n",
                full, strerror(errno));
        }
    }

    /* restore configs for all packages attempted */
    for (int i = 0; i < tx->ninstall; i++) {
        Package *pkg = tx->install[i];
        if (pkg->state >= PKG_STATE_MERGED)
            safety_restore_configs(pkg, root);
    }

    printf(C_YELLOW "::" C_RESET " Rollback complete.\n");
    return 0;
}
