#include "lpm.h"

/* ── search helpers ──────────────────────────────────────────────────────── */

/* Lowercase ASCII in-place */
static void to_lower(char *s) {
    for (; *s; s++)
        if (*s >= 'A' && *s <= 'Z') *s += 32;
}

/* Try to match and print one PKGBUILD file.
 * Returns 1 if a match was found, 0 otherwise. */
static int search_one(const char *pbfile, const char *query_lower) {
    Pkg pkg;
    if (pkgbuild_parse(pbfile, &pkg) != 0) return 0;
    if (!pkg.pkgname[0]) return 0;

    char name_lower[LPM_NAME_MAX];
    snprintf(name_lower, sizeof(name_lower), "%s", pkg.pkgname);
    to_lower(name_lower);

    if (!strstr(name_lower, query_lower)) return 0;

    const char *inst = db_is_installed(pkg.pkgname)
        ? " " C_GREEN "[installed]" C_RESET : "";
    printf("  " C_BOLD "%-26s" C_RESET "  " C_CYAN "%s" C_RESET "-%s%s\n",
           pkg.pkgname,
           pkg.pkgver[0] ? pkg.pkgver : "?",
           pkg.pkgrel[0] ? pkg.pkgrel : "?",
           inst);
    return 1;
}

/* ── cmd_search ──────────────────────────────────────────────────────────── *
 *
 * Fast-path: repo uses directory layout  base/<letter>/<name>/PKGBUILD
 *   → try LPM_PKGBUILD_DIR/<letter>/<query>/PKGBUILD first (exact O(1))
 *   → then scan LPM_PKGBUILD_DIR/<letter>/  (prefix bucket, ~1/26 of repo)
 *   → fallback: full flat scan of LPM_PKGBUILD_DIR  (legacy flat layout)
 *
 * Old flat layout still works: pkgbuild_<name> files in LPM_PKGBUILD_DIR.
 */
void cmd_search(int argc, char **argv) {
    init_dirs();
    if (argc == 0) die("No search term.\nUsage: lpm search <term>");

    const char *query = argv[0];
    int found = 0;

    /* build lowercase query */
    char query_lower[LPM_NAME_MAX];
    snprintf(query_lower, sizeof(query_lower), "%s", query);
    to_lower(query_lower);

    /* first char of query → letter bucket */
    char bucket_letter = query_lower[0];

    /* ── fast path: directory-layout repo ─────────────────────────────── *
     * Try  LPM_PKGBUILD_DIR/<letter>/  first.
     * Each entry is a package dir containing PKGBUILD.                   */
    {
        char bucket_dir[LPM_PATH_MAX];
        snprintf(bucket_dir, sizeof(bucket_dir),
                 "%s/%c", LPM_PKGBUILD_DIR, bucket_letter);

        DIR *bd = opendir(bucket_dir);
        if (bd) {
            struct dirent *ent;
            while ((ent = readdir(bd))) {
                if (ent->d_name[0] == '.') continue;

                /* package name = directory name */
                char name_lower_tmp[LPM_NAME_MAX];
                strncpy(name_lower_tmp, ent->d_name, sizeof(name_lower_tmp) - 1);
                name_lower_tmp[sizeof(name_lower_tmp) - 1] = '\0';
                to_lower(name_lower_tmp);

                if (!strstr(name_lower_tmp, query_lower)) continue;

                /* try new layout: <bucket>/<name>/PKGBUILD */
                char pbfile[LPM_PATH_MAX];
                snprintf(pbfile, sizeof(pbfile),
                         "%s/%c/%s/PKGBUILD",
                         LPM_PKGBUILD_DIR, bucket_letter, ent->d_name);

                struct stat st;
                if (stat(pbfile, &st) == 0) {
                    found += search_one(pbfile, query_lower);
                    continue;
                }
            }
            closedir(bd);

            /* If bucket scan yielded results, we're done */
            if (found) goto search_done;
        }
    }

    /* ── fallback: flat layout  pkgbuild_<name> ──────────────────────── *
     * Still needed for repos that haven't migrated yet.                  */
    {
        DIR *d = opendir(LPM_PKGBUILD_DIR);
        if (!d) die("Cannot open PKGBUILD dir: %s", LPM_PKGBUILD_DIR);

        struct dirent *ent;
        while ((ent = readdir(d))) {
            if (strncmp(ent->d_name, "pkgbuild_", 9) != 0) continue;

            /* quick pre-filter on filename before parsing */
            char fname_lower[LPM_NAME_MAX];
            snprintf(fname_lower, sizeof(fname_lower),
                     "%s", ent->d_name + 9); /* skip "pkgbuild_" prefix */
            to_lower(fname_lower);
            if (!strstr(fname_lower, query_lower)) continue;

            char pbfile[LPM_PATH_MAX];
            snprintf(pbfile, sizeof(pbfile),
                     "%s/%s", LPM_PKGBUILD_DIR, ent->d_name);
            found += search_one(pbfile, query_lower);
        }
        closedir(d);
    }

search_done:

    if (!found)
        printf("No packages found matching " C_YELLOW "%s" C_RESET "\n", query);
}

