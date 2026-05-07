/*
 * sync.c — lpm -Suy: Sync + Update (pacman/XBPS/Portage inspired)
 *
 * Flow:
 *   1. Download base.db / extra.db / lotus.db in parallel (3 pthreads)
 *      Each thread shows a Gentoo-style spinner while fetching.
 *   2. Parse all three DBs → build a map of { pkgname → (repo, newver) }
 *   3. Walk installed DB, find packages where newver > installed
 *   4. Show update table (type, name, old→new, dl size, install size)
 *   5. Disk space check (binary packages only; source: heuristic)
 *   6. Single confirm prompt
 *   7. Fetch PKGBUILDs for packages to update, then rebuild
 *
 * repo.db format (one line per package, hosted at REPO_BASE/<repo>/repo.db):
 *   pkgname=VERSION-REL pkgtype=binary|source dlsize=BYTES instsize=BYTES
 *   e.g.:
 *   firefox=144.0.1-1 pkgtype=binary dlsize=234799513 instsize=569548800
 *   acl=2.3.2-1 pkgtype=source dlsize=524288 instsize=0
 *
 * Fields are space-separated; unknown fields are ignored (forward compat).
 */

#include "lpm.h"
#include <pthread.h>
#include <sys/statvfs.h>
#include <ctype.h>


/* CHECK_CANCEL — same macro as in build.c, needed here too */
#ifndef CHECK_CANCEL
#define CHECK_CANCEL(label)                                        \
  do {                                                             \
    if (g_cancel) {                                               \
      fprintf(stderr, "\n" C_YELLOW "warning:" C_RESET             \
              " Interrupt received — operation aborted.\n");       \
      goto label;                                                  \
    }                                                              \
  } while (0)
#endif

/* ── Constants ───────────────────────────────────────────────────────── */

#define REPO_BASE \
    "https://raw.githubusercontent.com/draconmc1337/lotus-repository/main"

#define NREPOS      3
#define DB_TIMEOUT  15          /* seconds for repo.db fetch            */
#define SPIN_FPS    10          /* spinner updates per second           */
#define DISK_MARGIN 1.10        /* require 10% headroom above need      */
/* source build heuristic: estimated install = download × SRCMUL        */
#define SRC_SIZE_MUL 8

static const char *REPO_NAMES[NREPOS] = { "base", "extra", "lotus" };

/* ── Data structures ─────────────────────────────────────────────────── */

/* One entry from a repo.db */
typedef struct {
    char name[LPM_NAME_MAX];
    char version[LPM_VER_MAX + 16]; /* "ver-rel" */
    char repo[16];                   /* "base" / "extra" / "lotus" */
    int  is_binary;                  /* 1=binary, 0=source */
    long dl_size;                    /* bytes; 0 = unknown */
    long inst_size;                  /* bytes; 0 = source/unknown */
} RepoEntry;

/* One package that needs updating */
typedef struct {
    char name[LPM_NAME_MAX];
    char inst_ver[LPM_VER_MAX + 16];
    char new_ver[LPM_VER_MAX + 16];
    char repo[16];
    int  is_binary;
    int  is_critical;
    long dl_size;
    long inst_size;
} UpdateEntry;

/* Per-thread state for parallel repo.db fetch */
typedef struct {
    int   idx;                    /* 0=base, 1=extra, 2=lotus */
    char  url[LPM_URL_MAX];
    char  dest[LPM_PATH_MAX];
    int   result;                 /* 0=ok, -1=fail */
    /* spinner state */
    volatile int done;
    pthread_mutex_t *print_mu;
} RepoFetchJob;

/* ── Spinner ─────────────────────────────────────────────────────────── */
/* Gentoo-style: "[ \ ]" spinning in place, replaced by bar when done.  */

static const char SPIN_CHARS[] = { '|', '/', '-', '\\' };


/* ── Fetch thread ────────────────────────────────────────────────────── */

