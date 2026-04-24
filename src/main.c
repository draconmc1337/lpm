#include "lpm.h"
#include <signal.h>

/* ── signal handler: release lock + kill child process group ─────────── */
static void sig_handler(int sig) {
    (void)sig;
    lpm_lock_release();
    killpg(0, SIGTERM);
    _exit(1);
}

static void usage(void) {
  printf(C_BOLD "lpm" C_RESET
                " — Lotus Package Manager " C_CYAN LPM_VERSION C_RESET "\n\n"
                "Usage: lpm <command> [package(s)]\n\n"
                "  -S   <pkg...>   Fetch PKGBUILD from repo + build + install\n"
                "  -bi  <pkg...>   Build + install using local PKGBUILD\n"
                "  -Sy  <pkg...>   Fetch PKGBUILD only\n"
                "  -c   <pkg...>   Run test suite\n"
                "  -r   <pkg...>   Remove  (--force, --no-confirm)\n"
                "  -rcc [pkg...]   Remove build cache\n"
                "  -u   [pkg...]   Update  (all if no args)\n"
                "  -s   <term>     Search packages\n"
                "  -D   <pkg...>   Dependency tree\n"
                "  -qi  <pkg...>   Package info\n"
                "  -l              List installed\n"
                "  -Qo             List orphaned packages\n"
                "  -v              Version\n\n"
                "Config: /etc/lpm/lpm.conf\n"
                "PKGBUILD: %s/pkgbuild_<name>\n"
                "Logs:     %s/<pkg>.log\n",
                LPM_PKGBUILD_DIR, LPM_LOG_DIR);
}

int main(int argc, char **argv) {
  if (argc < 2) { usage(); return 0; }

  const char *cmd = argv[1];
  int   sub_argc  = argc - 2;
  char **sub_argv = argv + 2;

  /* read-only commands — no lock needed */
  int needs_lock = strcmp(cmd, "-s")     != 0 &&
                   strcmp(cmd, "-qi")    != 0 &&
                   strcmp(cmd, "-l")     != 0 &&
                   strcmp(cmd, "-Qo")    != 0 &&
                   strcmp(cmd, "-v")     != 0 &&
                   strcmp(cmd, "-h")     != 0 &&
                   strcmp(cmd, "--help") != 0 &&
                   strcmp(cmd, "-D")     != 0;

  if (needs_lock) {
    /* own process group so killpg() catches wget/curl/bash children */
    setpgid(0, 0);

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
          "  If stale, remove: " C_CYAN LPM_LOCK_FILE C_RESET "\n");
      return 1;
    }
  }

  if      (!strcmp(cmd, "-S"))   cmd_sync(sub_argc, sub_argv);
  else if (!strcmp(cmd, "-bi"))  cmd_local(sub_argc, sub_argv);
  else if (!strcmp(cmd, "-Sy"))  cmd_fetch(sub_argc, sub_argv);
  else if (!strcmp(cmd, "-c"))   cmd_check(sub_argc, sub_argv);
  else if (!strcmp(cmd, "-r"))   cmd_remove(sub_argc, sub_argv);
  else if (!strcmp(cmd, "-rcc")) cmd_rcc(sub_argc, sub_argv);
  else if (!strcmp(cmd, "-u"))   cmd_update(sub_argc, sub_argv);
  else if (!strcmp(cmd, "-s"))   cmd_search(sub_argc, sub_argv);
  else if (!strcmp(cmd, "-D"))   cmd_deptree(sub_argc, sub_argv);
  else if (!strcmp(cmd, "-qi"))  cmd_info(sub_argc, sub_argv);
  else if (!strcmp(cmd, "-l"))   cmd_list(sub_argc, sub_argv);
  else if (!strcmp(cmd, "-Qo"))  cmd_orphans(sub_argc, sub_argv);
  else if (!strcmp(cmd, "-v"))   printf("lpm %s\n", LPM_VERSION);
  else if (!strcmp(cmd, "-h") ||
           !strcmp(cmd, "--help")) usage();
  else {
    fprintf(stderr,
        C_RED "error: " C_RESET "unknown command: %s\n"
        "Run 'lpm -h' for help.\n", cmd);
    if (needs_lock) lpm_lock_release();
    return 1;
  }

  if (needs_lock) lpm_lock_release();
  return 0;
}
