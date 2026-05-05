#include "lpm.h"

#define LPM_KEYRING_DIR "/etc/lpm/gnupg"

static void key_usage(void) {
  printf("usage: lpm -K <subcommand> [args]\n"
         "subcommands:\n"
         "  init                 initialize lpm keyring\n"
         "  list                 list keys in keyring\n"
         "  recv <keyid>         receive key from keyserver\n"
         "  import <file>        import key file\n"
         "  trust <keyid>        set key to ultimate trust\n");
}

void cmd_key(int argc, char **argv) {
  check_root();
  if (argc < 1) {
    key_usage();
    return;
  }

  util_mkdirp("/etc/lpm", 0755);
  util_mkdirp(LPM_KEYRING_DIR, 0700);

  const char *sub = argv[0];
  char cmd[MAX_CMD];

  if (!strcmp(sub, "init")) {
    snprintf(cmd, sizeof(cmd),
             "gpg --homedir '%s' --batch --list-keys >/dev/null 2>&1 || "
             "gpg --homedir '%s' --batch --quick-generate-key 'lpm (local) <root@localhost>' default default never",
             LPM_KEYRING_DIR, LPM_KEYRING_DIR);
    if (util_run(cmd) != 0) die("failed to initialize keyring");
    printf(C_GREEN "==>" C_RESET " lpm keyring initialized at %s\n", LPM_KEYRING_DIR);
  } else if (!strcmp(sub, "list")) {
    snprintf(cmd, sizeof(cmd), "gpg --homedir '%s' --list-keys", LPM_KEYRING_DIR);
    if (util_run(cmd) != 0) die("failed to list keys");
  } else if (!strcmp(sub, "recv")) {
    if (argc < 2) die("usage: lpm -K recv <keyid>");
    snprintf(cmd, sizeof(cmd),
             "gpg --homedir '%s' --keyserver keyserver.ubuntu.com --recv-keys '%s'",
             LPM_KEYRING_DIR, argv[1]);
    if (util_run(cmd) != 0) die("failed to receive key %s", argv[1]);
  } else if (!strcmp(sub, "import")) {
    if (argc < 2) die("usage: lpm -K import <file>");
    snprintf(cmd, sizeof(cmd), "gpg --homedir '%s' --import '%s'", LPM_KEYRING_DIR, argv[1]);
    if (util_run(cmd) != 0) die("failed to import key file %s", argv[1]);
  } else if (!strcmp(sub, "trust")) {
    if (argc < 2) die("usage: lpm -K trust <keyid>");
    snprintf(cmd, sizeof(cmd),
             "printf 'trust\\n5\\ny\\nquit\\n' | gpg --homedir '%s' --command-fd 0 --edit-key '%s'",
             LPM_KEYRING_DIR, argv[1]);
    if (util_run(cmd) != 0) die("failed to trust key %s", argv[1]);
  } else {
    die("unknown key subcommand: %s", sub);
  }
}
