
/* ── cmd_group_expand ────────────────────────────────────────────────── *
 * Expand a group name to all member package names, by scanning local    *
 * PKGBUILDs via pkgbuild_parse_fast() (the one LPDF parser) and matching *
 * against Package.groups[].                                              *
 *                                                                          *
 * This used to also try a repo.db "groups=" scan first as a fast path —  *
 * deleted: repo.db (see parse_repo_db(), sync.c / gen-repo-db.sh) has     *
 * never carried a groups field, so that path always fell through to      *
 * this one anyway. Group membership only exists in the LPDF file.        *
 *                                                                          *
 * Returns number of packages found; fills out[] with package names.      *
 * Called by cmd_sync before building queue when target looks like group. */
#include "lpm.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"

int cmd_group_expand(const char *group_name,
                     char out[][LPM_NAME_MAX], int max_out) {
    int found = 0;

    DIR *d = opendir(LPM_PKGBUILD_DIR);
    if (!d) return 0;
    struct dirent *ent;
    while ((ent = readdir(d)) && found < max_out) {
        if (strncmp(ent->d_name, "pkgbuild_", 9) != 0) continue;
        char pbfile[LPM_PATH_MAX];
        snprintf(pbfile, sizeof(pbfile), "%s/%s",
                 LPM_PKGBUILD_DIR, ent->d_name);
        Package fm; memset(&fm, 0, sizeof(fm));
        if (pkgbuild_parse_fast(pbfile, &fm) != 0) continue;
        for (int gi = 0; gi < fm.ngroups; gi++) {
            if (!strcmp(fm.groups[gi], group_name)) {
                if (fm.name[0])
                    { size_t _sl = strlen(fm.name);
                      if (_sl >= LPM_NAME_MAX) _sl = LPM_NAME_MAX-1;
                      memcpy(out[found], fm.name, _sl);
                      out[found++][_sl] = (char)0; }
                break;
            }
        }
    }
    closedir(d);
    return found;
}


/* ── search helpers ──────────────────────────────────────────────────────── */

/* Lowercase ASCII in-place */
static void to_lower(char *s) {
    for (; *s; s++)
        if (*s >= 'A' && *s <= 'Z') *s += 32;
}

/* Try to match and print one PKGBUILD file.
 * Returns 1 if a match was found, 0 otherwise. */
static int search_one(const char *pbfile, const char *query_lower) {
    Package pkg;
    if (pkgbuild_parse_fast(pbfile, &pkg) != 0) return 0;
    if (!pkg.name[0]) return 0;

    char name_lower[LPM_NAME_MAX];
    snprintf(name_lower, sizeof(name_lower), "%s", pkg.name);
    to_lower(name_lower);

    if (!strstr(name_lower, query_lower)) return 0;

    printf("%s %s\n",
           pkg.name,
           pkg.version[0] ? pkg.version : "?");
    if (pkg.description[0])
        printf("\n%s\n", pkg.description);
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

    char query_lower[LPM_NAME_MAX];
    snprintf(query_lower, sizeof(query_lower), "%s", query);
    to_lower(query_lower);

    /* ══════════════════════════════════════════════════════════════════
     * PHASE 1: query persisted repo.db files (fast, no network needed)
     *   /var/lib/lpm/db/base.db  extra.db  lotus.db
     *
     * parse_repo_db() (sync.c) is the one repo.db parser — search.c used
     * to hand-roll its own copy of the same "pkgname=VER-REL pkgtype=...
     * desc=..." tokenizer inline here. Same for build.c's
     * pkg_locate_from_db()/repo_db_get_size().
     *
     * Output mimics pacman -Ss:
     *   lotus/neofetch 7.1.0-1 [bin]
     *       A CLI system information tool
     * ══════════════════════════════════════════════════════════════════ */
    static const char *DB_REPOS[] = { "base", "extra", "lotus", NULL };

    for (int ri = 0; DB_REPOS[ri]; ri++) {
        char dbpath[LPM_PATH_MAX];
        snprintf(dbpath, sizeof(dbpath),
                 "/var/lib/lpm/db/%s.db", DB_REPOS[ri]);

        static RepoEntry entries[4096];
        int n = parse_repo_db(dbpath, DB_REPOS[ri], entries, 4096);

        for (int i = 0; i < n; i++) {
            RepoEntry *e = &entries[i];

            /* case-insensitive match on name OR desc */
            char name_l[LPM_NAME_MAX], desc_l[256];
            snprintf(name_l, sizeof(name_l), "%s", e->name); to_lower(name_l);
            snprintf(desc_l, sizeof(desc_l), "%s", e->desc); to_lower(desc_l);

            if (!strstr(name_l, query_lower) && !strstr(desc_l, query_lower))
                continue;

            /* version field may be VER-REL — show version only */
            char ver[LPM_VER_MAX];
            snprintf(ver, sizeof(ver), "%s", e->version);
            char *dash = strchr(ver, '-');
            if (dash) *dash = '\0';

            printf("%s %s\n", e->name, ver[0] ? ver : e->version);
            if (e->desc[0])
                printf("\n%s\n", e->desc);
            printf("\n");

            found++;
        }
    }

    if (found) goto search_done;

    /* ══════════════════════════════════════════════════════════════════
     * PHASE 2: fallback — scan local PKGBUILD cache
     *   Only runs if repo.db not synced yet (lpm update never run)
     * ══════════════════════════════════════════════════════════════════ */

    /* letter-bucket layout: LPM_PKGBUILD_DIR/<letter>/<name>/ */
    {
        char bucket_dir[LPM_PATH_MAX];
        snprintf(bucket_dir, sizeof(bucket_dir),
                 "%s/%c", LPM_PKGBUILD_DIR, query_lower[0]);
        DIR *bd = opendir(bucket_dir);
        if (bd) {
            struct dirent *ent;
            while ((ent = readdir(bd))) {
                if (ent->d_name[0] == '.') continue;
                char name_l[256];
                snprintf(name_l, sizeof(name_l), "%s", ent->d_name);
                to_lower(name_l);
                if (!strstr(name_l, query_lower)) continue;
                char pbfile[LPM_PATH_MAX];
                snprintf(pbfile, sizeof(pbfile),
                         "%s/%c/%s/PKGBUILD",
                         LPM_PKGBUILD_DIR, query_lower[0], ent->d_name);
                struct stat st;
                if (stat(pbfile, &st) == 0)
                    found += search_one(pbfile, query_lower);
            }
            closedir(bd);
        }
        if (found) goto search_done;
    }

    /* flat layout fallback: pkgbuild_<name> */
    {
        DIR *d = opendir(LPM_PKGBUILD_DIR);
        if (d) {
            struct dirent *ent;
            while ((ent = readdir(d))) {
                if (strncmp(ent->d_name, "pkgbuild_", 9) != 0) continue;
                char name_l[256];
                snprintf(name_l, sizeof(name_l), "%s", ent->d_name + 9);
                to_lower(name_l);
                if (!strstr(name_l, query_lower)) continue;
                char pbfile[LPM_PATH_MAX];
                snprintf(pbfile, sizeof(pbfile),
                         "%s/%s", LPM_PKGBUILD_DIR, ent->d_name);
                found += search_one(pbfile, query_lower);
            }
            closedir(d);
        }
    }

search_done:
    if (!found)
        printf("No packages found matching %s\n", query);
}

