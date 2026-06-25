#include "lpm.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result"
#pragma GCC diagnostic ignored "-Wformat-truncation"
#pragma GCC diagnostic ignored "-Wstringop-truncation"


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
            /* get size */
            char size_cmd[MAX_CMD];
            snprintf(size_cmd, sizeof(size_cmd), "du -sh '%s' 2>/dev/null | cut -f1", cachedir);
            char sz[32] = "?";
            FILE *p = popen(size_cmd, "r");
            if (p) { (void)fgets(sz, sizeof(sz), p); sz[strcspn(sz, "\n")] = '\0'; pclose(p); }

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
    for (int i = 0; i < ntargets; i++) {
        char cachedir[MAX_STR];
        snprintf(cachedir, sizeof(cachedir), "%s/%s", LPM_BUILD_DIR, targets[i]);
        char size_cmd[MAX_CMD];
        snprintf(size_cmd, sizeof(size_cmd), "du -sh '%s' 2>/dev/null | cut -f1", cachedir);
        char sz[32] = "?";
        FILE *p = popen(size_cmd, "r");
        if (p) { (void)fgets(sz, sizeof(sz), p); sz[strcspn(sz, "\n")] = '\0'; pclose(p); }
        printf("  " C_BOLD "%-24s" C_RESET "  %s\n", targets[i], sz);
    }
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