/* ── cmd_info ────────────────────────────────────────────────────────────── */
void cmd_info(int argc, char **argv) {
    init_dirs();
    if (argc == 0) die("No package specified.\nUsage: lpm -qi <package>");

    for (int i = 0; i < argc; i++) {
        char pbfile[MAX_STR];
        snprintf(pbfile, sizeof(pbfile), "%s/pkgbuild_%s", LPM_PKGBUILD_DIR, argv[i]);

        Pkg pkg;
        if (pkgbuild_parse(pbfile, &pkg) != 0) {
            fprintf(stderr, C_RED "error: " C_RESET "No PKGBUILD found for '%s'\n", argv[i]);
            continue;
        }

        /* build dep strings */
        char deps[2048]     = "(none)";
        char recs[2048]     = "(none)";
        char makedeps[2048] = "(none)";

        if (pkg.ndepends > 0) {
            deps[0] = '\0';
            for (int d = 0; d < pkg.ndepends; d++) {
                if (d) strncat(deps, " ", sizeof(deps) - strlen(deps) - 1);
                strncat(deps, pkg.depends[d], sizeof(deps) - strlen(deps) - 1);
            }
        }
        if (pkg.nrecommends > 0) {
            recs[0] = '\0';
            for (int d = 0; d < pkg.nrecommends; d++) {
                if (d) strncat(recs, " ", sizeof(recs) - strlen(recs) - 1);
                strncat(recs, pkg.recommends[d], sizeof(recs) - strlen(recs) - 1);
            }
        }
        if (pkg.nmakedepends > 0) {
            makedeps[0] = '\0';
            for (int d = 0; d < pkg.nmakedepends; d++) {
                if (d) strncat(makedeps, " ", sizeof(makedeps) - strlen(makedeps) - 1);
                strncat(makedeps, pkg.makedepends[d], sizeof(makedeps) - strlen(makedeps) - 1);
            }
        }

        char *rdeps_str = reverse_deps(argv[i]);
        const char *inst_str = db_is_installed(argv[i])
            ? C_GREEN "Yes" C_RESET : C_YELLOW "No" C_RESET;

        /* ── load build metadata if available ── */
        BuildMeta bm;
        int has_bm = (buildmeta_load(argv[i], &bm) == 0);

        printf(C_BOLD "  %s %s-%s" C_RESET "\n",
               pkg.pkgname, pkg.pkgver, pkg.pkgrel);
        printf("  %s\n\n", pkg.pkgname[0] ? "" : "");
        printf("  " C_BOLD "%-16s" C_RESET " %s\n", "Installed", inst_str);
        printf("  " C_BOLD "%-16s" C_RESET " %s\n", "Depends",     deps);
        printf("  " C_BOLD "%-16s" C_RESET " %s\n", "Recommends",  recs);
        printf("  " C_BOLD "%-16s" C_RESET " %s\n", "MakeDepends", makedeps);
        printf("  " C_BOLD "%-16s" C_RESET " %s\n", "Required by",
               rdeps_str[0] ? rdeps_str : "(none)");
        printf("  " C_BOLD "%-16s" C_RESET " %s\n", "PKGBUILD", pbfile);

        if (has_bm) {
            printf("\n");
            printf("  " C_BOLD "Build info\n" C_RESET);
            printf("  " C_BOLD "%-16s" C_RESET " %s\n",
                   "Built on",    bm.built_on[0]    ? bm.built_on    : "unknown");
            printf("  " C_BOLD "%-16s" C_RESET " %s\n",
                   "Compiler",    bm.compiler[0]    ? bm.compiler    : "unknown");
            printf("  " C_BOLD "%-16s" C_RESET " %s\n",
                   "libc",        bm.libc[0]        ? bm.libc        : "musl");
            printf("  " C_BOLD "%-16s" C_RESET " %s\n",
                   "Build flags", bm.build_flags[0] ? bm.build_flags : "(none)");
            printf("  " C_BOLD "%-16s" C_RESET " %s\n",
                   "Build date",  bm.build_date[0]  ? bm.build_date  : "unknown");
            printf("  " C_BOLD "%-16s" C_RESET " " C_GRAY "%s" C_RESET "\n",
                   "Build hash",  bm.build_hash[0]  ? bm.build_hash  : "(not available)");
        }
        printf("\n");
    }
}

