#include "lpm.h"

/* ═══════════════════════════════════════════════════════════════════════
 * pkg_merge — safe merge of staged pkgdir into root
 *
 * Flow:
 *   1. Disk space check
 *   2. File-level conflict detection
 *   3. Config file protection (backup + .lpm-new)
 *   4. Merge (cp -a)
 *   5. Record files + update transaction journal
 * ═══════════════════════════════════════════════════════════════════════ */
int pkg_merge(Package *pkg, const char *root, Transaction *tx) {
    /* ── 1. disk space check ── */
    Package *self[1] = { pkg };
    if (safety_check_space(self, 1, root) != 0)
        return -1;

    /* ── 2. file-level conflict detection ── */
    int force = 0;
    if (safety_check_toolchain(pkg->pkg_dir, pkg->name) != 0)
        return -1;
    if (safety_check_file_conflicts(pkg->pkg_dir, pkg->name, force) != 0) {
        fprintf(stderr,
            C_RED "error:" C_RESET
            " file conflicts detected for %s — aborting merge.\n"
            "  Remove conflicting packages first, or rebuild with --force.\n",
            pkg->name);
        return -1;
    }

    /* ── 3. config file protection ── */
    safety_backup_configs(pkg, root);
    safety_protect_configs(pkg, pkg->pkg_dir, root);

    /* ── 4. merge staged pkgdir into root ── */
    char cmd[LPM_PATH_MAX * 2 + 32];
    snprintf(cmd, sizeof(cmd),
        "cp -a --remove-destination '%s'/. '%s'/",
        pkg->pkg_dir, root);

    if (util_run(cmd) != 0) {
        fprintf(stderr,
            C_RED "error:" C_RESET " merge failed for %s\n", pkg->name);
        /* attempt config restore on failure */
        safety_restore_configs(pkg, root);
        return -1;
    }

    /* ── 5. record file ownership + update transaction ── */
    db_files_save(pkg->name, pkg->pkg_dir);
    pkg->state = PKG_STATE_MERGED;

    /* record in transaction journal for rollback */
    if (tx) {
        /* append merged files to tx->merged_files if capacity allows */
        /* (full rollback journal is handled by transaction.c) */
        (void)tx;
    }

    printf(C_GREEN "  ->" C_RESET " Merged %s into %s\n", pkg->name, root);
    return 0;
}
