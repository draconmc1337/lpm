#include "lpm.h"
#include <signal.h>

static volatile sig_atomic_t g_interrupted = 0;

static void sig_handler(int sig) {
    (void)sig;
    g_interrupted = 1;
    lpm_lock_release();
    _exit(130);
}

/* ── usage ───────────────────────────────────────────────────────────── */

static void usage(void) {
    printf(
        "usage: lpm <command> [options] [package(s)]\n"
        "\n"
        "package management:\n"
        "  install  <pkg(s)>    install packages\n"
        "  remove   <pkg(s)>    remove packages\n"
        "  upgrade  [pkg(s)]    upgrade packages (all if none given)\n"
        "  update               sync package databases\n"
        "  search   <term>      search available packages\n"
        "  info     <pkg(s)>    show package information\n"
        "  deps     [pkg(s)]    show dependency tree\n"
        "\n"
        "system:\n"
        "  cache    [clean]     manage build cache\n"
        "  verify   [pkg(s)]    verify installed packages\n"
        "  audit                show audit log\n"
        "  repo     <sub>       repo management\n"
        "\n"
        "query:\n"
        "  list                 list installed packages\n"
        "  owns     <path>      which package owns a file\n"
        "  files    <pkg>       list files in package\n"
        "  orphans              show orphaned packages\n"
        "\n"
        "tools:\n"
        "  -P  <sub>            package tools (build/pack)\n"
        "  -K  <sub>            key management\n"
        "\n"
        "options:\n"
        "  --no-confirm         skip confirmation prompt\n"
        "  --no-check           skip check() phase\n"
        "  --dry-run            simulate without making changes\n"
        "  --force              override conflict checks\n"
        "  --debug=N            debug level 1-3\n"
        "  -V, --version        show version\n"
        "  -h, --help           show this help\n"
    );
}

static void usage_P(void) {
    printf(
        "usage: lpm -P <subcommand> [args]\n"
        "\n"
        "package tool subcommands:\n"
        "  build   <pkg>             build package from PKGBUILD\n"
        "  pack    <pkg>             build and pack → .lpkg binary\n"
        "  install <pkg.lpkg|name>   install from .lpkg file\n"
        "  query   [pkg.lpkg|name]   query cached .lpkg or show info\n"
        "  extract <pkg.lpkg|name>   extract .lpkg into current dir\n"
        "  verify  <pkg.lpkg|name>   verify .lpkg checksum + metadata\n"
        "  remove  <pkg.lpkg|name>   remove cached .lpkg file\n"
    );
}

static void usage_K(void) {
    printf(
        "usage: lpm -K <subcommand> [args]\n"
        "\n"
        "key subcommands:\n"
        "  init                initialize lpm keyring\n"
        "  genid               generate a random key id\n"
        "  list                list keys in keyring\n"
        "  recv   <keyid>      receive key from keyserver\n"
        "  import <file>       import key file\n"
        "  trust  <keyid>      set key to ultimate trust\n"
    );
}

/* ── debug flag parser ───────────────────────────────────────────────── */

static int parse_debug_level(const char *s) {
    if (!s || s[0] < '1' || s[0] > '3' || s[1] != '\0') return -1;
    return s[0] - '0';
}

/* ── -P subcommand dispatch ──────────────────────────────────────────── */

static int dispatch_P(int argc, char **argv) {
    if (argc < 1) { usage_P(); return 0; }

    const char *sub = argv[0];
    int sub_argc = argc - 1;
    char **sub_argv = argv + 1;

    if (!strcmp(sub, "build") || !strcmp(sub, "b"))
        cmd_pack(sub_argc, sub_argv);          /* build + pack */
    else if (!strcmp(sub, "pack"))
        cmd_pack(sub_argc, sub_argv);
    else if (!strcmp(sub, "install") || !strcmp(sub, "i"))
        cmd_pkginstall(sub_argc, sub_argv);
    else if (!strcmp(sub, "query") || !strcmp(sub, "q"))
        cmd_pkglist(sub_argc, sub_argv);
    else if (!strcmp(sub, "extract") || !strcmp(sub, "e"))
        cmd_pkginstall_dir(sub_argc, sub_argv);
    else if (!strcmp(sub, "verify") || !strcmp(sub, "v"))
        cmd_pkgverify(sub_argc, sub_argv);
    else if (!strcmp(sub, "remove") || !strcmp(sub, "r"))
        cmd_pkgremove_file(sub_argc, sub_argv);
    else if (!strcmp(sub, "--help") || !strcmp(sub, "help") || !strcmp(sub, "-h"))
        usage_P();
    else {
        fprintf(stderr,
            C_RED "error:" C_RESET " unknown -P subcommand '%s'\n"
            "run 'lpm -P help' for available subcommands\n", sub);
        return 1;
    }
    return 0;
}

/* ── -K subcommand dispatch ──────────────────────────────────────────── */