/* compare for qsort */
static int cmp_str(const void *a, const void *b) {
    return strcmp(*(const char **)a, *(const char **)b);
}

/* ── cmd_list ────────────────────────────────────────────────────────────── */
void cmd_list(int argc, char **argv) {
    /* check for --count flag */
    int count_only = 0;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--count") == 0) { count_only = 1; break; }
    }

    FILE *f = fopen(LPM_DB, "r");
    if (!f) {
        if (count_only) printf("0\n");
        else            printf("No packages installed via lpm.\n");
        return;
    }

    char lines[512][MAX_STR];
    int n = 0;
    char line[MAX_STR];
    while (n < 512 && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = '\0';
        if (!line[0]) continue;
        snprintf(lines[n++], MAX_STR, "%s", line);
    }
    fclose(f);

    /* --count: print bare number and exit */
    if (count_only) {
        printf("%d\n", n);
        return;
    }

    if (n == 0) { printf("No packages installed via lpm.\n"); return; }

    /* sort A-Z by pkgname */
    char *ptrs[512];
    for (int i = 0; i < n; i++) ptrs[i] = lines[i];
    qsort(ptrs, n, sizeof(char *), cmp_str);

    printf("\n  " C_BOLD "%-26s  %s" C_RESET "\n", "packages", "version");
    printf("  ───────────────────────────────────────\n");
    for (int i = 0; i < n; i++) {
        char name[MAX_STR], ver[MAX_STR];
        char *eq = strchr(ptrs[i], '=');
        if (eq) {
            strncpy(name, ptrs[i], eq - ptrs[i]);
            name[eq - ptrs[i]] = '\0';
            strncpy(ver, eq + 1, MAX_STR - 1);
        } else {
            strncpy(name, ptrs[i], MAX_STR - 1);
            strncpy(ver,  "-",     MAX_STR - 1);
        }
        printf("  " C_BOLD "%-26s" C_RESET "  " C_CYAN "%s" C_RESET "\n", name, ver);
    }
    printf("  ───────────────────────────────────────\n");
    printf("  Total: " C_CYAN "%d" C_RESET " package(s)\n\n", n);
}
/* ═══════════════════════════════════════════════════════════════════════
 * #22  ORPHAN DETECTION  (lpm -Qo)
 *
 * An orphan is a package installed as a dependency (reason=DEP) but
 * no currently-installed package lists it as a dependency anymore.
 * ═══════════════════════════════════════════════════════════════════════ */
