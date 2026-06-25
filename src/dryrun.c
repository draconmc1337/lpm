/*
 * dryrun.c — Atomic transaction preview for lpm --dry-run
 *
 * Simulates a transaction (install/upgrade/remove) and prints exactly
 * what would happen: targets, version changes, conflicts, hooks,
 * download size, disk usage delta.  Zero side effects — nothing is
 * written to disk, no package is installed or removed.
 *
 * Usage:
 *   lpm install firefox --dry-run
 *   lpm install --dry-run firefox mesa
 *   lpm remove --dry-run gtk3
 */
#define _XOPEN_SOURCE 700
#include "lpm.h"
#include <stddef.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
#pragma GCC diagnostic ignored "-Wunused-result"

/* ── format helpers ──────────────────────────────────────────────────── */
static void fmt_size(long bytes, char *buf, size_t sz) {
    if      (bytes >= (long)1024*1024*1024)
        snprintf(buf, sz, "%.1f GB", (double)bytes / (1024.0*1024*1024));
    else if (bytes >= 1024*1024)
        snprintf(buf, sz, "%.1f MB", (double)bytes / (1024.0*1024));
    else if (bytes >= 1024)
        snprintf(buf, sz, "%.1f KB", (double)bytes / 1024.0);
    else
        snprintf(buf, sz, "%ld B", bytes);
}

/* ── dryrun_print ────────────────────────────────────────────────────── */
void dryrun_print(const DryRun *dr) {
    if (dr->nops == 0) {
        printf(":: Nothing to do.\n");
        return;
    }

    /* ── group ops by type for the header ── */
    int ninstall = 0, nupgrade = 0, nremove = 0;
    for (int i = 0; i < dr->nops; i++) {
        if (dr->ops[i].type == DRY_INSTALL) ninstall++;
        else if (dr->ops[i].type == DRY_UPGRADE) nupgrade++;
        else nremove++;
    }

    /* ── header: pick label based on dominant op ── */
    if (ninstall > 0 || nupgrade > 0) {
        int ntotal = ninstall + nupgrade;
        printf("\nPackages to install (%d):\n", ntotal);
    } else {
        printf("\nPackages to remove (%d):\n", nremove);
    }

    /* ── per-target lines ── */
    int nconflicts = 0;
    int nhooks     = 0;
    char conflicts[32][LPM_NAME_MAX * 2 + 8];
    char hooks[32][LPM_NAME_MAX];

    for (int i = 0; i < dr->nops; i++) {
        const DryOp *op = &dr->ops[i];

        /* type label */
        const char *type_tag;
        switch (op->type) {
        case DRY_UPGRADE: type_tag = "upgrade"; break;
        case DRY_REMOVE:  type_tag = "remove";  break;
        default:          type_tag = "binary";  break;
        }

        /* version string */
        char ver_str[LPM_VER_MAX * 2 + 8] = "";
        if (op->type == DRY_UPGRADE && op->from_ver[0])
            snprintf(ver_str, sizeof(ver_str),
                     "-%s -> %s", op->from_ver, op->to_ver);
        else if (op->to_ver[0])
            snprintf(ver_str, sizeof(ver_str), "-%s", op->to_ver);

        printf(" %s%s (%s)", op->name, ver_str, type_tag);
        if (op->dl_bytes > 0) {
            char sz[24];
            fmt_size(op->dl_bytes, sz, sizeof(sz));
            printf("  %s", sz);
        }
        printf("\n");

        /* collect conflicts and hooks */
        if (op->conflict_with[0] && nconflicts < 32)
            snprintf(conflicts[nconflicts++], sizeof(conflicts[0]),
                     "%s (conflicts with %s)", op->name, op->conflict_with);
        if (op->hook[0] && nhooks < 32)
            snprintf(hooks[nhooks++], sizeof(hooks[0]), "%s", op->hook);
    }

    /* ── sizes ── */
    printf("\n");
    if (dr->total_dl > 0) {
        char dl_str[24];
        fmt_size(dr->total_dl, dl_str, sizeof(dl_str));
        printf("Download:   %s\n", dl_str);
    }
    if (dr->total_inst_delta != 0) {
        char tmp[24];
        if (dr->total_inst_delta >= 0) {
            fmt_size(dr->total_inst_delta, tmp, sizeof(tmp));
            printf("Installed:  %s\n", tmp);
        } else {
            fmt_size(-dr->total_inst_delta, tmp, sizeof(tmp));
            printf("Freed:      %s\n", tmp);
        }
    }

    /* ── conflicts ── */
    if (nconflicts > 0) {
        printf("\nConflicts:\n");
        for (int i = 0; i < nconflicts; i++)
            printf(" ! %s\n", conflicts[i]);
    }

    /* ── hooks ── */
    if (nhooks > 0) {
        printf("\nHooks:\n");
        /* deduplicate */
        for (int i = 0; i < nhooks; i++) {
            int dup = 0;
            for (int j = 0; j < i; j++)
                if (!strcmp(hooks[i], hooks[j])) { dup = 1; break; }
            if (!dup)
                printf(" >> %s\n", hooks[i]);
        }
    }

    printf("\n(dry run — no changes were made)\n\n");
}

/* ── dryrun_build ────────────────────────────────────────────────────── *
 * Simulate an install/upgrade transaction for pkgnames[0..npkgs-1].    *
 * Reads repo index + local DB but writes nothing.                       *
 * Returns 0 on success (even if conflicts exist), -1 on fatal error.   */