/* ── cmd_info ────────────────────────────────────────────────────────────── */
void cmd_info(int argc, char **argv) {
    init_dirs();
    if (argc == 0) die("No package specified.\nUsage: lpm -qi <package>");

    for (int i = 0; i < argc; i++) {
        char pbfile[MAX_STR];
        snprintf(pbfile, sizeof(pbfile), "%s/pkgbuild_%s", LPM_PKGBUILD_DIR, argv[i]);

        Package pkg;
        if (pkgbuild_parse_fast(pbfile, &pkg) != 0) {
            fprintf(stderr,
                    "Error:\n\n"
                    "No PKGBUILD found for '%s'\n\n"
                    "Cannot continue.\n", argv[i]);
            continue;
        }

        char deps[2048] = "";
        char recs[2048] = "";

        if (pkg.ndepends > 0) {
            for (int d = 0; d < pkg.ndepends; d++) {
                if (d) strncat(deps, " ", sizeof(deps) - strlen(deps) - 1);
                strncat(deps, pkg.depends[d], sizeof(deps) - strlen(deps) - 1);
            }
        }
        if (pkg.nrecommends > 0) {
            for (int d = 0; d < pkg.nrecommends; d++) {
                if (d) strncat(recs, " ", sizeof(recs) - strlen(recs) - 1);
                strncat(recs, pkg.recommends[d], sizeof(recs) - strlen(recs) - 1);
            }
        }

        char *rdeps_str = reverse_deps(argv[i]);

        long dl = (long)pkg.dl_size;
        long inst = (long)pkg.inst_size;
        const char *repo = "";
        static const char *DB_REPOS[] = { "base", "extra", "lotus", NULL };
        for (int ri = 0; DB_REPOS[ri]; ri++) {
            char dbpath[LPM_PATH_MAX];
            snprintf(dbpath, sizeof(dbpath),
                     "/var/lib/lpm/db/%s.db", DB_REPOS[ri]);
            static RepoEntry entries[4096];
            int n = parse_repo_db(dbpath, DB_REPOS[ri], entries, 4096);
            for (int ei = 0; ei < n; ei++) {
                if (!strcmp(entries[ei].name, pkg.name)) {
                    repo = DB_REPOS[ri];
                    if (entries[ei].dl_size > 0)   dl   = entries[ei].dl_size;
                    if (entries[ei].inst_size > 0) inst = entries[ei].inst_size;
                    break;
                }
            }
            if (repo[0]) break;
        }

        char sdl[32] = "", sinst[32] = "";
        if (dl > 0)   format_size(dl, sdl, sizeof(sdl));
        if (inst > 0) format_size(inst, sinst, sizeof(sinst));

        printf("%-16s%s\n", "Name", pkg.name);
        printf("%-16s%s\n", "Version", pkg.version[0] ? pkg.version : "?");
        if (repo[0])
            printf("%-16s%s\n", "Repository", repo);
        printf("%-16s%s\n", "Architecture", "x86_64");
        if (sinst[0])
            printf("%-16s%s\n", "Installed size", sinst);
        if (sdl[0])
            printf("%-16s%s\n", "Download size", sdl);
        if (pkg.license[0])
            printf("%-16s%s\n", "License", pkg.license);
        if (deps[0])
            printf("%-16s%s\n", "Dependencies", deps);
        if (recs[0])
            printf("%-16s%s\n", "Optional", recs);
        printf("%-16s%s\n", "Required by",
               rdeps_str[0] ? rdeps_str : "");
        if (pkg.description[0])
            printf("%-16s%s\n", "Description", pkg.description);
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
        else            printf("There is nothing to do.\n");
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

    if (n == 0) { printf("There is nothing to do.\n"); return; }

    /* sort A-Z by pkgname */
    char *ptrs[512];
    for (int i = 0; i < n; i++) ptrs[i] = lines[i];
    qsort(ptrs, n, sizeof(char *), cmp_str);

    int name_w = 8;
    for (int i = 0; i < n; i++) {
        char *eq = strchr(ptrs[i], '=');
        int len = eq ? (int)(eq - ptrs[i]) : (int)strlen(ptrs[i]);
        if (len > name_w) name_w = len;
    }

    for (int i = 0; i < n; i++) {
        char name[MAX_STR], ver[MAX_STR];
        char *eq = strchr(ptrs[i], '=');
        if (eq) {
            size_t nl = (size_t)(eq - ptrs[i]);
            if (nl >= sizeof(name)) nl = sizeof(name) - 1;
            memcpy(name, ptrs[i], nl);
            name[nl] = '\0';
            snprintf(ver, sizeof(ver), "%s", eq + 1);
        } else {
            snprintf(name, sizeof(name), "%s", ptrs[i]);
            snprintf(ver, sizeof(ver), "-");
        }
        printf("%-*s  %s\n", name_w, name, ver);
    }
}
/* ═══════════════════════════════════════════════════════════════════════
 * #22  ORPHAN DETECTION  (lpm -Qo)
 *
 * An orphan is a package installed as a dependency (reason=DEP) but
 * no currently-installed package lists it as a dependency anymore.
 * ═══════════════════════════════════════════════════════════════════════ */
/* ── db_count_orphans ─────────────────────────────────────────────────── *
 * Same logic as cmd_orphans but returns just the count, no output.
 * Used by `lpm audit`.                                                    */
int db_count_orphans(void) {
    InstalledPkg *all = NULL;
    int n = 0;
    if (db_list_all(&all, &n) != 0 || n == 0) {
        free(all);
        return 0;
    }

    int *needed = calloc(n, sizeof(int));
    if (!needed) { free(all); return 0; }

    for (int i = 0; i < n; i++) {
        char pbfile[LPM_PATH_MAX];
        snprintf(pbfile, sizeof(pbfile), "%s/pkgbuild_%s",
                 LPM_PKGBUILD_DIR, all[i].name);
        Package pkg;
        if (pkgbuild_parse_fast(pbfile, &pkg) != 0) continue;

        for (int d = 0; d < pkg.ndepends; d++) {
            for (int j = 0; j < n; j++) {
                if (!strcmp(all[j].name, pkg.depends[d])) { needed[j] = 1; break; }
            }
        }
        for (int d = 0; d < pkg.nmakedepends; d++) {
            for (int j = 0; j < n; j++) {
                if (!strcmp(all[j].name, pkg.makedepends[d])) { needed[j] = 1; break; }
            }
        }
    }

    int norphans = 0;
    for (int i = 0; i < n; i++) {
        if (all[i].reason != REASON_DEP) continue;
        if (needed[i]) continue;
        norphans++;
    }

    free(needed);
    free(all);
    return norphans;
}

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
        Package pkg;
        if (pkgbuild_parse_fast(pbfile, &pkg) != 0) continue;

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
    for (int i = 0; i < n; i++) {
        if (all[i].reason != REASON_DEP) continue;
        if (needed[i]) continue;

        if (norphans == 0)
            printf("Unused dependencies:\n\n");

        printf("%s\n", all[i].name);
        norphans++;
    }

    if (norphans == 0)
        printf("There is nothing to do.\n");
    else
        printf("\n%d unused\n", norphans);

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
#pragma GCC diagnostic pop
