/*
 * verify.c — `lpm verify` : real system integrity verification.
 *
 * NOT to be confused with `lpm test` (formerly `lpm verify`, which ran
 * a PKGBUILD's check() function — that's a package *test suite*, not a
 * system check. See cmd_check() in build.c, now wired to `lpm test`).
 *
 * Levels implemented:
 *   1. Existence   — every file recorded in files.list still exists
 *   2. Checksum    — sha256 of regular files matches what was recorded
 *                     at install time (files.meta)
 *   3. Permission / ownership — mode bits (incl. setuid/setgid/sticky),
 *                     uid, gid match what was recorded
 *   4. --deps      — every dependency of every installed package is
 *                     itself installed (basic consistency check)
 *
 * Usage:
 *   lpm verify              verify all installed packages
 *   lpm verify <pkg...>     verify specific package(s)
 *   lpm verify --deps       check dependency consistency across the system
 */

#include "lpm.h"

#define VERIFY_MAX_ISSUES 32

typedef struct {
    int total;
    int missing;
    int modified;
    int perm_mismatch;
    int owner_mismatch;
    int no_meta;   /* package predates files.meta — checks 2/3 skipped */

    char missing_paths[VERIFY_MAX_ISSUES][LPM_PATH_MAX];
    int  n_missing_paths;

    char modified_paths[VERIFY_MAX_ISSUES][LPM_PATH_MAX];
    int  n_modified_paths;

    char perm_msgs[VERIFY_MAX_ISSUES][LPM_PATH_MAX + 64];
    int  n_perm_msgs;

    char owner_msgs[VERIFY_MAX_ISSUES][LPM_PATH_MAX + 64];
    int  n_owner_msgs;
} VerifyResult;

/* ── verify_package — runs levels 1-3 against files.meta ──────────────── */
static int verify_package(const char *pkgname, VerifyResult *r) {
    memset(r, 0, sizeof(*r));

    char metapath[LPM_PATH_MAX + 64];
    snprintf(metapath, sizeof(metapath), "%s/%s/files.meta", LPM_FILES_DIR, pkgname);

    FILE *fp = fopen(metapath, "r");
    if (!fp) {
        /* legacy package — fall back to files.list, existence only */
        char listpath[LPM_PATH_MAX + 64];
        snprintf(listpath, sizeof(listpath), "%s/%s/files.list", LPM_FILES_DIR, pkgname);
        FILE *flist = fopen(listpath, "r");
        if (!flist) return -1;  /* no record at all */

        r->no_meta = 1;
        char line[LPM_PATH_MAX];
        while (fgets(line, sizeof(line), flist)) {
            line[strcspn(line, "\n")] = '\0';
            if (!line[0]) continue;
            r->total++;
            struct stat st;
            if (lstat(line, &st) != 0) {
                r->missing++;
                if (r->n_missing_paths < VERIFY_MAX_ISSUES)
                    strncpy(r->missing_paths[r->n_missing_paths++], line, LPM_PATH_MAX - 1);
            }
        }
        fclose(flist);
        return 0;
    }

    char line[LPM_PATH_MAX * 2];
    while (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\n")] = '\0';
        if (!line[0]) continue;

        /* path \t type \t mode \t uid \t gid \t sha256 */
        char path[LPM_PATH_MAX] = "";
        char type = 'f';
        unsigned mode = 0;
        int uid = 0, gid = 0;
        char sha[65] = "-";

        char *p = line;
        char *tab1 = strchr(p, '\t'); if (!tab1) continue; *tab1 = '\0';
        strncpy(path, p, sizeof(path) - 1);
        p = tab1 + 1;

        char *tab2 = strchr(p, '\t'); if (!tab2) continue; *tab2 = '\0';
        type = p[0];
        p = tab2 + 1;

        char *tab3 = strchr(p, '\t'); if (!tab3) continue; *tab3 = '\0';
        mode = (unsigned)strtoul(p, NULL, 8);
        p = tab3 + 1;

        char *tab4 = strchr(p, '\t'); if (!tab4) continue; *tab4 = '\0';
        uid = atoi(p);
        p = tab4 + 1;

        char *tab5 = strchr(p, '\t'); if (!tab5) continue; *tab5 = '\0';
        gid = atoi(p);
        p = tab5 + 1;

        strncpy(sha, p, sizeof(sha) - 1);

        r->total++;

        struct stat st;
        if (lstat(path, &st) != 0) {
            r->missing++;
            if (r->n_missing_paths < VERIFY_MAX_ISSUES)
                strncpy(r->missing_paths[r->n_missing_paths++], path, LPM_PATH_MAX - 1);
            continue;
        }

        /* ── level 2: checksum (regular files only) ───────────────────── */
        if (type == 'f' && strcmp(sha, "-") != 0) {
            char actual[65];
            if (sha256_file(path, actual) == 0 && strcmp(actual, sha) != 0) {
                r->modified++;
                if (r->n_modified_paths < VERIFY_MAX_ISSUES)
                    strncpy(r->modified_paths[r->n_modified_paths++], path, LPM_PATH_MAX - 1);
            }
        }

        /* ── level 3: permission + ownership (skip for symlinks) ──────── */
        if (type != 'l') {
            unsigned actual_mode = st.st_mode & 07777;
            if (actual_mode != mode) {
                r->perm_mismatch++;
                if (r->n_perm_msgs < VERIFY_MAX_ISSUES)
                    snprintf(r->perm_msgs[r->n_perm_msgs++], sizeof(r->perm_msgs[0]),
                             "%s: expected %04o, got %04o", path, mode, actual_mode);
            }
            if ((int)st.st_uid != uid || (int)st.st_gid != gid) {
                r->owner_mismatch++;
                if (r->n_owner_msgs < VERIFY_MAX_ISSUES)
                    snprintf(r->owner_msgs[r->n_owner_msgs++], sizeof(r->owner_msgs[0]),
                             "%s: expected %d:%d, got %d:%d",
                             path, uid, gid, (int)st.st_uid, (int)st.st_gid);
            }
        }
    }
    fclose(fp);
    return 0;
}