int dryrun_build(char **pkgnames, int npkgs, DryRun *dr) {
    memset(dr, 0, sizeof(*dr));

    /* resolve full dep queue using existing dep engine */
    char queue[256][MAX_STR];
    int  nqueue = dep_resolve_queue_multi(pkgnames, npkgs,
                                          queue, 256, 0 /*skip installed*/);

    /* well-known post-install hooks: triggered by certain packages */
    static const struct { const char *pkg; const char *hook; } HOOKS[] = {
        { "gtk2",             "gtk-update-icon-cache"       },
        { "gtk3",             "gtk-update-icon-cache"       },
        { "gtk4",             "gtk-update-icon-cache"       },
        { "hicolor-icon-theme","gtk-update-icon-cache"      },
        { "glib2",            "glib-compile-schemas"        },
        { "dbus",             "dbus-reload"                 },
        { "systemd",          "systemd-daemon-reload"       },
        { "fontconfig",       "fc-cache"                    },
        { "desktop-file-utils","update-desktop-database"   },
        { "shared-mime-info", "update-mime-database"        },
        { NULL, NULL }
    };

    for (int qi = 0; qi < nqueue && dr->nops < 256; qi++) {
        const char *name = queue[qi];

        DryOp *op = &dr->ops[dr->nops++];
        memset(op, 0, sizeof(*op));
        snprintf(op->name, sizeof(op->name), "%s", name);

        /* installed? → upgrade; fresh → install */
        char *inst_ver = db_get_version(name);
        if (inst_ver) {
            op->type = DRY_UPGRADE;
            snprintf(op->from_ver, sizeof(op->from_ver), "%s", inst_ver);
            free(inst_ver);
        } else {
            op->type = DRY_INSTALL;
        }

        /* fetch PKGBUILD metadata for version + size estimates */
        char pbfile[LPM_PATH_MAX + LPM_NAME_MAX + 16];
        snprintf(pbfile, sizeof(pbfile),
                 "%s/pkgbuild_%s", LPM_PKGBUILD_DIR, name);
        PkgMeta meta;
        memset(&meta, 0, sizeof(meta));
        if (pkgbuild_parse_fast(pbfile, &meta) == 0) {
            char ver[LPM_VER_MAX + 16];
            snprintf(ver, sizeof(ver), "%s-%s", meta.pkgver, meta.pkgrel);
            snprintf(op->to_ver, sizeof(op->to_ver), "%s", ver);

            /* size: rough heuristic — source pkgs ~10 MB dl, binary varies */
            op->dl_bytes   = meta.is_binary ? 30*1024*1024 : 10*1024*1024;
            op->inst_bytes = meta.is_binary ? 80*1024*1024 : 50*1024*1024;

            /* conflict detection */
            for (int ci = 0; ci < meta.nconflicts; ci++) {
                if (db_is_installed(meta.conflicts[ci])) {
                    snprintf(op->conflict_with, sizeof(op->conflict_with),
                             "%s", meta.conflicts[ci]);
                    break;
                }
            }
        } else {
            snprintf(op->to_ver, sizeof(op->to_ver), "?");
        }

        /* hook detection */
        for (int h = 0; HOOKS[h].pkg; h++) {
            if (!strcmp(name, HOOKS[h].pkg)) {
                snprintf(op->hook, sizeof(op->hook), "%s", HOOKS[h].hook);
                break;
            }
        }

        dr->total_dl         += op->dl_bytes;
        dr->total_inst_delta += op->inst_bytes;
    }

    /* add explicitly requested targets that were already installed
     * (dep resolver skips them, but user listed them explicitly) */
    for (int i = 0; i < npkgs; i++) {
        /* check if already in ops */
        int found = 0;
        for (int j = 0; j < dr->nops; j++)
            if (!strcmp(dr->ops[j].name, pkgnames[i])) { found = 1; break; }
        if (found) continue;

        /* already installed — show as upgrade if version differs */
        if (db_is_installed(pkgnames[i]) && dr->nops < 256) {
            DryOp *op = &dr->ops[dr->nops++];
            memset(op, 0, sizeof(*op));
            snprintf(op->name, sizeof(op->name), "%s", pkgnames[i]);
            op->type = DRY_UPGRADE;
            char *v = db_get_version(pkgnames[i]);
            if (v) { snprintf(op->from_ver, sizeof(op->from_ver), "%s", v); free(v); }
            snprintf(op->to_ver, sizeof(op->to_ver), "(already installed)");
        }
    }

    return 0;
}

/* ── dryrun_remove ───────────────────────────────────────────────────── *
 * Simulate a removal transaction.                                       */
int dryrun_remove(char **pkgnames, int npkgs, DryRun *dr) {
    memset(dr, 0, sizeof(*dr));

    for (int i = 0; i < npkgs && dr->nops < 256; i++) {
        if (!db_is_installed(pkgnames[i])) {
            fprintf(stderr, C_YELLOW "warning: " C_RESET
                    "'%s' is not installed\n", pkgnames[i]);
            continue;
        }
        DryOp *op = &dr->ops[dr->nops++];
        memset(op, 0, sizeof(*op));
        snprintf(op->name, sizeof(op->name), "%s", pkgnames[i]);
        op->type = DRY_REMOVE;
        char *v = db_get_version(pkgnames[i]);
        if (v) { snprintf(op->from_ver, sizeof(op->from_ver), "%s", v); free(v); }
        /* inst_bytes negative = freeing disk space */
        op->inst_bytes = -50*1024*1024; /* heuristic */
        dr->total_inst_delta += op->inst_bytes;
    }
    return 0;
}

#pragma GCC diagnostic pop
