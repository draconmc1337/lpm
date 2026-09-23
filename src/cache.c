#include "lpm.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result"
#pragma GCC diagnostic ignored "-Wformat-truncation"
#pragma GCC diagnostic ignored "-Wstringop-truncation"

/* ── dir_size_bytes ──────────────────────────────────────────────────── *
 * Recursive directory size in bytes. Self-contained (no `du` dependency,
 * which formats inconsistently across glibc-host vs musl/busybox targets
 * and prints a bare "0" with no unit for empty dirs).                   */
static long dir_size_bytes(const char *path) {
    long total = 0;
    DIR *d = opendir(path);
    if (!d) return 0;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        char sub[MAX_STR];
        snprintf(sub, sizeof(sub), "%s/%s", path, ent->d_name);
        struct stat st;
        if (lstat(sub, &st) != 0) continue;
        if (S_ISDIR(st.st_mode))
            total += dir_size_bytes(sub);
        else
            total += st.st_size;
    }
    closedir(d);
    return total;
}

/* count cached package dirs + total size under LPM_BUILD_DIR */
static void cache_stats(int *n_pkgs_out, long *bytes_out) {
    int npkgs = 0;
    long total = 0;
    DIR *d = opendir(LPM_BUILD_DIR);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d))) {
            if (ent->d_name[0] == '.') continue;
            char sub[MAX_STR];
            snprintf(sub, sizeof(sub), "%s/%s", LPM_BUILD_DIR, ent->d_name);
            struct stat st;
            if (lstat(sub, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
            npkgs++;
            total += dir_size_bytes(sub);
        }
        closedir(d);
    }
    if (n_pkgs_out) *n_pkgs_out = npkgs;
    if (bytes_out)  *bytes_out  = total;
}

void cmd_rcc(int argc, char **argv) {
    check_root(); init_dirs();

    int clean = (argc > 0 &&
                 (!strcmp(argv[0], "clean") || !strcmp(argv[0], "clear")));

    /* ── VIEW: `lpm cache` ─────────────────────────────────────────── */
    if (!clean) {
        int npkgs; long bytes;
        cache_stats(&npkgs, &bytes);
        char sz[32]; format_size(bytes, sz, sizeof(sz));
        printf("Build cache\n\n");
        printf("Packages: %d\n", npkgs);
        printf("Disk usage: %s\n", sz);
        printf("Location: %s\n", LPM_BUILD_DIR);
        printf("\nUse 'lpm cache clean' to remove cached build data.\n");
        return;
    }

    /* ── CLEAN: `lpm cache clean [pkg...]` ─────────────────────────── */
    int npkgs; long bytes;
    cache_stats(&npkgs, &bytes);
    char sz[32]; format_size(bytes, sz, sizeof(sz));
    printf("Build cache\n\n");
    printf("Packages: %d\n", npkgs);
    printf("Disk usage: %s\n\n", sz);

    if (npkgs == 0) { printf("Nothing to clean.\n"); return; }

    if (!confirm("Clean build cache? [Y/n] ")) { printf("Interrupted.\n"); return; }

    printf("\nRemoving cached build data...\n");
    if (argc > 1) {
        /* specific packages */
        for (int i = 1; i < argc; i++) {
            char cachedir[MAX_STR];
            snprintf(cachedir, sizeof(cachedir), "%s/%s", LPM_BUILD_DIR, argv[i]);
            char rm_cmd[MAX_CMD];
            snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf '%s'", cachedir);
            (void)system(rm_cmd);
            lpm_log("Cache removed: %s", argv[i]);
        }
    } else {
        /* all uninstalled package caches */
        DIR *d = opendir(LPM_BUILD_DIR);
        if (d) {
            struct dirent *ent;
            while ((ent = readdir(d))) {
                if (ent->d_name[0] == '.') continue;
                if (db_is_installed(ent->d_name)) continue;
                char cachedir[MAX_STR], rm_cmd[MAX_CMD];
                snprintf(cachedir, sizeof(cachedir), "%s/%s", LPM_BUILD_DIR, ent->d_name);
                snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf '%s'", cachedir);
                (void)system(rm_cmd);
                lpm_log("Cache removed: %s", ent->d_name);
            }
            closedir(d);
        }
    }
    printf("Done.\n\n");
    printf("Reclaimed %s.\n", sz);
}

#pragma GCC diagnostic pop
