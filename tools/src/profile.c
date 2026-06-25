#include "lpm.h"
#include <dirent.h>

void cmd_profile(int argc, char **argv) {
  if (argc < 1) die("usage: lpm profile <list|set>");
  if (!strcmp(argv[0], "list")) {
    DIR *d = opendir("/etc/lpm/profiles");
    if (!d) die("cannot open /etc/lpm/profiles");
    struct dirent *e;
    while ((e = readdir(d))) {
      if (e->d_name[0] == '.') continue;
      printf("%s\n", e->d_name);
    }
    closedir(d);
    return;
  }
  if (!strcmp(argv[0], "set")) {
    if (argc < 2) die("usage: lpm profile set <name>");
    FILE *in = fopen(LPM_CONF_FILE, "r");
    char outtmp[LPM_PATH_MAX];
    snprintf(outtmp, sizeof(outtmp), "%s.tmp", LPM_CONF_FILE);
    FILE *out = fopen(outtmp, "w");
    if (!out) die("cannot write %s", outtmp);
    int found = 0;
    if (in) {
      char line[1024];
      while (fgets(line, sizeof(line), in)) {
        if (!strncmp(line, "PROFILE", 7)) {
          fprintf(out, "PROFILE = %s\n", argv[1]);
          found = 1;
        } else {
          fputs(line, out);
        }
      }
      fclose(in);
    }
    if (!found) fprintf(out, "\nPROFILE = %s\n", argv[1]);
    fclose(out);
    if (rename(outtmp, LPM_CONF_FILE) != 0) die("failed to update lpm.conf");
    printf("set PROFILE=%s\n", argv[1]);
    return;
  }
  die("unknown profile command: %s", argv[0]);
}

void cmd_doctor(int argc, char **argv) {
  (void)argc; (void)argv;
  LpmConfig cfg;
  lpm_config_load(LPM_CONF_FILE, &cfg);
  printf("[OK] Profile: %s\n", cfg.profile[0] ? cfg.profile : "generic");
  if (strstr(cfg.cflags, "-march=native"))
    printf("[WARN] Using -march=native\n");
  else
    printf("[OK] CFLAGS sane\n");
  if (strstr(cfg.cflags, "-Ofast"))
    printf("[WARN] -Ofast may break packages\n");
  long free_kb = util_disk_free(cfg.build_dir[0] ? cfg.build_dir : LPM_BUILD_DIR);
  if (free_kb < 0) printf("[WARN] Unable to read disk space\n");
  else printf("[OK] Disk space: %ld KB\n", free_kb);
  if (cfg.jobs > 0) printf("[OK] Parallel jobs: %d\n", cfg.jobs);
}