/* ── print full report for a single package ───────────────────────────── */
static const char *print_package_report(const char *pkgname) {
    VerifyResult r;
    if (verify_package(pkgname, &r) != 0) {
        printf(C_YELLOW "Verifying %s..." C_RESET "\n\n", pkgname);
        printf("  " C_YELLOW "[SKIP]" C_RESET " no file record found\n\n");
        printf("Result: " C_GRAY "UNKNOWN" C_RESET "\n");
        return "UNKNOWN";
    }

    printf("Verifying %s...\n\n", pkgname);

    /* ── existence ─────────────────────────────────────────────────── */
    if (r.missing == 0) {
        printf("[" C_GREEN "OK" C_RESET "]   Files present\n");
    } else {
        printf("[" C_RED "FAIL" C_RESET "] Missing files:\n");
        for (int i = 0; i < r.n_missing_paths; i++)
            printf("       %s\n", r.missing_paths[i]);
        if (r.missing > r.n_missing_paths)
            printf("       ... and %d more\n", r.missing - r.n_missing_paths);
    }

    /* ── checksum ───────────────────────────────────────────────────── */
    if (r.no_meta) {
        printf("[" C_GRAY "SKIP" C_RESET "] Checksums "
               C_GRAY "(no metadata — installed with older lpm)" C_RESET "\n");
    } else if (r.modified == 0) {
        printf("[" C_GREEN "OK" C_RESET "]   Checksums match\n");
    } else {
        printf("[" C_RED "FAIL" C_RESET "] Checksum mismatch:\n");
        for (int i = 0; i < r.n_modified_paths; i++)
            printf("       %s\n", r.modified_paths[i]);
        if (r.modified > r.n_modified_paths)
            printf("       ... and %d more\n", r.modified - r.n_modified_paths);
    }

    /* ── permissions ───────────────────────────────────────────────── */
    if (r.no_meta) {
        printf("[" C_GRAY "SKIP" C_RESET "] Permissions "
               C_GRAY "(no metadata — installed with older lpm)" C_RESET "\n");
    } else if (r.perm_mismatch == 0) {
        printf("[" C_GREEN "OK" C_RESET "]   Permissions match\n");
    } else {
        printf("[" C_RED "FAIL" C_RESET "] Permission mismatch:\n");
        for (int i = 0; i < r.n_perm_msgs; i++)
            printf("       %s\n", r.perm_msgs[i]);
    }

    /* ── ownership ─────────────────────────────────────────────────── */
    if (r.no_meta) {
        printf("[" C_GRAY "SKIP" C_RESET "] Ownership "
               C_GRAY "(no metadata — installed with older lpm)" C_RESET "\n");
    } else if (r.owner_mismatch == 0) {
        printf("[" C_GREEN "OK" C_RESET "]   Ownership matches\n");
    } else {
        printf("[" C_RED "FAIL" C_RESET "] Ownership mismatch:\n");
        for (int i = 0; i < r.n_owner_msgs; i++)
            printf("       %s\n", r.owner_msgs[i]);
    }

    printf("\n");

    /* ── verdict ───────────────────────────────────────────────────── */
    const char *verdict;
    const char *color;
    if (r.missing > 0)              { verdict = "BROKEN";   color = C_RED;    }
    else if (r.modified > 0)        { verdict = "MODIFIED"; color = C_RED;    }
    else if (r.perm_mismatch > 0 ||
             r.owner_mismatch > 0)  { verdict = "INSECURE"; color = C_YELLOW; }
    else if (r.no_meta)             { verdict = "PRESENT";  color = C_GRAY;   }
    else                             { verdict = "VERIFIED"; color = C_GREEN;  }

    printf("Result: %s%s%s\n", color, verdict, C_RESET);
    return verdict;
}