static void *repo_fetch_thread(void *arg) {
    RepoFetchJob *j = (RepoFetchJob *)arg;

    /* build wget/curl command — quiet, strict timeout */
    char part[LPM_PATH_MAX];
    snprintf(part, sizeof(part), "%s.part", j->dest);

    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
        "wget -q --timeout=%d --tries=2 -O '%s' '%s' 2>/dev/null"
        " || curl -sL --connect-timeout %d --max-time %d -o '%s' '%s' 2>/dev/null",
        DB_TIMEOUT, part, j->url,
        DB_TIMEOUT, DB_TIMEOUT * 2, part, j->url);

    int rc = system(cmd);

    struct stat st;
    if (rc == 0 && stat(part, &st) == 0 && st.st_size >= 4) {
        rename(part, j->dest);
        j->result = 0;
    } else {
        remove(part);
        j->result = -1;
    }
    j->done = 1;
    return NULL;
}

/* ── repo.db parser ──────────────────────────────────────────────────── *
 * Format: one package per line                                          *
 *   pkgname=VER-REL pkgtype=binary|source dlsize=N instsize=N          */
static int parse_repo_db(const char *path, const char *reponame,
                         RepoEntry *out, int maxn) {
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;

    char line[1024];
    int n = 0;

    while (n < maxn && fgets(line, sizeof(line), fp)) {
        /* skip comments and empty lines */
        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p || *p == '#') continue;

        RepoEntry e;
        memset(&e, 0, sizeof(e));
        strncpy(e.repo, reponame, 15);
        e.is_binary = 0; /* default: source */

        /* tokenise space-separated fields */
        char *tok = strtok(p, " \t\n\r");
        int first = 1;
        while (tok) {
            char *eq = strchr(tok, '=');
            if (!eq) { tok = strtok(NULL, " \t\n\r"); continue; }
            *eq = '\0';
            char *key = tok, *val = eq + 1;

            if (first) {
                /* first field is always pkgname=VER-REL */
                strncpy(e.name,    key, LPM_NAME_MAX - 1);
                strncpy(e.version, val, LPM_VER_MAX + 15);
                first = 0;
            } else if (!strcmp(key, "pkgtype")) {
                e.is_binary = (!strcmp(val, "binary") || !strcmp(val, "bin"));
            } else if (!strcmp(key, "dlsize")) {
                e.dl_size = atol(val);
            } else if (!strcmp(key, "instsize")) {
                e.inst_size = atol(val);
            }
            tok = strtok(NULL, " \t\n\r");
        }

        if (e.name[0] && e.version[0])
            out[n++] = e;
    }
    fclose(fp);
    return n;
}

/* ── Size formatter ──────────────────────────────────────────────────── */
static void fmt_size(long bytes, char *out, size_t sz) {
    if (bytes <= 0)        { snprintf(out, sz, "—"); return; }
    if (bytes < 1024)      { snprintf(out, sz, "%ld B",   bytes);              return; }
    if (bytes < 1<<20)     { snprintf(out, sz, "%.1f KiB", bytes / 1024.0);   return; }
    if (bytes < 1<<30)     { snprintf(out, sz, "%.1f MiB", bytes / 1048576.0);return; }
    snprintf(out, sz, "%.2f GiB", bytes / 1073741824.0);
}

/* ── cmd_suy — public entry point ────────────────────────────────────── */

