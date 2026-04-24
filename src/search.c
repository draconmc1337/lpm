#include "lpm.h"

/* ── cmd_search ──────────────────────────────────────────────────────────── */
void cmd_search(int argc, char **argv) {
    init_dirs();
    if (argc == 0) die("No search term.\nUsage: lpm -s <term>");

    const char *query = argv[0];
    int found = 0;

    DIR *d = opendir(LPM_PKGBUILD_DIR);
    if (!d) die("Cannot open PKGBUILD dir: %s", LPM_PKGBUILD_DIR);

    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (strncmp(ent->d_name, "pkgbuild_", 9) != 0) continue;

        char pbfile[MAX_STR];
        snprintf(pbfile, sizeof(pbfile), "%s/%s", LPM_PKGBUILD_DIR, ent->d_name);

        Pkg pkg;
        if (pkgbuild_parse(pbfile, &pkg) != 0) continue;
        if (!pkg.pkgname[0]) continue;

        /* case-insensitive match */
        char name_lower[MAX_STR], query_lower[MAX_STR];
        snprintf(name_lower,  MAX_STR, "%s", pkg.pkgname);
        strncpy(query_lower, query,       MAX_STR - 1);
        for (char *c = name_lower;  *c; c++) *c = (*c >= 'A' && *c <= 'Z') ? *c + 32 : *c;
        for (char *c = query_lower; *c; c++) *c = (*c >= 'A' && *c <= 'Z') ? *c + 32 : *c;

        if (!strstr(name_lower, query_lower)) continue;

        const char *inst = db_is_installed(pkg.pkgname)
            ? " " C_GREEN "[installed]" C_RESET : "";
        printf("  " C_BOLD "%-26s" C_RESET "  " C_CYAN "%s" C_RESET "-%s%s\n",
               pkg.pkgname, pkg.pkgver[0] ? pkg.pkgver : "?",
               pkg.pkgrel[0] ? pkg.pkgrel : "?", inst);
        found = 1;
    }
    closedir(d);

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

        printf(C_BOLD "──────────────────────────────────────" C_RESET "\n");
        printf("  " C_BOLD "%-14s" C_RESET " %s\n",        "Name",        pkg.pkgname);
        printf("  " C_BOLD "%-14s" C_RESET " %s-%s\n",     "Version",     pkg.pkgver, pkg.pkgrel);
        printf("  " C_BOLD "%-14s" C_RESET " %s\n",        "Installed",   inst_str);
        printf("  " C_BOLD "%-14s" C_RESET " %s\n",        "Depends",     deps);
        printf("  " C_BOLD "%-14s" C_RESET " %s\n",        "Recommends",  recs);
        printf("  " C_BOLD "%-14s" C_RESET " %s\n",        "MakeDepends", makedeps);
        printf("  " C_BOLD "%-14s" C_RESET " %s\n",        "Required by", rdeps_str[0] ? rdeps_str : "(none)");
        printf("  " C_BOLD "%-14s" C_RESET " %s\n",        "PKGBUILD",    pbfile);
        printf(C_BOLD "──────────────────────────────────────" C_RESET "\n\n");
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
