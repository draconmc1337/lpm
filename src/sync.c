/*
 * sync.c — lpm upgrade: Sync + Update (pacman/XBPS/Portage inspired)
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
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result"
#pragma GCC diagnostic ignored "-Wformat-truncation"
#pragma GCC diagnostic ignored "-Wstringop-truncation"



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
    char desc[256];                  /* short description */
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
    char replaces_old[LPM_NAME_MAX]; /* non-empty = rename from this name */
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
            } else if (!strcmp(key, "desc")) {
                /* decode %20 → space */
                char *d = e.desc; const char *s = val;
                while (*s && d < e.desc + sizeof(e.desc) - 1) {
                    if (s[0]=='%' && s[1]=='2' && s[2]=='0') { *d++=(char)32; s+=3; }
                    else *d++ = *s++;
                }
                *d = (char)0;
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

/* ── do_repo_sync ────────────────────────────────────────────────────── *
 * Fetches all NREPOS repo.db files in parallel, parses them into
 * repo_entries (caller-allocated, maxn capacity), and persists copies to
 * /var/lib/lpm/db/<repo>.db for offline `lpm search`.
 *
 * show_progress=1 (used by `lpm update`):
 *   :: Syncing repositories...
 *
 *     -> base
 *     -> extra
 *     -> lotus
 *
 * show_progress=0 (used internally by `lpm upgrade` — always fetches
 * fresh data for correctness, but stays quiet until the update list is
 * known, per the "predictable, not verbose" output style).
 *
 * Returns the number of repos successfully fetched (0..NREPOS).        */
static int do_repo_sync(int show_progress, RepoEntry *repo_entries, int *total_entries_out) {
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

    for (int i = 0; i < NREPOS; i++)
        pthread_join(threads[i], NULL);

    /* ── fetch .sig files (parallel, same temp dir) ───────────────── */
    RepoFetchJob sig_jobs[NREPOS];
    pthread_t    sig_threads[NREPOS];
    memset(sig_jobs, 0, sizeof(sig_jobs));
    for (int i = 0; i < NREPOS; i++) {
        sig_jobs[i].idx    = i;
        sig_jobs[i].done   = 0;
        sig_jobs[i].result = -1;
        snprintf(sig_jobs[i].url,  sizeof(sig_jobs[i].url),
                 "%s/%s/repo.db.sig", REPO_BASE, REPO_NAMES[i]);
        snprintf(sig_jobs[i].dest, sizeof(sig_jobs[i].dest),
                 "%s/%s.db.sig", db_dir, REPO_NAMES[i]);
        pthread_create(&sig_threads[i], NULL, repo_fetch_thread, &sig_jobs[i]);
    }
    for (int i = 0; i < NREPOS; i++)
        pthread_join(sig_threads[i], NULL);

    int ok_count = 0;
    for (int i = 0; i < NREPOS; i++) {
        if (jobs[i].result == 0) {
            ok_count++;
            if (show_progress)
                printf(" >> %-16s [OK]\n", REPO_NAMES[i]);
        } else {
            if (show_progress)
                printf(" >> %-16s (failed)\n", REPO_NAMES[i]);
        }
    }

    /* ── verify repo.db signatures before touching any metadata ───── *
     * Controlled by VERIFY_SIG in lpm.conf (default: off).            *
     * sig_required=1 → missing .sig is a hard abort.                  *
     * sig_required=0 → missing .sig is OK; a present-but-bad .sig    *
     *   still fails (TOFU: once you sign, the sig must be valid).     */
    for (int i = 0; i < NREPOS; i++) {
        if (jobs[i].result != 0) continue; /* already failed to fetch */

        /* Skip sig check entirely if VERIFY_SIG=0 (the default) */
        if (!g_cfg.verify_sig) {
            DBG(2, "[%s] repo.db sig check skipped (VERIFY_SIG=0)", REPO_NAMES[i]);
            continue;
        }

        int sig_ok = (sig_jobs[i].result == 0);
        const char *sigpath = sig_ok ? sig_jobs[i].dest : NULL;
        if (lpm_sig_verify(jobs[i].dest, sigpath, /*sig_required=*/1) != 0) {
            fprintf(stderr,
                "error: repo.db signature verification failed for [%s] — "
                "refusing to use this index\n", REPO_NAMES[i]);
            jobs[i].result = -1; /* mark failed so parse/persist loops skip it */
            ok_count--;
        } else {
            DBG(2, "[%s] repo.db signature OK", REPO_NAMES[i]);
        }
    }

    /* ── parse ─────────────────────────────────────────────────────── */
    int total_entries = 0;
    for (int i = 0; i < NREPOS; i++) {
        if (jobs[i].result != 0) continue;
        int n = parse_repo_db(jobs[i].dest, REPO_NAMES[i],
                              repo_entries + total_entries,
                              4096 - total_entries);
        DBG(1, "[%s] repo.db: %d packages", REPO_NAMES[i], n);
        total_entries += n;
    }
    *total_entries_out = total_entries;

    /* ── persist ───────────────────────────────────────────────────── */
    util_mkdirp("/var/lib/lpm/db", 0755);
    for (int i = 0; i < NREPOS; i++) {
        if (jobs[i].result != 0) continue;
        char persist[LPM_PATH_MAX];
        snprintf(persist, sizeof(persist),
                 "/var/lib/lpm/db/%s.db", REPO_NAMES[i]);
        util_copy_file(jobs[i].dest, persist);
        DBG(1, "persisted %s.db -> %s", REPO_NAMES[i], persist);
    }

    char rmcmd[LPM_PATH_MAX + 16];
    snprintf(rmcmd, sizeof(rmcmd), "rm -rf '%s'", db_dir);
    (void)system(rmcmd);

    return ok_count;
}

/* ── cmd_db_update — `lpm update`: sync repo databases only ────────────── *
 *   :: Syncing repositories...
 *
 *     -> base
 *     -> extra
 *     -> lotus
 *
 *   :: Package databases updated                                          */

/* ── db_count_pending_updates ────────────────────────────────────────── *
 * Reads the persisted /var/lib/lpm/db/{base,extra,lotus}.db (no network)
 * and counts installed packages with a newer version available.
 * Returns -1 if no persisted DBs exist yet (caller should suggest
 * `lpm update`). Used by `lpm audit`.                                    */
int db_count_pending_updates(void) {
    RepoEntry *repo_entries = calloc(4096, sizeof(RepoEntry));
    if (!repo_entries) return -1;

    int total_entries = 0;
    int any_db = 0;

    for (int i = 0; i < NREPOS; i++) {
        char path[LPM_PATH_MAX];
        snprintf(path, sizeof(path), "/var/lib/lpm/db/%s.db", REPO_NAMES[i]);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        any_db = 1;
        int n = parse_repo_db(path, REPO_NAMES[i],
                              repo_entries + total_entries,
                              4096 - total_entries);
        total_entries += n;
    }

    if (!any_db) { free(repo_entries); return -1; }

    InstalledPkg *all = NULL; int nall = 0;
    if (db_list_all(&all, &nall) != 0 || !all) { free(repo_entries); return 0; }

    LpmConfig cfg;
    lpm_config_load(LPM_CONF_FILE, &cfg);

    int count = 0;
    for (int i = 0; i < nall; i++) {
        if (lpm_config_is_ignored(&cfg, all[i].name)) continue;

        RepoEntry *found = NULL;
        for (int e = 0; e < total_entries; e++)
            if (!strcmp(repo_entries[e].name, all[i].name))
                found = &repo_entries[e];
        if (!found) continue;

        char inst_ver[LPM_VER_MAX + 16];
        snprintf(inst_ver, sizeof(inst_ver), "%s-%s", all[i].version, all[i].release);
        if (version_compare(inst_ver, found->version) < 0)
            count++;
    }

    free(all);
    free(repo_entries);
    return count;
}

void cmd_db_update(int argc, char **argv) {
    check_root();
    init_dirs();
    (void)argc; (void)argv;

    printf(":: Syncing repositories...\n\n");

    RepoEntry *repo_entries = calloc(4096, sizeof(RepoEntry));
    if (!repo_entries) die("Out of memory");
    int total_entries = 0;
    int ok = do_repo_sync(1, repo_entries, &total_entries);
    free(repo_entries);

    printf("\n");
    if (ok == 0) {
        fprintf(stderr, "error: All repositories failed to sync. Check your network connection.\n");
        exit(1);
    }
    printf(":: Package databases updated\n");
}

/* ── cmd_suy — public entry point ────────────────────────────────────── */
/* (do_repo_sync + cmd_db_update inserted here below) */

void cmd_suy(int argc, char **argv) {
    check_root();
    init_dirs();
    check_remove_journal();
    time_t _suy_start = time(NULL);

    LpmConfig cfg;
    lpm_config_load(LPM_CONF_FILE, &cfg);
    LpmFlags flags;
    char *flagargs[256];
    lpm_parse_flags(argc, argv, &flags, flagargs, 256);

    /* ── sync repos (always fresh — silent until update list known) ── */
    DBG(1, "starting repo sync (%d repos)", NREPOS);

    RepoEntry *repo_entries = calloc(4096, sizeof(RepoEntry));
    if (!repo_entries) die("Out of memory");
    int total_entries = 0;
    do_repo_sync(0, repo_entries, &total_entries);

    if (total_entries == 0) {
        fprintf(stderr, C_RED "error:" C_RESET
                " could not sync repositories — check your network connection.\n");
        free(repo_entries);
        exit(1);
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
            printf(":: No packages installed.\n");
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

    /* ── Phase 4b: scan repo for packages that replace installed ones ─
     * e.g.  swww (installed) → awww (new pkg with replaces=(swww))
     * We iterate all repo entries, parse their PKGBUILD for replaces=,
     * and if a replacement target is installed but the new pkg is not,
     * we queue it as a special "rename" upgrade entry.              */
    for (int e = 0; e < total_entries && nupdate < 1024; e++) {
        RepoEntry *re = &repo_entries[e];
        if (db_is_installed(re->name)) continue; /* new pkg already here */
        /* parse its PKGBUILD for replaces= */
        char pbf[LPM_PATH_MAX + LPM_NAME_MAX + 16];
        snprintf(pbf, sizeof(pbf), "%s/pkgbuild_%s",
                 LPM_PKGBUILD_DIR, re->name);
        PkgMeta meta;
        memset(&meta, 0, sizeof(meta));
        if (pkgbuild_parse_fast(pbf, &meta) != 0) continue;
        for (int ri = 0; ri < meta.nreplaces; ri++) {
            const char *old_name = meta.replaces[ri];
            if (!db_is_installed(old_name)) continue;
            if (lpm_config_is_ignored(&cfg, old_name)) continue;
            /* old_name is installed, new name is not → rename upgrade */
            char *old_ver = db_get_version(old_name);
            UpdateEntry *u = &updates[nupdate++];
            snprintf(u->name,         LPM_NAME_MAX,      "%s", re->name);
            snprintf(u->inst_ver,     LPM_VER_MAX + 15,  "%s",
                     old_ver ? old_ver : "?");
            snprintf(u->new_ver,      LPM_VER_MAX + 15,  "%s", re->version);
            snprintf(u->repo,         15,                "%s", re->repo);
            snprintf(u->replaces_old, LPM_NAME_MAX,      "%s", old_name);
            u->is_binary   = re->is_binary;
            u->is_critical = lpm_config_is_critical(&cfg, old_name);
            u->dl_size     = re->dl_size;
            u->inst_size   = re->inst_size;
            if (old_ver) free(old_ver);
        }
    }

    /* ── Phase 5: display ─────────────────────────────────────────── */
    if (nupdate == 0) {
        printf(":: Checking for updates...\n\n");
        printf(":: System is up to date\n");
        goto suy_cleanup;
    }

    printf(":: Checking for updates...\n\n");
    printf("Packages to upgrade (%d):\n", nupdate);

    long total_dl   = 0;
    long total_inst = 0;  /* binary installs only */
    long total_src_heuristic = 0;

    for (int i = 0; i < nupdate; i++) {
        UpdateEntry *u = &updates[i];

        /* rename indicator */
        char rename_tag[LPM_NAME_MAX + 16] = "";
        if (u->replaces_old[0])
            snprintf(rename_tag, sizeof(rename_tag),
                     " (replaces %s)", u->replaces_old);

        /* critical marker */
        const char *crit = u->is_critical ? " [CRITICAL]" : "";

        printf(" %-24s %s -> %s%s%s\n",
               u->name,
               u->inst_ver,
               u->new_ver,
               crit,
               rename_tag);

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
        if (total_dl > 0)
            printf("Download:   %s\n", dl_str);
        if (nignored)
            printf("(%d package(s) skipped — IgnorePkg)\n", nignored);

        DBG(1, "disk check: need %ld bytes, free %ld bytes", needed, free_bytes);
        if (free_bytes > 0 && free_bytes < needed) {
            char need_str[32];
            fmt_size(needed, need_str, sizeof(need_str));
            printf("\nerror: Not enough disk space — need %s, have %s.\n",
                   need_str, free_str);
            goto suy_cleanup;
        }
    }

    /* ── Phase 7: confirm ────────────────────────────────────────── */
    printf("\n");
    if (!flags.no_confirm) {
        if (!confirm(":: Proceed with upgrade? [Y/n] ")) {
            printf("Operation cancelled.\n");
            goto suy_cleanup;
        }
    }
    printf("\n");

    /* ── Phase 8: fetch PKGBUILDs + rebuild ─────────────────────── */
    int failed = 0;
    for (int i = 0; i < nupdate; i++) {
        CHECK_CANCEL(suy_done);
        UpdateEntry *u = &updates[i];

        printf("Upgrading %s (%d/%d)...\n", u->name, i + 1, nupdate);

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
            char part[LPM_PATH_MAX];
            snprintf(part, sizeof(part), "%s.part", dest);
            char cmd[2048];

            /* try directory layout first: <repo>/<letter>/<name>/PKGBUILD */
            {
                char _l = u->name[0];
                if (_l >= 'A' && _l <= 'Z') _l += 32;
                if (_l < 'a' || _l > 'z')  _l = '0';
                snprintf(url_found, sizeof(url_found),
                         "%s/%s/%c/%s/PKGBUILD",
                         REPO_BASE, try_repos[r], _l, u->name);
            }
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
                continue;
            }
            remove(part);

            /* fallback: flat layout <repo>/<letter>/pkgbuild_<name> */
            {
                char _l = u->name[0];
                if (_l >= 'A' && _l <= 'Z') _l += 32;
                if (_l < 'a' || _l > 'z')  _l = '0';
                snprintf(url_found, sizeof(url_found),
                         "%s/%s/%c/pkgbuild_%s",
                         REPO_BASE, try_repos[r], _l, u->name);
            }
            snprintf(cmd, sizeof(cmd),
                "wget -q --timeout=30 --tries=2 -O '%s' '%s' 2>/dev/null"
                " || curl -sL --connect-timeout 30 -o '%s' '%s' 2>/dev/null",
                part, url_found, part, url_found);
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
        (void)system(rmcache);

        /* delegate to cmd_sync which handles build + merge + db */
        char *pair[1] = { u->name };
        cmd_sync(1, pair);

        lpm_log("Updated %s  %s -> %s", u->name, u->inst_ver, u->new_ver);
    }

suy_done:
    printf("\n");
    if (failed)
        printf("warning: %d package(s) failed\n", failed);
    else {
        long _suy_elapsed = (long)(time(NULL) - _suy_start);
        printf(":: Transaction completed (%ldm%lds)\n",
               _suy_elapsed / 60, _suy_elapsed % 60);
    }

suy_cleanup:
    free(repo_entries);
    free(updates);
    if (targets_alloc)
        for (int i = 0; i < ntargets; i++) free(targets[i]);
}

#pragma GCC diagnostic pop