static int dispatch_K(int argc, char **argv) {
    if (argc < 1) { usage_K(); return 0; }
    /* cmd_key already handles all subcommands internally */
    if (!strcmp(argv[0], "--help") || !strcmp(argv[0], "help") || !strcmp(argv[0], "-h")) {
        usage_K();
        return 0;
    }
    cmd_key(argc, argv);
    return 0;
}

/* ── lock helpers ────────────────────────────────────────────────────── */

/* commands that don't need root / write lock */
static int is_readonly_cmd(const char *cmd) {
    return !strcmp(cmd, "search")  ||
           !strcmp(cmd, "info")    ||
           !strcmp(cmd, "list")    ||
           !strcmp(cmd, "owns")    ||
           !strcmp(cmd, "files")   ||
           !strcmp(cmd, "orphans") ||
           !strcmp(cmd, "deps")    ||
           !strcmp(cmd, "audit")   ||
           !strcmp(cmd, "-V")      ||
           !strcmp(cmd, "--version")||
           !strcmp(cmd, "-h")      ||
           !strcmp(cmd, "--help");
}

/* ── main ────────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    /* ── debug flag: LPM_DEBUG env ─── */
    char *env_dbg = getenv("LPM_DEBUG");
    if (env_dbg && *env_dbg) {
        int lvl = parse_debug_level(env_dbg);
        if (lvl < 0) {
            fprintf(stderr,
                C_RED "error: " C_RESET
                "invalid LPM_DEBUG='%s' (use 1..3)\n", env_dbg);
            return 1;
        }
        g_verbose = lvl;
    }

    /* ── strip --debug=N from argv ─── */
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "--debug=", 8)) {
            int lvl = parse_debug_level(argv[i] + 8);
            if (lvl < 0) {
                fprintf(stderr,
                    C_RED "error: " C_RESET
                    "invalid --debug level (use 1..3)\n");
                return 1;
            }
            g_verbose = lvl;
            for (int j = i; j < argc - 1; j++) argv[j] = argv[j + 1];
            argc--; i--;
        }
    }

    if (argc < 2) {
        fprintf(stderr,
            C_RED "error: " C_RESET
            "no command specified (use -h for help)\n");
        return 1;
    }

    const char *cmd = argv[1];
    int sub_argc = argc - 2;
    char **sub_argv = argv + 2;

    /* ── top-level help / version shortcuts ─── */
    if (!strcmp(cmd, "-h") || !strcmp(cmd, "--help") || !strcmp(cmd, "help")) {
        usage();
        return 0;
    }
    if (!strcmp(cmd, "-V") || !strcmp(cmd, "--version") || !strcmp(cmd, "version")) {
        printf("lpm " LPM_VERSION " — libllpm 1.1.2-beta\n");
        printf("Lotus Linux (musl/x86_64)\n");
        printf("Copyright (C) Lotus Linux Project\n");
        return 0;
    }

    /* ── per-command help: lpm <cmd> --help ─── */
    if (sub_argc >= 1 &&
        (!strcmp(sub_argv[0], "--help") || !strcmp(sub_argv[0], "-h"))) {
        if (!strcmp(cmd, "-P") || !strcmp(cmd, "pack") || !strcmp(cmd, "package"))
            usage_P();
        else if (!strcmp(cmd, "-K") || !strcmp(cmd, "key"))
            usage_K();
        else
            usage();
        return 0;
    }

    /* ── guard against multiple operation flags (old pacman-style) ─── */
    {
        static const char *OP_LIST[] = {
            "-S", "--sync", "-R", "--remove", "-Q", "--query",
            "-D", "--database", "-U", "--upgrade", "-F", "--files",
            NULL
        };
        for (int i = 0; i < sub_argc; i++) {
            for (int k = 0; OP_LIST[k]; k++) {
                if (!strcmp(sub_argv[i], OP_LIST[k])) {
                    fprintf(stderr,
                        C_RED "error:" C_RESET
                        " only one command may be used at a time\n");
                    return 1;
                }
            }
        }
    }

    /* ── acquire lock for write operations ─── */
    int needs_lock = !is_readonly_cmd(cmd);
    if (needs_lock) {
        signal(SIGINT,  sig_handler);
        signal(SIGTERM, sig_handler);
        signal(SIGHUP,  sig_handler);

        int lock_rc = lpm_lock_acquire();
        if (lock_rc == -1) {
            fprintf(stderr,
                C_RED "error: " C_RESET
                "permission denied — run lpm as root (doas/sudo)\n");
            return 1;
        } else if (lock_rc == -2) {
            fprintf(stderr,
                C_RED "error: " C_RESET
                "lpm is already running — only one instance at a time\n"
                "  if stale, remove: " C_CYAN LPM_LOCK_FILE C_RESET "\n");
            return 1;
        }
    }

    /* ══════════════════════════════════════════════════════════════════
     * Command dispatch
     * ══════════════════════════════════════════════════════════════════ */

    /* ── install ─────────────────────────────────────────────────────── */
    if (!strcmp(cmd, "install") || !strcmp(cmd, "-S"))
        cmd_sync(sub_argc, sub_argv);

    /* ── remove ──────────────────────────────────────────────────────── */
    else if (!strcmp(cmd, "remove") || !strcmp(cmd, "-R"))
        cmd_remove(sub_argc, sub_argv);

    /* ── upgrade: no args = full system upgrade; args = specific pkgs ── */
    else if (!strcmp(cmd, "upgrade") || !strcmp(cmd, "-U")) {
        if (sub_argc > 0)
            cmd_update(sub_argc, sub_argv);  /* upgrade specific packages */
        else
            cmd_suy(0, NULL);                /* full system upgrade */
    }

    /* ── update: sync repo databases ─────────────────────────────────── */
    else if (!strcmp(cmd, "update"))
        cmd_suy(0, NULL);                    /* fetch DBs + check for updates */

    /* ── search ──────────────────────────────────────────────────────── */
    else if (!strcmp(cmd, "search"))
        cmd_search(sub_argc, sub_argv);

    /* ── info ────────────────────────────────────────────────────────── */
    else if (!strcmp(cmd, "info"))
        cmd_info(sub_argc, sub_argv);

    /* ── deps ────────────────────────────────────────────────────────── */
    else if (!strcmp(cmd, "deps") || !strcmp(cmd, "-D"))
        cmd_deptree(sub_argc, sub_argv);

    /* ── list ────────────────────────────────────────────────────────── */
    else if (!strcmp(cmd, "list") || !strcmp(cmd, "-Q"))
        cmd_list(sub_argc, sub_argv);

    /* ── owns ────────────────────────────────────────────────────────── */
    else if (!strcmp(cmd, "owns"))
        cmd_owns(sub_argc, sub_argv);

    /* ── files ───────────────────────────────────────────────────────── */
    else if (!strcmp(cmd, "files"))
        cmd_files(sub_argc, sub_argv);

    /* ── orphans ─────────────────────────────────────────────────────── */
    else if (!strcmp(cmd, "orphans"))
        cmd_orphans(sub_argc, sub_argv);

    /* ── cache ───────────────────────────────────────────────────────── */
    else if (!strcmp(cmd, "cache")) {
        /* lpm cache         → show cache info
         * lpm cache clean   → clean uninstalled build caches */
        if (sub_argc > 0 &&
            (!strcmp(sub_argv[0], "clean") || !strcmp(sub_argv[0], "clear")))
            cmd_rcc(sub_argc - 1, sub_argv + 1);
        else
            cmd_rcc(sub_argc, sub_argv);
    }

    /* ── verify ──────────────────────────────────────────────────────── */
    else if (!strcmp(cmd, "verify"))
        cmd_check(sub_argc, sub_argv);

    /* ── audit ───────────────────────────────────────────────────────── */
    else if (!strcmp(cmd, "audit")) {
        /* dump the audit log — simple, no extra function needed */
        const char *log = LPM_AUDIT_LOG;
        FILE *f = fopen(log, "r");
        if (!f) {
            printf("No audit log found at %s\n", log);
        } else {
            char line[1024];
            while (fgets(line, sizeof(line), f))
                fputs(line, stdout);
            fclose(f);
        }
    }

    /* ── repo ────────────────────────────────────────────────────────── */
    else if (!strcmp(cmd, "repo")) {
        /* placeholder: repo management subcommands go here in future */
        if (sub_argc == 0) {
            printf(
                "usage: lpm repo <subcommand>\n"
                "subcommands:\n"
                "  list          list configured repos\n"
                "  add  <url>    add a repo\n"
                "  remove <name> remove a repo\n"
                "  sync          sync repo databases\n"
            );
        } else {
            fprintf(stderr,
                C_YELLOW "warning:" C_RESET
                " repo subcommands not yet implemented\n");
        }
    }

    /* ── -P: package tools ───────────────────────────────────────────── */
    else if (!strcmp(cmd, "-P")) {
        int rc = dispatch_P(sub_argc, sub_argv);
        if (needs_lock && !g_interrupted) lpm_lock_release();
        return rc;
    }

    /* ── -K: key management ──────────────────────────────────────────── */
    else if (!strcmp(cmd, "-K")) {
        int rc = dispatch_K(sub_argc, sub_argv);
        if (needs_lock && !g_interrupted) lpm_lock_release();
        return rc;
    }

    /* ── unknown command ─────────────────────────────────────────────── */
    else {
        if (cmd[0] == '-')
            fprintf(stderr,
                C_RED "error: " C_RESET "unknown option '%s'\n", cmd);
        else
            fprintf(stderr,
                C_RED "error: " C_RESET
                "unknown command '%s' (use 'lpm help')\n", cmd);
        if (needs_lock) lpm_lock_release();
        return 1;
    }

    if (needs_lock && !g_interrupted) lpm_lock_release();
    return 0;
}