void cmd_suy(int argc, char **argv) {
    check_root();
    init_dirs();
    check_remove_journal();

    LpmConfig cfg;
    lpm_config_load(LPM_CONF_FILE, &cfg);
    LpmFlags flags;
    char *flagargs[256];
    lpm_parse_flags(argc, argv, &flags, flagargs, 256);

    /* ── Phase 1: fetch repo.db files in parallel ─────────────────── */
    DBG(1, "starting repo sync (%d repos)", NREPOS);
    printf(C_CYAN "::" C_RESET C_BOLD
           " Synchronizing package databases...\n" C_RESET);

    /* temp dir for downloaded DBs */
    char db_dir[LPM_PATH_MAX];
    snprintf(db_dir, sizeof(db_dir), "/tmp/lpm-sync.%d", (int)getpid());
    util_mkdirp(db_dir, 0700);

    RepoFetchJob jobs[NREPOS];
    pthread_t    threads[NREPOS];
    memset(jobs, 0, sizeof(jobs));

    for (int i = 0; i < NREPOS; i++) {
        jobs[i].idx  = i;
        jobs[i].done = 0;
        jobs[i].result = -1;
        snprintf(jobs[i].url,  sizeof(jobs[i].url),
                 "%s/%s/repo.db", REPO_BASE, REPO_NAMES[i]);
        snprintf(jobs[i].dest, sizeof(jobs[i].dest),
                 "%s/%s.db", db_dir, REPO_NAMES[i]);
        pthread_create(&threads[i], NULL, repo_fetch_thread, &jobs[i]);
    }

    /* spinner loop — runs in main thread until all workers done */
    int frame = 0;
    int all_done = 0;
    int printed[NREPOS] = {0};   /* 1 once we print the final "OK" line */

    /* Print 3 placeholder lines */
    for (int i = 0; i < NREPOS; i++)
        printf("  [ ] %-8s  waiting...\n", REPO_NAMES[i]);

    while (!all_done) {
        all_done = 1;
        /* move cursor up NREPOS lines */
        printf("\033[%dA", NREPOS);

        for (int i = 0; i < NREPOS; i++) {
            if (!jobs[i].done) {
                all_done = 0;
                /* spinning */
                printf("  [" C_CYAN "%c" C_RESET
                       "         ]  %%  " C_BOLD "%-8s" C_RESET "  syncing...\n",
                       SPIN_CHARS[frame % 4], REPO_NAMES[i]);
            } else if (!printed[i]) {
                printed[i] = 1;
                if (jobs[i].result == 0)
                    printf("  [" C_GREEN "##########" C_RESET
                           "] 100%%  " C_BOLD "%-8s" C_RESET
                           "  " C_GREEN "OK" C_RESET "          \n",
                           REPO_NAMES[i]);
                else
                    printf("  [" C_RED "!!!!!!!!!!!" C_RESET
                           "]  ERR  " C_BOLD "%-8s" C_RESET
                           "  " C_RED "FAILED" C_RESET "        \n",
                           REPO_NAMES[i]);
            } else {
                /* already printed final line — reprint same */
                if (jobs[i].result == 0)
                    printf("  [" C_GREEN "##########" C_RESET
                           "] 100%%  " C_BOLD "%-8s" C_RESET
                           "  " C_GREEN "OK" C_RESET "          \n",
                           REPO_NAMES[i]);
                else
                    printf("  [" C_RED "!!!!!!!!!!!" C_RESET
                           "]  ERR  " C_BOLD "%-8s" C_RESET
                           "  " C_RED "FAILED" C_RESET "        \n",
                           REPO_NAMES[i]);
            }
        }
        frame++;
        fflush(stdout);
        if (!all_done)
            usleep(1000000 / SPIN_FPS);
    }

    /* join threads */
    for (int i = 0; i < NREPOS; i++)
        pthread_join(threads[i], NULL);

    printf("\n");

    /* ── Phase 2: parse all repo DBs ─────────────────────────────── */
    /* max 4096 entries total across all repos */
    RepoEntry *repo_entries = calloc(4096, sizeof(RepoEntry));
    if (!repo_entries) die("Out of memory");
    int total_entries = 0;

    for (int i = 0; i < NREPOS; i++) {
        if (jobs[i].result != 0) continue;
        int n = parse_repo_db(jobs[i].dest, REPO_NAMES[i],
                              repo_entries + total_entries,
                              4096 - total_entries);
        DBG(1, "[%s] repo.db: %d packages", REPO_NAMES[i], n);
        total_entries += n;
    }

    /* cleanup temp dir */
    char rmcmd[LPM_PATH_MAX + 16];
    snprintf(rmcmd, sizeof(rmcmd), "rm -rf '%s'", db_dir);
    system(rmcmd);

    if (total_entries == 0) {
        fprintf(stderr, C_RED "error:" C_RESET
                " All repo DBs failed to fetch or are empty.\n"
                "  Check your network connection.\n");
        free(repo_entries);
        return;
    }

    /* ── Phase 3: determine targets ──────────────────────────────── */
    /* If args given: only check those packages.
     * Otherwise: check all installed packages.                       */
    char  *targets[1024];
    int    ntargets = 0;
    int    targets_alloc = 0;

    if (argc > 0) {
        for (int i = 0; i < argc && ntargets < 1024; i++)
            targets[ntargets++] = argv[i];
    } else {
        FILE *f = fopen(LPM_DB, "r");
        if (!f) {
            printf(C_CYAN "::" C_RESET " No packages installed.\n");
            free(repo_entries);
            return;
        }
        char line[MAX_STR];
        while (fgets(line, sizeof(line), f) && ntargets < 1024) {
            line[strcspn(line, "\n")] = '\0';
            if (!line[0]) continue;
            char *eq = strchr(line, '=');
            if (eq) *eq = '\0';
            targets[ntargets++] = strdup(line);
        }
        fclose(f);
        targets_alloc = 1; /* remember to free */
    }

    /* ── Phase 4: diff installed vs repo ─────────────────────────── */
    UpdateEntry *updates = calloc(1024, sizeof(UpdateEntry));
    if (!updates) die("Out of memory");
    int nupdate  = 0;
    int nignored = 0;

    for (int t = 0; t < ntargets; t++) {
        const char *name = targets[t];

        if (lpm_config_is_ignored(&cfg, name)) { nignored++; continue; }

        /* look up in repo entries — last writer wins (lotus > extra > base) */
        RepoEntry *found = NULL;
        for (int e = 0; e < total_entries; e++) {
            if (!strcmp(repo_entries[e].name, name))
                found = &repo_entries[e];
        }
        if (!found) continue; /* not in any repo */

        char *inst = db_get_version(name);
        if (!inst) {
            /* installed but no version record — add to update list */
            UpdateEntry *u = &updates[nupdate++];
            strncpy(u->name,     name,            LPM_NAME_MAX-1);
            strncpy(u->inst_ver, "unknown",        LPM_VER_MAX+15);
            strncpy(u->new_ver,  found->version,   LPM_VER_MAX+15);
            strncpy(u->repo,     found->repo,       15);
            u->is_binary   = found->is_binary;
            u->is_critical = lpm_config_is_critical(&cfg, name);
            u->dl_size     = found->dl_size;
            u->inst_size   = found->inst_size;
        } else if (version_compare(inst, found->version) < 0) {
            DBG(2, "update queued: %s  %s -> %s  [%s]",
                name, inst, found->version, found->repo);
            UpdateEntry *u = &updates[nupdate++];
            strncpy(u->name,     name,           LPM_NAME_MAX-1);
            strncpy(u->inst_ver, inst,            LPM_VER_MAX+15);
            strncpy(u->new_ver,  found->version,  LPM_VER_MAX+15);
            strncpy(u->repo,     found->repo,      15);
            u->is_binary   = found->is_binary;
            u->is_critical = lpm_config_is_critical(&cfg, name);
            u->dl_size     = found->dl_size;
            u->inst_size   = found->inst_size;
            free(inst);
        } else {
            free(inst);
        }
    }

    /* ── Phase 5: display ─────────────────────────────────────────── */
    if (nupdate == 0) {
        printf(C_CYAN "::" C_RESET " " C_GREEN
               "All packages are up to date." C_RESET "\n");
        if (nignored)
            printf(C_GRAY "   (%d package(s) in IgnorePkg — skipped)"
                   C_RESET "\n", nignored);
        goto suy_cleanup;
    }

    printf(C_CYAN "::" C_RESET C_BOLD
           " Packages to update (%d):\n\n" C_RESET, nupdate);

    /* column header */
    printf("  %-8s  %-24s  %-16s  %-16s  %-12s  %s\n",
           "Type", "Package", "Installed", "New", "Download", "Install");
    printf("  %-8s  %-24s  %-16s  %-16s  %-12s  %s\n",
           "------", "-------", "---------", "---", "--------", "-------");

    long total_dl   = 0;
    long total_inst = 0;  /* binary installs only */
    long total_src_heuristic = 0;

    for (int i = 0; i < nupdate; i++) {
        UpdateEntry *u = &updates[i];

        char type_str[32];
        if (u->is_binary)
            snprintf(type_str, sizeof(type_str),
                     "[" C_BLUE "bin" C_RESET "]");
        else
            snprintf(type_str, sizeof(type_str),
                     "[" C_YELLOW "src" C_RESET "]");

        char dl_str[32], inst_str[32];
        fmt_size(u->dl_size,   dl_str,   sizeof(dl_str));
        if (u->is_binary)
            fmt_size(u->inst_size, inst_str, sizeof(inst_str));
        else
            snprintf(inst_str, sizeof(inst_str),
                     C_GRAY "(source build)" C_RESET);

        char crit_tag[24] = "";
        if (u->is_critical)
            snprintf(crit_tag, sizeof(crit_tag),
                     " " C_RED "[CRITICAL]" C_RESET);

        printf("  %-18s  %-24s  %-16s  %-16s  %-12s  %s%s\n",
               type_str,
               u->name,
               u->inst_ver,
               u->new_ver,
               dl_str,
               inst_str,
               crit_tag);

        if (u->dl_size > 0)   total_dl   += u->dl_size;
        if (u->is_binary)     total_inst += u->inst_size;
        else                  total_src_heuristic +=
                                  (long)(u->dl_size * SRC_SIZE_MUL);
    }

    /* ── Phase 6: summary + disk check ──────────────────────────── */
    {
        char dl_str[32], inst_str[32], free_str[32];
        long needed = (long)((total_inst + total_src_heuristic) * DISK_MARGIN);

        fmt_size(total_dl,   dl_str,   sizeof(dl_str));
        fmt_size(total_inst + total_src_heuristic, inst_str, sizeof(inst_str));

        long free_bytes = util_disk_free("/");
        fmt_size(free_bytes, free_str, sizeof(free_str));

        printf("\n");
        if (nignored)
            printf(C_GRAY "  (%d package(s) in IgnorePkg — skipped)\n"
                   C_RESET, nignored);
        printf("  Download:          %s\n",  dl_str);
        printf("  Disk after update: +%s",   inst_str);
        if (total_src_heuristic > 0)
            printf(C_GRAY "  (src: heuristic ×%d)" C_RESET, SRC_SIZE_MUL);
        printf("\n");

        DBG(1, "disk check: need %ld bytes, free %ld bytes", needed, free_bytes);
        if (free_bytes > 0) {
            if (free_bytes < needed) {
                char need_str[32];
                fmt_size(needed, need_str, sizeof(need_str));
                printf("  Free space:        " C_RED "%s  ✗ Not enough!"
                       C_RESET "\n", free_str);
                printf("\n" C_RED "error:" C_RESET
                       " No space left on device.\n"
                       "  Need at least " C_BOLD "%s" C_RESET
                       " free, have %s.\n",
                       need_str, free_str);
                goto suy_cleanup;
            } else {
                printf("  Free space:        " C_GREEN "%s  ✓ OK"
                       C_RESET "\n", free_str);
            }
        }
    }

    /* ── Phase 7: confirm ────────────────────────────────────────── */
    printf("\n");
    if (!flags.no_confirm) {
        if (!confirm(C_CYAN "::" C_RESET " Proceed to update? ["
                     C_GREEN "Y" C_RESET "/" C_RED "n" C_RESET "] ")) {
            printf(C_YELLOW "Operation cancelled." C_RESET "\n");
            goto suy_cleanup;
        }
    }
    printf("\n");

    /* ── Phase 8: fetch PKGBUILDs + rebuild ─────────────────────── */
    int failed = 0;
    for (int i = 0; i < nupdate; i++) {
        CHECK_CANCEL(suy_done);
        UpdateEntry *u = &updates[i];

        printf(C_CYAN "(%d/%d)" C_RESET C_BOLD " Updating %s" C_RESET
               "  " C_YELLOW "%s" C_RESET " -> " C_CYAN "%s" C_RESET "\n",
               i + 1, nupdate, u->name, u->inst_ver, u->new_ver);

        /* fetch fresh PKGBUILD */
        char dest[LPM_PATH_MAX], url_found[LPM_URL_MAX];
        int fetched = 0;
        snprintf(dest, sizeof(dest), "%s/pkgbuild_%s",
                 LPM_PKGBUILD_DIR, u->name);

        /* try known repo first, then fall back to all */
        const char *try_repos[NREPOS + 1];
        int ntr = 0;
        try_repos[ntr++] = u->repo; /* known repo first */
        for (int r = 0; r < NREPOS; r++)
            if (strcmp(REPO_NAMES[r], u->repo) != 0)
                try_repos[ntr++] = REPO_NAMES[r];

        for (int r = 0; r < ntr && !fetched; r++) {
            snprintf(url_found, sizeof(url_found),
                     "%s/%s/pkgbuild_%s", REPO_BASE, try_repos[r], u->name);
            char part[LPM_PATH_MAX];
            snprintf(part, sizeof(part), "%s.part", dest);
            char cmd[2048];
            snprintf(cmd, sizeof(cmd),
                "wget -q --timeout=30 --tries=2 -O '%s' '%s' 2>/dev/null"
                " || curl -sL --connect-timeout 30 -o '%s' '%s' 2>/dev/null",
                part, url_found, part, url_found);
            struct stat st;
            if (system(cmd) == 0 &&
                stat(part, &st) == 0 && st.st_size >= 32) {
                rename(part, dest);
                pkgbuild_invalidate_cache(u->name);
                dep_meta_cache_invalidate();
                fetched = 1;
            } else {
                remove(part);
            }
        }

        if (!fetched) {
            fprintf(stderr,
                C_RED "error:" C_RESET
                " Could not fetch PKGBUILD for %s — skipping.\n", u->name);
            failed++;
            continue;
        }

        /* wipe build cache → clean rebuild */
        char cache[LPM_PATH_MAX], rmcache[LPM_PATH_MAX + 16];
        snprintf(cache,   sizeof(cache),   "%s/%s", LPM_BUILD_DIR, u->name);
        snprintf(rmcache, sizeof(rmcache), "rm -rf '%s'", cache);
        system(rmcache);

        /* delegate to cmd_sync which handles build + merge + db */
        char *pair[1] = { u->name };
        cmd_sync(1, pair);

        lpm_log("Updated %s  %s -> %s", u->name, u->inst_ver, u->new_ver);
    }

suy_done:
    printf("\n" C_CYAN "::" C_RESET " ");
    if (failed)
        printf(C_YELLOW "%d package(s) updated, " C_RED "%d failed."
               C_RESET "\n", nupdate - failed, failed);
    else
        printf(C_GREEN "System is up to date." C_RESET "\n");

suy_cleanup:
    free(repo_entries);
    free(updates);
    if (targets_alloc)
        for (int i = 0; i < ntargets; i++) free(targets[i]);
}
