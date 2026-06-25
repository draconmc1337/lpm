#include "lpm.h"
#include <dirent.h>

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

    /* ── 2b. symlink attack guard ── */
    {
        DIR *d = opendir(pkg->pkg_dir);
        if (d) {
            struct dirent *ent;
            while ((ent = readdir(d))) {
                if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
                char src[LPM_PATH_MAX * 2], dst[LPM_PATH_MAX];
                snprintf(src, sizeof(src), "%s/%s", pkg->pkg_dir, ent->d_name);
                snprintf(dst, sizeof(dst), "/%s", ent->d_name);
                if (safety_guard_symlinks(src, dst) != 0) {
                    closedir(d);
                    return -1;
                }
            }
            closedir(d);
        }
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

    /* record merged files into transaction journal for rollback */
    if (tx) {
        char listpath[LPM_PATH_MAX + 64];
        snprintf(listpath, sizeof(listpath), "%s/%s/files.list",
                 LPM_FILES_DIR, pkg->name);
        FILE *flp = fopen(listpath, "r");
        if (flp) {
            char line[LPM_PATH_MAX];
            while (fgets(line, sizeof(line), flp) &&
                   tx->nmerged < LPM_MAX_FILES) {
                line[strcspn(line, "\n")] = '\0';
                if (!line[0]) continue;
                strncpy(tx->merged_files[tx->nmerged], line, LPM_PATH_MAX - 1);
                tx->merged_files[tx->nmerged][LPM_PATH_MAX - 1] = '\0';
                tx->nmerged++;
            }
            fclose(flp);
        }
    }

    printf(C_GREEN "  ->" C_RESET " Merged %s into %s\n", pkg->name, root);
    return 0;
}
