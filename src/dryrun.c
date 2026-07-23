/*
 * dryrun.c — Atomic transaction preview for lpm --dry-run
 *
 * Simulates a transaction (install/upgrade/remove) and prints exactly
 * what would happen. Zero side effects.
 */
#define _XOPEN_SOURCE 700
#include "lpm.h"
#include <stddef.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
#pragma GCC diagnostic ignored "-Wunused-result"

/* ── dryrun_print ────────────────────────────────────────────────────── */
void dryrun_print(const DryRun *dr) {
    if (dr->nops == 0) {
        printf("There is nothing to do.\n");
        return;
    }

    int ninstall = 0, nupgrade = 0, nremove = 0;
    for (int i = 0; i < dr->nops; i++) {
        if (dr->ops[i].type == DRY_INSTALL) ninstall++;
        else if (dr->ops[i].type == DRY_UPGRADE) nupgrade++;
        else nremove++;
    }

    printf("Dry run\n\n");
    printf("No changes will be made.\n\n");

    if (ninstall > 0 || nupgrade > 0) {
        printf("Packages to install:\n\n");
        for (int i = 0; i < dr->nops; i++) {
            const DryOp *op = &dr->ops[i];
            if (op->type == DRY_REMOVE) continue;
            if (op->type == DRY_UPGRADE && op->from_ver[0])
                printf("%s  %s -> %s\n", op->name, op->from_ver, op->to_ver);
            else if (op->to_ver[0])
                printf("%s-%s\n", op->name, op->to_ver);
            else
                printf("%s\n", op->name);
        }
        printf("\n");
    }

    if (nremove > 0) {
        printf("Packages to remove:\n\n");
        for (int i = 0; i < dr->nops; i++) {
            const DryOp *op = &dr->ops[i];
            if (op->type != DRY_REMOVE) continue;
            printf("%s\n", op->name);
        }
        printf("\n");
    }

    long installed = 0, freed = 0;
    if (dr->total_inst_delta > 0)
        installed = dr->total_inst_delta;
    else if (dr->total_inst_delta < 0)
        freed = -dr->total_inst_delta;

    char tmp[32];
    if (installed > 0) {
        format_size(installed, tmp, sizeof(tmp));
        printf("Installed size:\n%s\n\n", tmp);
    }
    if (freed > 0) {
        format_size(freed, tmp, sizeof(tmp));
        printf("Freed space:\n%s\n\n", tmp);
    }
    if (installed > 0 || freed > 0) {
        long net = installed - freed;
        format_size(net >= 0 ? net : -net, tmp, sizeof(tmp));
        printf("Net change:\n%s%s\n", net >= 0 ? "+" : "-", tmp);
    }

    if (dr->total_dl > 0) {
        format_size(dr->total_dl, tmp, sizeof(tmp));
        printf("\nDownload size:\n%s\n", tmp);
    }
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

    for (int qi = 0; qi < nqueue && dr->nops < 256; qi++) {
        const char *name = queue[qi];

        DryOp *op = &dr->ops[dr->nops++];
        memset(op, 0, sizeof(*op));
        snprintf(op->name, sizeof(op->name), "%s", name);

        char *inst_ver = db_get_version(name);
        if (inst_ver) {
            op->type = DRY_UPGRADE;
            snprintf(op->from_ver, sizeof(op->from_ver), "%s", inst_ver);
            free(inst_ver);
        } else {
            op->type = DRY_INSTALL;
        }

        char pbfile[LPM_PATH_MAX + LPM_NAME_MAX + 16];
        snprintf(pbfile, sizeof(pbfile),
                 "%s/pkgbuild_%s", LPM_PKGBUILD_DIR, name);
        Package meta;
        memset(&meta, 0, sizeof(meta));
        if (pkgbuild_parse_fast(pbfile, &meta) == 0) {
            snprintf(op->to_ver, sizeof(op->to_ver), "%s",
                     meta.version[0] ? meta.version : "?");

            op->dl_bytes   = meta.dl_size > 0 ? meta.dl_size :
                             ((meta.type == PKG_TYPE_BINARY) ? 30*1024*1024 : 10*1024*1024);
            op->inst_bytes = meta.inst_size > 0 ? meta.inst_size :
                             ((meta.type == PKG_TYPE_BINARY) ? 80*1024*1024 : 50*1024*1024);

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

        dr->total_dl         += op->dl_bytes;
        dr->total_inst_delta += op->inst_bytes;
    }

    for (int i = 0; i < npkgs; i++) {
        int found = 0;
        for (int j = 0; j < dr->nops; j++)
            if (!strcmp(dr->ops[j].name, pkgnames[i])) { found = 1; break; }
        if (found) continue;

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

/* ── dryrun_remove ───────────────────────────────────────────────────── */
int dryrun_remove(char **pkgnames, int npkgs, DryRun *dr) {
    memset(dr, 0, sizeof(*dr));

    for (int i = 0; i < npkgs && dr->nops < 256; i++) {
        if (!db_is_installed(pkgnames[i])) {
            warn("'%s' is not installed", pkgnames[i]);
            continue;
        }
        DryOp *op = &dr->ops[dr->nops++];
        memset(op, 0, sizeof(*op));
        snprintf(op->name, sizeof(op->name), "%s", pkgnames[i]);
        op->type = DRY_REMOVE;
        char *v = db_get_version(pkgnames[i]);
        if (v) { snprintf(op->from_ver, sizeof(op->from_ver), "%s", v); free(v); }

        InstalledPkg ip; memset(&ip, 0, sizeof(ip));
        if (db_query(pkgnames[i], &ip) == 0 && ip.install_size > 0)
            op->inst_bytes = -(long)ip.install_size;
        else
            op->inst_bytes = -50*1024*1024;
        dr->total_inst_delta += op->inst_bytes;
    }
    return 0;
}

#pragma GCC diagnostic pop