void cmd_orphans(int argc, char **argv) {
    (void)argc; (void)argv;
    init_dirs();

    /* load all installed packages */
    InstalledPkg *all = NULL;
    int n = 0;
    if (db_list_all(&all, &n) != 0 || n == 0) {
        printf("No packages installed.\n");
        free(all);
        return;
    }

    /* build a set of all packages that are needed as deps */
    /* needed[i] = 1 if all[i] is required by someone */
    int *needed = calloc(n, sizeof(int));
    if (!needed) { free(all); return; }

    for (int i = 0; i < n; i++) {
        /* read this package's PKGBUILD to get its deps */
        char pbfile[LPM_PATH_MAX];
        snprintf(pbfile, sizeof(pbfile), "%s/pkgbuild_%s",
                 LPM_PKGBUILD_DIR, all[i].name);
        Pkg pkg;
        if (pkgbuild_parse(pbfile, &pkg) != 0) continue;

        for (int d = 0; d < pkg.ndepends; d++) {
            /* mark the dep as needed */
            for (int j = 0; j < n; j++) {
                if (!strcmp(all[j].name, pkg.depends[d])) {
                    needed[j] = 1;
                    break;
                }
            }
        }
        for (int d = 0; d < pkg.nmakedepends; d++) {
            for (int j = 0; j < n; j++) {
                if (!strcmp(all[j].name, pkg.makedepends[d])) {
                    needed[j] = 1;
                    break;
                }
            }
        }
    }

    /* orphans = installed as DEP + not needed by anyone */
    int norphans = 0;
    printf("\n");
    for (int i = 0; i < n; i++) {
        /* only flag packages installed as dependency, not explicit */
        if (all[i].reason != REASON_DEP) continue;
        if (needed[i]) continue;

        if (norphans == 0)
            printf("  " C_BOLD "%-26s  %s" C_RESET "\n"
                   "  ───────────────────────────────────────\n",
                   "orphan packages", "version");

        printf("  " C_BOLD "%-26s" C_RESET "  " C_YELLOW "%s-%s" C_RESET "\n",
               all[i].name, all[i].version, all[i].release);
        norphans++;
    }

    if (norphans == 0) {
        printf(C_GREEN "  No orphaned packages found." C_RESET "\n\n");
    } else {
        printf("  ───────────────────────────────────────\n");
        printf("  " C_YELLOW "%d" C_RESET " orphan(s) found."
               " Remove with: " C_BOLD "lpm -r <pkg>" C_RESET "\n\n",
               norphans);
    }

    free(needed);
    free(all);
}

/* ── cmd_owns ────────────────────────────────────────────────────────────── */
void cmd_owns(int argc, char **argv) {
    if (argc == 0) die("No path specified.\nUsage: lpm owns <path>");
    for (int i = 0; i < argc; i++) {
        /* normalise: resolve symlinks / relative paths */
        char abs[LPM_PATH_MAX];
        if (!realpath(argv[i], abs))
            snprintf(abs, sizeof(abs), "%s", argv[i]);

        char owner[LPM_NAME_MAX] = "";
        if (db_query_owner(abs, owner, sizeof(owner)) == 0) {
            /* also get version for pretty output */
            char *ver = db_get_version(owner);
            if (ver) {
                printf("%s-%s\n", owner, ver);
                free(ver);
            } else {
                printf("%s\n", owner);
            }
        } else {
            fprintf(stderr, C_RED "error: " C_RESET
                    "no package owns '%s'\n", abs);
        }
    }
}

/* ── cmd_files ───────────────────────────────────────────────────────────── */
void cmd_files(int argc, char **argv) {
    if (argc == 0) die("No package specified.\nUsage: lpm files <package>");
    for (int i = 0; i < argc; i++) {
        if (!db_is_installed(argv[i])) {
            fprintf(stderr, C_YELLOW "warning: " C_RESET
                    "'%s' is not installed\n", argv[i]);
            continue;
        }
        if (argc > 1)
            printf(C_BOLD "%s:\n" C_RESET, argv[i]);
        db_list_files(argv[i]);
    }
}
