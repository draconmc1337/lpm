#include "lpm.h"

#define LPM_KEYRING_DIR "/etc/lpm/gnupg"
#define LPM_DEVKEY_LEN 15

static int gen_key_id(char *out, size_t outsz) {
  static const char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
  unsigned char rnd[LPM_DEVKEY_LEN];
  FILE *fp;

  if (!out || outsz < (size_t)LPM_DEVKEY_LEN + 1) return -1;
  fp = fopen("/dev/urandom", "rb");
  if (!fp) return -1;
  if (fread(rnd, 1, sizeof(rnd), fp) != sizeof(rnd)) {
    fclose(fp);
    return -1;
  }
  fclose(fp);

  for (size_t i = 0; i < (size_t)LPM_DEVKEY_LEN; i++) {
    out[i] = alphabet[rnd[i] % (sizeof(alphabet) - 1)];
  }
  out[LPM_DEVKEY_LEN] = '\0';
  return 0;
}

static void key_usage(void) {
  printf("usage: lpm key <subcommand> [args]\n"
         "subcommands:\n"
         "  init                 initialize lpm keyring\n"
         "  genid                print random 15-char key id\n"
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
  char kid[LPM_DEVKEY_LEN + 1];

  if (!strcmp(sub, "init")) {
    if (gen_key_id(kid, sizeof(kid)) != 0) die("failed to generate key id");
    /* gpg --list-keys returns 0 even on an empty keyring, so we can't use
     * its exit code to detect "no keys yet".  Instead, check whether any
     * pub lines appear in the output — only skip generation if at least
     * one key already exists. */
    snprintf(cmd, sizeof(cmd),
             "gpg --homedir '%s' --list-keys 2>/dev/null | grep -q '^pub' || "
             "gpg --homedir '%s' --batch --pinentry-mode loopback --passphrase '' "
             "--quick-generate-key 'lpm-%s (lotus) <root@localhost>' default default never",
             LPM_KEYRING_DIR, LPM_KEYRING_DIR, kid);
    if (util_run(cmd) != 0) die("failed to initialize keyring");
    printf("==> lpm keyring initialized at %s\n", LPM_KEYRING_DIR);
    printf("  -> key id seed: %s\n", kid);
    /* print fingerprint so the user can immediately run: lpm key trust <fpr> */
    snprintf(cmd, sizeof(cmd),
             "gpg --homedir '%s' --list-keys --with-colons 2>/dev/null"
             " | awk -F: '/^fpr/{print $10; exit}'",
             LPM_KEYRING_DIR);
    printf("  -> fingerprint: ");
    fflush(stdout);
    util_run(cmd); /* gpg prints to stdout */
  } else if (!strcmp(sub, "genid")) {
    if (gen_key_id(kid, sizeof(kid)) != 0) die("failed to generate key id");
    printf("%s\n", kid);
  } else if (!strcmp(sub, "list")) {
    snprintf(cmd, sizeof(cmd),
             "gpg --homedir '%s' --list-keys --with-colons 2>/dev/null",
             LPM_KEYRING_DIR);
    FILE *p = popen(cmd, "r");
    if (!p) die("failed to list keys");
    /* collect pub keyids + their first uid */
    char ids[128][32]; char uids[128][256]; int n = 0;
    char last_id[32] = "";
    char l[1024];
    while (fgets(l, sizeof(l), p)) {
      l[strcspn(l, "\n")] = '\0';
      char *f1 = strchr(l, ':');
      if (!f1) continue;
      *f1 = '\0';
      if (!strcmp(l, "pub")) {
        /* keyid is colon-field 5 */
        char *tok = strtok(f1 + 1, ":");
        tok = tok ? strtok(NULL, ":") : NULL;
        tok = tok ? strtok(NULL, ":") : NULL;
        tok = tok ? strtok(NULL, ":") : NULL;
        snprintf(last_id, sizeof(last_id), "%s", tok ? tok : "");
      } else if (!strcmp(l, "uid") && last_id[0] && n < 128) {
        /* uid is colon-field 10 */
        char *save = NULL;
        char *flds[12]; int nf = 0;
        for (char *t = strtok_r(f1 + 1, ":", &save); t && nf < 12;
             t = strtok_r(NULL, ":", &save)) flds[nf++] = t;
        const char *uid = (nf >= 10) ? flds[9] : "";
        snprintf(ids[n], 32, "%s", last_id);
        snprintf(uids[n], 256, "%s", uid);
        n++;
        last_id[0] = '\0';
      }
    }
    pclose(p);
    printf("Trusted keys (%d)\n\n", n);
    for (int i = 0; i < n; i++)
      printf("%.8s...  %s\n", ids[i], uids[i]);
  } else if (!strcmp(sub, "recv")) {
    if (argc < 2) die("usage: lpm key recv <keyid>");
    snprintf(cmd, sizeof(cmd),
             "gpg --homedir '%s' --keyserver keyserver.ubuntu.com --recv-keys '%s'"
             " 2>/dev/null",
             LPM_KEYRING_DIR, argv[1]);
    ui_out("Importing key...\n");
    if (util_run(cmd) != 0) die("failed to receive key %s", argv[1]);
    ui_out("Key imported successfully.\n");
  } else if (!strcmp(sub, "import")) {
    if (argc < 2) die("usage: lpm key import <file>");
    if (access(argv[1], R_OK) != 0) die("Package not found: %s", argv[1]);
    ui_out("Importing key...\n");
    snprintf(cmd, sizeof(cmd),
             "gpg --homedir '%s' --import '%s' 2>/dev/null",
             LPM_KEYRING_DIR, argv[1]);
    if (util_run(cmd) != 0) die("Signature verification failed.");
    ui_out("Key imported successfully.\n");
  } else if (!strcmp(sub, "trust")) {
    if (argc < 2) die("usage: lpm key trust <keyid>");
    snprintf(cmd, sizeof(cmd),
             "printf 'trust\\n5\\ny\\nquit\\n' | gpg --homedir '%s'"
             " --command-fd 0 --edit-key '%s' >/dev/null 2>&1",
             LPM_KEYRING_DIR, argv[1]);
    if (util_run(cmd) != 0) die("failed to trust key %s", argv[1]);
    printf("==> Key %s set to ultimate trust\n", argv[1]);
  } else {
    die("unknown key subcommand: %s", sub);
  }
}
