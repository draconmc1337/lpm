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

void cmd_rcc(int argc, char **argv) {
    check_root(); init_dirs();

    if (argc > 0) {
        /* specific packages */
        for (int i = 0; i < argc; i++) {
            char cachedir[MAX_STR];
            snprintf(cachedir, sizeof(cachedir), "%s/%s", LPM_BUILD_DIR, argv[i]);
            struct stat st;
            if (stat(cachedir, &st) != 0) {
                printf("  " C_YELLOW "%s" C_RESET ": no cache found\n", argv[i]);
                continue;
            }
            long bytes = dir_size_bytes(cachedir);
            char sz[32];
            if (bytes > 0) format_size(bytes, sz, sizeof(sz));
            else           snprintf(sz, sizeof(sz), "0 B");

            printf("  Cleaning " C_BOLD "%s" C_RESET " (%s)...", argv[i], sz);
            fflush(stdout);
            char rm_cmd[MAX_CMD];
            snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf '%s'", cachedir);
            (void)system(rm_cmd);
            printf(" " C_GREEN "done" C_RESET "\n");
            lpm_log("Cache removed: %s", argv[i]);
        }
        return;
    }

    /* no args — clean all uninstalled packages */
    DIR *d = opendir(LPM_BUILD_DIR);
    if (!d) { printf("Nothing to clean.\n"); return; }

    char *targets[256]; int ntargets = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) && ntargets < 256) {
        if (ent->d_name[0] == '.') continue;
        if (!db_is_installed(ent->d_name))
            targets[ntargets++] = strdup(ent->d_name);
    }
    closedir(d);

    if (ntargets == 0) { printf("Nothing to clean.\n"); return; }

    printf("Cache of uninstalled packages (" C_BOLD "%d" C_RESET "):\n", ntargets);
    int nhidden = 0;
    for (int i = 0; i < ntargets; i++) {
        char cachedir[MAX_STR];
        snprintf(cachedir, sizeof(cachedir), "%s/%s", LPM_BUILD_DIR, targets[i]);
        long bytes = dir_size_bytes(cachedir);
        if (bytes == 0) { nhidden++; continue; }
        char sz[32];
        format_size(bytes, sz, sizeof(sz));
        printf("  " C_BOLD "%-24s" C_RESET "  %s\n", targets[i], sz);
    }
    if (nhidden > 0)
        printf("  " C_GRAY "+ %d empty cache dir(s) (0 B, hidden)" C_RESET "\n", nhidden);
    printf("\n");
    if (!confirm("Remove cache(s)? [y/N] ")) { printf("Aborted.\n"); goto cleanup; }

    for (int i = 0; i < ntargets; i++) {
        char cachedir[MAX_STR];
        snprintf(cachedir, sizeof(cachedir), "%s/%s", LPM_BUILD_DIR, targets[i]);
        char rm_cmd[MAX_CMD];
        snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf '%s'", cachedir);
        (void)system(rm_cmd);
        lpm_log("Cache removed: %s", targets[i]);
    }
    printf(C_CYAN "::" C_RESET " " C_GREEN "Cache cleaned." C_RESET "\n");

cleanup:
    for (int i = 0; i < ntargets; i++) free(targets[i]);
}

#pragma GCC diagnostic pop