/* ── Level 4: dependency consistency ───────────────────────────────────── */
static void verify_deps(void) {
    InstalledPkg *all = NULL; int nall = 0;
    if (db_list_all(&all, &nall) != 0 || !all) {
        printf("No installed packages.\n");
        return;
    }

    printf(C_CYAN "::" C_RESET " Checking dependency consistency for %d package(s)...\n\n",
           nall);

    int broken = 0;
    for (int i = 0; i < nall; i++) {
        char pbf[LPM_PATH_MAX + LPM_NAME_MAX + 16];
        snprintf(pbf, sizeof(pbf), "%s/pkgbuild_%s", LPM_PKGBUILD_DIR, all[i].name);

        Pkg pkg; memset(&pkg, 0, sizeof(pkg));
        if (pkgbuild_parse(pbf, &pkg) != 0)
            continue;  /* no PKGBUILD cached — can't check, skip silently */

        for (int d = 0; d < pkg.ndepends; d++) {
            /* strip version constraint */
            char depname[LPM_NAME_MAX] = {0};
            int ni = 0;
            const char *p = pkg.depends[d];
            while (*p && *p != '<' && *p != '>' && *p != '='
                   && ni < (int)sizeof(depname) - 1)
                depname[ni++] = *p++;
            depname[ni] = '\0';
            if (!depname[0]) continue;

            InstalledPkg dep_check;
            if (db_query(depname, &dep_check) != 0) {
                printf("  " C_RED "[FAIL]" C_RESET " %s depends on %s\n",
                       all[i].name, depname);
                printf("         %s " C_RED "missing" C_RESET "\n", depname);
                broken++;
            }
        }
    }
    free(all);

    printf("\n");
    if (broken == 0)
        printf(C_GREEN "All dependencies satisfied." C_RESET "\n");
    else
        printf(C_RED "%d broken dependency reference(s) found." C_RESET "\n", broken);
}

/* ── cmd_verify ─────────────────────────────────────────────────────────── */
void cmd_verify(int argc, char **argv) {
    init_dirs();

    /* --deps: dependency consistency check, ignores other args */
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--deps")) {
            verify_deps();
            return;
        }
    }

    LpmFlags flags;
    char *pkgs[256];
    int npkgs = lpm_parse_flags(argc, argv, &flags, pkgs, 256);

    if (npkgs == 0) {
        /* verify all installed packages */
        InstalledPkg *all = NULL; int nall = 0;
        if (db_list_all(&all, &nall) != 0 || !all || nall == 0) {
            printf("No installed packages.\n");
            return;
        }

        printf(C_CYAN "::" C_RESET " Verifying %d installed package(s)...\n\n", nall);

        int n_verified = 0, n_modified = 0, n_broken = 0, n_insecure = 0, n_unknown = 0;
        for (int i = 0; i < nall; i++) {
            VerifyResult r;
            const char *verdict;
            const char *color;

            if (verify_package(all[i].name, &r) != 0) {
                verdict = "UNKNOWN"; color = C_GRAY; n_unknown++;
            } else if (r.missing > 0) {
                verdict = "BROKEN"; color = C_RED; n_broken++;
            } else if (r.modified > 0) {
                verdict = "MODIFIED"; color = C_RED; n_modified++;
            } else if (r.perm_mismatch > 0 || r.owner_mismatch > 0) {
                verdict = "INSECURE"; color = C_YELLOW; n_insecure++;
            } else if (r.no_meta) {
                verdict = "PRESENT"; color = C_GRAY; n_verified++;
            } else {
                verdict = "VERIFIED"; color = C_GREEN; n_verified++;
            }

            printf("  %-24s %s%s%s\n", all[i].name, color, verdict, C_RESET);
        }
        free(all);

        printf("\n── Summary " C_GRAY "────────────────────────────────" C_RESET "\n");
        printf("  verified : " C_GREEN "%d" C_RESET "\n", n_verified);
        if (n_modified) printf("  modified : " C_RED "%d" C_RESET "\n", n_modified);
        if (n_broken)   printf("  broken   : " C_RED "%d" C_RESET "\n", n_broken);
        if (n_insecure) printf("  insecure : " C_YELLOW "%d" C_RESET "\n", n_insecure);
        if (n_unknown)  printf("  unknown  : " C_GRAY "%d" C_RESET "\n", n_unknown);

        if (n_modified || n_broken)
            exit(1);
        return;
    }

    /* verify specific package(s) */
    int any_fail = 0;
    for (int i = 0; i < npkgs; i++) {
        const char *v = print_package_report(pkgs[i]);
        if (i < npkgs - 1) printf("\n");
        if (!strcmp(v, "BROKEN") || !strcmp(v, "MODIFIED"))
            any_fail = 1;
    }
    if (any_fail) exit(1);
}
