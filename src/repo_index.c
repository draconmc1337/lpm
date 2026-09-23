/*
 * src/repo_index.c — one in-process repository index per command.
 *
 * Formerly every consumer of repo metadata re-parsed the synced
 * /var/lib/lpm/db/<repo>.db files for every package it touched: dep.c
 * resolved dependencies by stat()ing local PKGBUILDs only, build.c's
 * pkg_locate_from_db() parsed up to three repo.db files per lookup, and
 * search.c re-parsed the whole index once per command-line argument. None
 * of them shared an index, and the resolver never consulted the repository
 * at all — so a dependency that existed only in the repo was invisible.
 *
 * This module owns the single shared index: all synced <repo>.db files
 * loaded once, cached for the process, and resettable after `lpm update`.
 * Lookups are O(index), not O(files × repos).
 *
 * Determinism: the repo.db files are enumerated in lexicographic order, so
 * a package present in two repos always resolves from the same one.
 */
#include "lpm.h"
#include <dirent.h>

#define MAX_REPO_FILES 32

static RepoEntry *g_entries = NULL;
static int g_nentries = 0;
static int g_cap = 0;
static int g_loaded = 0;

static int cmp_dbfile(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* Enumerate every *.db under LPM_DB_DIR in lexicographic order. Writes up
 * to maxn pointers into out and returns the count. */
static int list_db_files(char **out, int maxn) {
    DIR *d = opendir(LPM_DB_DIR);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *name = e->d_name;
        size_t len = strlen(name);
        if (len < 3 || strcmp(name + len - 3, ".db") != 0) continue;
        if (len <= 3) continue;              /* ".db" alone is not a repo   */
        if (n >= maxn) break;
        char *copy = strdup(name);
        if (!copy) continue;
        out[n++] = copy;
    }
    closedir(d);
    qsort(out, (size_t)n, sizeof(char *), cmp_dbfile);
    return n;
}

static void index_free(void) {
    free(g_entries);
    g_entries = NULL;
    g_nentries = 0;
    g_cap = 0;
    g_loaded = 0;
}

void dep_repo_index_reset(void) {
    index_free();
}

int repo_index_size(void) {
    if (!g_loaded && repo_index_load() < 0) return 0;
    return g_nentries;
}

int repo_index_load(void) {
    if (g_loaded) return g_nentries;

    char *files[MAX_REPO_FILES];
    int nfiles = list_db_files(files, MAX_REPO_FILES);
    if (nfiles < 0) return -1;

    for (int i = 0; i < nfiles; i++) {
        if (g_nentries >= LPM_MAX_REPO_ENTRIES) {
            ui_err("error: repository index overflow (>%d entries); "
                   "refusing to truncate silently", LPM_MAX_REPO_ENTRIES);
            for (int j = i; j < nfiles; j++) free(files[j]);
            index_free();
            return -1;
        }
        char path[LPM_PATH_MAX];
        int n = snprintf(path, sizeof(path), "%s/%s", LPM_DB_DIR, files[i]);
        if (n < 0 || (size_t)n >= sizeof(path)) { free(files[i]); continue; }

        /* grow once per file; parse_repo_db reports the true entry count */
        RepoEntry *tmp = realloc(g_entries,
                                 (size_t)(g_nentries + 4096) * sizeof(RepoEntry));
        if (!tmp) {
            ui_err("error: out of memory loading repository index");
            for (int j = i; j < nfiles; j++) free(files[j]);
            index_free();
            return -1;
        }
        g_entries = tmp;
        g_cap = g_nentries + 4096;

        char repo[64];
        snprintf(repo, sizeof(repo), "%s", files[i]);
        repo[strlen(repo) - 3] = '\0';       /* strip ".db" */

        int got = parse_repo_db(path, repo, g_entries + g_nentries,
                                g_cap - g_nentries);
        if (got < 0) {
            ui_err("error: cannot read repository index %s", path);
            for (int j = i; j < nfiles; j++) free(files[j]);
            index_free();
            return -1;
        }
        g_nentries += got;
        free(files[i]);
    }
    g_loaded = 1;
    DBG(1, "repository index: %d package(s) from %d repo.db file(s)",
        g_nentries, nfiles);
    return g_nentries;
}

int repo_index_find(const char *name, RepoEntry *out) {
    if (!name || !name[0]) return -1;
    if (!g_loaded && repo_index_load() < 0) return -1;
    for (int i = 0; i < g_nentries; i++) {
        if (strcmp(g_entries[i].name, name) == 0) {
            if (out) *out = g_entries[i];
            return 0;
        }
    }
    return -1;
}

int repo_index_find_provider(const char *virtual_name, const DepSpec *spec,
                             char *provider_out, size_t provider_sz) {
    if (!virtual_name || !virtual_name[0] || !provider_out || provider_sz < 2)
        return -1;
    if (!g_loaded && repo_index_load() < 0) return -1;

    /* Deterministic: the first repo (lexicographic) that declares the
     * virtual name and satisfies the version constraint wins. */
    for (int i = 0; i < g_nentries; i++) {
        const RepoEntry *e = &g_entries[i];
        for (int p = 0; p < e->nprovides && p < LPM_MAX_DEPS; p++) {
            DepSpec prov;
            dep_parse(e->provides[p], &prov);
            if (strcmp(prov.name, virtual_name) != 0) continue;
            /* If the dependency carries a version constraint, the
             * provider's provides= entry must carry a version that
             * satisfies it; a versionless provides= satisfies only ANY. */
            if (spec && spec->op != LLPM_DEP_ANY) {
                if (prov.op == LLPM_DEP_ANY || !prov.ver[0]) break;
                if (!dep_constraint_satisfied(spec, prov.ver)) break;
            }
            snprintf(provider_out, provider_sz, "%s", e->name);
            return 0;
        }
    }
    return -1;
}
