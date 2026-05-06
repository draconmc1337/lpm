#include "lpm.h"
#include <signal.h>

static volatile sig_atomic_t g_interrupted = 0;

/* ── signal handler: release lock and exit cleanly ──────────────────── */
static void sig_handler(int sig) {
    (void)sig;
    g_interrupted = 1;
    lpm_lock_release();
    _exit(130);
}

static void usage(void) {
  printf("usage:  lpm <operation> [...]\n"
         "operations:\n"
         "    lpm {-h --help}\n"
         "    lpm {-V --version}\n"
         "    lpm {-D --deps}     [options] [package(s)]\n"
         "    lpm {-Q --query}    [options] [package(s)]\n"
         "    lpm {-K --key}      <subcommand> [args]\n"
         "    lpm {-R --remove}   [options] <package(s)>\n"
         "    lpm {-S --sync}     [options] [package(s)]\n"
         "    lpm {-U --upgrade}  [options] [package(s)]\n"
         "\n"
         "use 'lpm {-h --help}' with an operation for available options\n");
}

static void usage_op(const char *op) {
  if ((op[0] == '-' && op[1] == 'S') || !strcmp(op, "--sync")) {
    printf("usage: lpm -S [options] <package(s)>\n"
           "sync options:\n"
           "  -S           install target package(s)\n"
           "  -Sy          fetch PKGBUILD(s) only\n"
           "  -Syu         full system update\n"
           "  --no-confirm skip confirmation prompt\n"
           "  --no-recommend skip recommend package handling\n"
           "  --no-check   skip check() phase\n");
    return;
  }
  if (!strcmp(op, "-Q") || !strcmp(op, "--query")) {
    printf("usage: lpm -Q [options] [package(s)]\n"
           "query options:\n"
           "  -Q       list installed packages\n"
           "  -qi      show package info\n"
           "  -Qo      show orphaned packages\n");
    return;
  }
  if (!strcmp(op, "-D") || !strcmp(op, "--deps")) {
    printf("usage: lpm -D [package(s)]\n"
           "dependency options:\n"
           "  -D       show dependency tree\n");
    return;
  }
  usage();
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, C_RED "error: " C_RESET
                    "no operation specified (use -h for help)\n");
    return 1;
  }

  const char *cmd = argv[1];
  if (argc >= 3 && (!strcmp(argv[2], "--help") || !strcmp(argv[2], "-h"))) {
    usage_op(cmd);
    return 0;
  }
  if (!strcmp(cmd, "--u")) cmd = "-u";

  int sub_argc = argc - 2;
  char **sub_argv = argv + 2;

  if (!strcmp(cmd, "--sync")) {
    cmd = "-S";
  } else if (cmd[0] == '-' && cmd[1] == 'S') {
    cmd = "-S";
  } else if (!strcmp(cmd, "--sync-only")) {
    cmd = "-S";
  } else if (!strcmp(cmd, "--query")) {
    cmd = "-Q";
  } else if (!strcmp(cmd, "--deps")) {
    cmd = "-D";
  } else if (!strcmp(cmd, "--remove")) {
    cmd = "-R";
  } else if (!strcmp(cmd, "--upgrade")) {
    cmd = "-U";
  } else if (!strcmp(cmd, "--version")) {
    cmd = "-V";
  } else if (!strcmp(cmd, "--key")) {
    cmd = "-K";
  }

  int needs_lock = strcmp(cmd, "-s") != 0 && strcmp(cmd, "-qi") != 0 &&
                   strcmp(cmd, "-Q") != 0 && strcmp(cmd, "-Qo") != 0 &&
                   strcmp(cmd, "-V") != 0 && strcmp(cmd, "-v") != 0 &&
                   strcmp(cmd, "-h") != 0 && strcmp(cmd, "--help") != 0 &&
                   strcmp(cmd, "-D") != 0;

  if (needs_lock) {
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGHUP, sig_handler);

    int lock_rc = lpm_lock_acquire();
    if (lock_rc == -1) {
      fprintf(stderr, C_RED "error: " C_RESET
                      "permission denied — run lpm as root (doas/sudo)\n");
      return 1;
    } else if (lock_rc == -2) {
      fprintf(stderr, C_RED "error: " C_RESET
                      "lpm is already running — only one instance at a time\n"
                      "  If stale, remove: " C_CYAN LPM_LOCK_FILE C_RESET "\n");
      return 1;
    }
  }

  if (!strcmp(cmd, "-S"))
    cmd_sync(sub_argc, sub_argv);
  else if (!strcmp(cmd, "-bi"))
    cmd_local(sub_argc, sub_argv);
  else if (!strcmp(cmd, "-c"))
    cmd_check(sub_argc, sub_argv);
  else if (!strcmp(cmd, "-r") || !strcmp(cmd, "-R"))
    cmd_remove(sub_argc, sub_argv);
  else if (!strcmp(cmd, "-rcc"))
    cmd_rcc(sub_argc, sub_argv);
  else if (!strcmp(cmd, "-u") || !strcmp(cmd, "-U"))
    cmd_update(sub_argc, sub_argv);
  else if (!strcmp(cmd, "-s"))
    cmd_search(sub_argc, sub_argv);
  else if (!strcmp(cmd, "-D"))
    cmd_deptree(sub_argc, sub_argv);
  else if (!strcmp(cmd, "-qi"))
    cmd_info(sub_argc, sub_argv);
  else if (!strcmp(cmd, "-l") || !strcmp(cmd, "-Q"))
    cmd_list(sub_argc, sub_argv);
  else if (!strcmp(cmd, "-Qo"))
    cmd_orphans(sub_argc, sub_argv);
  else if (!strcmp(cmd, "-K"))
    cmd_key(sub_argc, sub_argv);
  else if (!strcmp(cmd, "-V") || !strcmp(cmd, "-v"))
    printf("lpm %s\n", LPM_VERSION);
  else if (!strcmp(cmd, "-h") || !strcmp(cmd, "--help"))
    usage();
  else {
    if (cmd[0] == '-')
      fprintf(stderr, C_RED "error: " C_RESET "invalid option '%s'\n", cmd);
    else
      fprintf(stderr, C_RED "error: " C_RESET "unknown operation '%s'\n", cmd);
    if (needs_lock) lpm_lock_release();
    return 1;
  }

  if (needs_lock && !g_interrupted) lpm_lock_release();
  return 0;
}
