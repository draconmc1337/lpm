#include "lpm.h"

static void usage(void) {
    printf(C_BOLD "lpm" C_RESET " — Lotus Package Manager " C_CYAN LPM_VERSION C_RESET "\n\n");
    printf("Usage: lpm <command> [package(s)]\n\n");
    printf("  -S   <pkg...>   Fetch PKGBUILD from repo + build + install\n");
    printf("  -bi  <pkg...>   Build + install using local PKGBUILD (offline)\n");
    printf("  -Sy  <pkg...>   Fetch PKGBUILD only (no build)\n");
    printf("  -c   <pkg...>   Run test suite\n");
    printf("  -r   <pkg...>   Remove  (--force to skip dep check)\n");
    printf("  -rcc [pkg...]   Remove build cache\n");
    printf("  -u   [pkg...]   Update  (all installed if no args)\n");
    printf("  -s   <term>     Search available packages\n");
    printf("  -D   <pkg...>   Show dependency tree\n");
    printf("  -qi  <pkg...>   Show package info\n");
    printf("  -l              List installed packages\n");
    printf("  -v              Print version\n");
    printf("  -h              Show this help\n\n");
    printf("PKGBUILD: %s/pkgbuild_<name>\n", LPM_PKGBUILD_DIR);
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 0; }

    const char *cmd = argv[1];
    int  sub_argc   = argc - 2;
    char **sub_argv = argv + 2;

    if      (strcmp(cmd, "-S")   == 0) cmd_sync(sub_argc, sub_argv);
    else if (strcmp(cmd, "-bi")  == 0) cmd_local(sub_argc, sub_argv);
    else if (strcmp(cmd, "-Sy")  == 0) cmd_fetch(sub_argc, sub_argv);
    else if (strcmp(cmd, "-c")   == 0) cmd_check(sub_argc, sub_argv);
    else if (strcmp(cmd, "-r")   == 0) cmd_remove(sub_argc, sub_argv);
    else if (strcmp(cmd, "-rcc") == 0) cmd_rcc(sub_argc, sub_argv);
    else if (strcmp(cmd, "-u")   == 0) cmd_update(sub_argc, sub_argv);
    else if (strcmp(cmd, "-s")   == 0) cmd_search(sub_argc, sub_argv);
    else if (strcmp(cmd, "-D")   == 0) cmd_deptree(sub_argc, sub_argv);
    else if (strcmp(cmd, "-qi")  == 0) cmd_info(sub_argc, sub_argv);
    else if (strcmp(cmd, "-l")   == 0) cmd_list();
    else if (strcmp(cmd, "-v")   == 0) printf("lpm %s\n", LPM_VERSION);
    else if (strcmp(cmd, "-h")   == 0 ||
             strcmp(cmd, "--help") == 0) usage();
    else {
        fprintf(stderr, C_RED "error: " C_RESET "Unknown command: %s\n", cmd);
        fprintf(stderr, "Run 'lpm -h' for help.\n");
        return 1;
    }

    return 0;
}
