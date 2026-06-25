#include "lpm.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
#pragma GCC diagnostic ignored "-Wstringop-truncation"


/* format: pkgname=ver-rel  (one per line) */

int db_is_installed(const char *pkgname) {
  DBG(3, "db_is_installed(%s)", pkgname);
  FILE *f = fopen(LPM_DB, "r");
  if (!f)
    return 0;
  char line[MAX_STR];
  int found = 0;
  while (fgets(line, sizeof(line), f)) {
    line[strcspn(line, "\n")] = '\0';
    size_t len = strlen(pkgname);
    if (strncmp(line, pkgname, len) == 0 &&
        (line[len] == '=' || line[len] == '\0')) {
      found = 1;
      break;
    }
  }
  fclose(f);
  return found;
}

char *db_get_version(const char *pkgname) {
  DBG(3, "db_get_version(%s)", pkgname);
  FILE *f = fopen(LPM_DB, "r");
  if (!f)
    return NULL;
  char line[MAX_STR];
  char *result = NULL;
  while (fgets(line, sizeof(line), f)) {
    line[strcspn(line, "\n")] = '\0';
    size_t len = strlen(pkgname);
    if (strncmp(line, pkgname, len) == 0 && line[len] == '=') {
      result = strdup(line + len + 1);
      break;
    }
  }
  fclose(f);
  return result;
}

void db_add(const char *pkgname, const char *ver, const char *rel) {
  DBG(3, "db_add(%s, %s-%s)", pkgname, ver, rel);
  db_remove(pkgname);
  FILE *f = fopen(LPM_DB, "a");
  if (!f)
    die("cannot write to DB: %s", LPM_DB);
  fprintf(f, "%s=%s-%s\n", pkgname, ver, rel);
  fflush(f);
  fsync(fileno(f));
  fclose(f);
}

void db_remove(const char *pkgname) {
  DBG(3, "db_remove(%s)", pkgname);
  FILE *f = fopen(LPM_DB, "r");
  if (!f)
    return;

  char tmp_path[MAX_STR];
  snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", LPM_DB);
  FILE *tmp = fopen(tmp_path, "w");
  if (!tmp) {
    fclose(f);
    return;
  }

  char line[MAX_STR];
  size_t len = strlen(pkgname);
  while (fgets(line, sizeof(line), f)) {
    char stripped[MAX_STR];
    snprintf(stripped, sizeof(stripped), "%s", line);
    stripped[strcspn(stripped, "\n")] = '\0';
    if (strncmp(stripped, pkgname, len) == 0 &&
        (stripped[len] == '=' || stripped[len] == '\0'))
      continue;
    fputs(line, tmp);
  }
  fclose(f);
  fflush(tmp);
  fsync(fileno(tmp));
  fclose(tmp);
  rename(tmp_path, LPM_DB);
}

static int scan_pkgdir(const char *base, const char *rel, const char *pkgdir,
                       FILE *fp, FILE *fpmeta) {
  char cur[MAX_STR];
  if (rel[0])
    snprintf(cur, sizeof(cur), "%s/%s", base, rel);
  else
    snprintf(cur, sizeof(cur), "%s", base);

  DIR *d = opendir(cur);
  if (!d)
    return 0;

  int count = 0;
  struct dirent *ent;
  while ((ent = readdir(d))) {
    if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
      continue;

    char rel_child[MAX_STR];
    if (rel[0])
      snprintf(rel_child, sizeof(rel_child), "%s/%s", rel, ent->d_name);
    else
      snprintf(rel_child, sizeof(rel_child), "%s", ent->d_name);

    char abs_in_pkgdir[MAX_STR + MAX_STR];
    snprintf(abs_in_pkgdir, sizeof(abs_in_pkgdir), "%s/%s", base, rel_child);

    char resolved[MAX_STR];
    if (realpath(abs_in_pkgdir, resolved)) {
      if (strncmp(resolved, pkgdir, strlen(pkgdir)) != 0) {
        fprintf(stderr,
                "\033[1;33mwarning:\033[0m skipping symlink escape: %s -> %s\n",
                abs_in_pkgdir, resolved);
        continue;
      }
    }

    struct stat st;
    if (lstat(abs_in_pkgdir, &st) != 0)
      continue;

    if (S_ISDIR(st.st_mode)) {
      count += scan_pkgdir(base, rel_child, pkgdir, fp, fpmeta);
    } else {
      char dest[MAX_STR];
      snprintf(dest, sizeof(dest), "/%s", rel_child);
      if (!dest[1])
        continue;
      fprintf(fp, "%s\n", dest);

      /* ── files.meta: type, mode, uid, gid, sha256 ──────────────────── *
       * Used by `lpm verify` (levels 1-3: existence, checksum,           *
       * permission/ownership). sha256 only computed for regular files;  *
       * symlinks/devices/etc. record "-" and skip checksum/perm checks. */
      if (fpmeta) {
        char type = 'o';
        char hexsum[65] = "-";
        if (S_ISREG(st.st_mode)) {
          type = 'f';
          if (sha256_file(abs_in_pkgdir, hexsum) != 0)
            strcpy(hexsum, "-");
        } else if (S_ISLNK(st.st_mode)) {
          type = 'l';
        }
        fprintf(fpmeta, "%s\t%c\t%04o\t%d\t%d\t%s\n",
                dest, type, (unsigned)(st.st_mode & 07777),
                (int)st.st_uid, (int)st.st_gid, hexsum);
      }

      count++;
    }
  }
  closedir(d);
  return count;
}

void db_files_save(const char *pkgname, const char *pkgdir) {
  char dir[MAX_STR];
  snprintf(dir, sizeof(dir), "%s/%s", LPM_FILES_DIR, pkgname);
  mkdir(LPM_FILES_DIR, 0755);
  mkdir(dir, 0755);

  char listpath[MAX_STR + 64];
  snprintf(listpath, sizeof(listpath), "%s/files.list", dir);

  char metapath[MAX_STR + 64];
  snprintf(metapath, sizeof(metapath), "%s/files.meta", dir);

  FILE *fp = fopen(listpath, "w");
  if (!fp) {
    fprintf(stderr, "\033[0;31merror:\033[0m cannot write files.list for %s\n",
            pkgname);
    return;
  }

  FILE *fpmeta = fopen(metapath, "w");
  if (!fpmeta)
    fprintf(stderr, "\033[1;33mwarning:\033[0m cannot write files.meta for %s "
                     "— `lpm verify` checksum/permission checks unavailable\n",
            pkgname);

  int n = scan_pkgdir(pkgdir, "", pkgdir, fp, fpmeta);
  fclose(fp);
  if (fpmeta) fclose(fpmeta);
  fprintf(stdout, "\033[0;36m  ->\033[0m Recorded %d file(s) for %s\n", n,
          pkgname);
}

int db_files_remove(const char *pkgname) {
  char listpath[MAX_STR + 64];
  snprintf(listpath, sizeof(listpath), "%s/%s/files.list", LPM_FILES_DIR,
           pkgname);

  FILE *fp = fopen(listpath, "r");
  if (!fp)
    return -1;

  int removed = 0;
  char line[MAX_STR];
  while (fgets(line, sizeof(line), fp)) {
    line[strcspn(line, "\n")] = '\0';
    if (!line[0] || !line[1])
      continue;
    if (line[0] != '/' || strstr(line, "..")) {
      fprintf(stderr,
              "\033[1;33mwarning:\033[0m skipping suspicious path in "
              "files.list: %s\n",
              line);
      continue;
    }
    if (unlink(line) == 0) {
      removed++;
    } else if (errno != ENOENT) {
      fprintf(stderr, "\033[1;33mwarning:\033[0m could not remove %s: %s\n",
              line, strerror(errno));
    }
  }
  fclose(fp);

  char dir[MAX_STR];
  snprintf(dir, sizeof(dir), "%s/%s", LPM_FILES_DIR, pkgname);
  unlink(listpath);
  char metapath[MAX_STR + 64];
  snprintf(metapath, sizeof(metapath), "%s/files.meta", dir);
  unlink(metapath);
  rmdir(dir);
  return removed;
}

/* ── db_init ─────────────────────────────────────────────────────────── */
int db_init(void) {
  util_mkdirp(LPM_DB_DIR, 0755);
  util_mkdirp(LPM_FILES_DIR, 0755);
  /* create empty DB file if missing */
  FILE *f = fopen(LPM_DB, "a");
  if (f)
    fclose(f);
  return 0;
}

/* ── db_record_install ───────────────────────────────────────────────── *
 * Records name/version/release in the main DB. File manifest recording
 * (files.list/files.meta) is done once by pkg_merge() -> db_files_save(),
 * which must run before this so it can read files.list back for the
 * rollback journal — do not duplicate that call here.                   */
int db_record_install(const Package *pkg, const char *root) {
  (void)root; /* rootfs prefix — reserved for future use */

  /* reuse db_add which already handles dedup */
  db_add(pkg->name, pkg->version, pkg->release);

  return 0;
}

/* ── db_query ────────────────────────────────────────────────────────── */
int db_query(const char *name, InstalledPkg *out) {
  if (!db_is_installed(name))
    return -1;

  memset(out, 0, sizeof(*out));
  strncpy(out->name, name, LPM_NAME_MAX - 1);

  char *ver = db_get_version(name);
  if (ver) {
    /* ver is "version-release" */
    char *dash = strrchr(ver, '-');
    if (dash) {
      *dash = '\0';
      strncpy(out->version, ver, LPM_VER_MAX - 1);
      strncpy(out->release, dash + 1, 15);
    } else {
      strncpy(out->version, ver, LPM_VER_MAX - 1);
    }
    free(ver);
  }

  /* files[][] removed from InstalledPkg — use db_list_files() /
   * db_files_remove() / db_check_integrity() to access files.list. */
  return 0;
}

/* ── db_list_all ─────────────────────────────────────────────────────── */
int db_list_all(InstalledPkg **out, int *count) {
  *out = NULL;
  *count = 0;

  FILE *f = fopen(LPM_DB, "r");
  if (!f)
    return 0;

  /* first pass: count entries */
  int n = 0;
  char line[MAX_STR];
  while (fgets(line, sizeof(line), f)) {
    line[strcspn(line, "\n")] = '\0';
    if (line[0])
      n++;
  }
  rewind(f);

  if (n == 0) {
    fclose(f);
    return 0;
  }

  InstalledPkg *arr = calloc(n, sizeof(InstalledPkg));
  if (!arr) {
    fclose(f);
    return -1;
  }

  int i = 0;
  while (fgets(line, sizeof(line), f) && i < n) {
    line[strcspn(line, "\n")] = '\0';
    if (!line[0])
      continue;
    char *eq = strchr(line, '=');
    if (eq)
      *eq = '\0';
    db_query(line, &arr[i++]);
  }
  fclose(f);

  *out = arr;
  *count = i;
  return 0;
}

/* ── db_query_owner ──────────────────────────────────────────────────── */
int db_query_owner(const char *filepath, char *out_name, size_t sz) {
  /* list all packages, check each files.list */
  InstalledPkg *pkgs = NULL;
  int n = 0;
  if (db_list_all(&pkgs, &n) != 0)
    return -1;

  int found = -1;
  for (int i = 0; i < n && found < 0; i++) {
    char listpath[MAX_STR + 64];
    snprintf(listpath, sizeof(listpath), "%s/%s/files.list", LPM_FILES_DIR,
             pkgs[i].name);
    FILE *fp = fopen(listpath, "r");
    if (!fp)
      continue;
    char line[MAX_STR];
    while (fgets(line, sizeof(line), fp)) {
      line[strcspn(line, "\n")] = '\0';
      if (!strcmp(line, filepath)) {
        strncpy(out_name, pkgs[i].name, sz - 1);
        out_name[sz - 1] = '\0';
        found = 0;
        break;
      }
    }
    fclose(fp);
  }
  free(pkgs);
  return found;
}

/* ── db_list_files ───────────────────────────────────────────────────── */
int db_list_files(const char *name) {
  char listpath[MAX_STR + 64];
  snprintf(listpath, sizeof(listpath), "%s/%s/files.list", LPM_FILES_DIR, name);
  FILE *fp = fopen(listpath, "r");
  if (!fp) {
    fprintf(stderr, "\033[1;31merror:\033[0m no file list for '%s'\n", name);
    return -1;
  }
  char line[MAX_STR];
  int n = 0;
  while (fgets(line, sizeof(line), fp)) {
    line[strcspn(line, "\n")] = '\0';
    if (line[0]) {
      puts(line);
      n++;
    }
  }
  fclose(fp);
  return n;
}

/* ── db_check_integrity ──────────────────────────────────────────────── */
int db_check_integrity(const char *name) {
  char listpath[MAX_STR + 64];
  snprintf(listpath, sizeof(listpath), "%s/%s/files.list", LPM_FILES_DIR, name);
  FILE *fp = fopen(listpath, "r");
  if (!fp) {
    fprintf(stderr, "\033[1;31merror:\033[0m no file list for '%s'\n", name);
    return -1;
  }
  char line[MAX_STR];
  int missing = 0;
  while (fgets(line, sizeof(line), fp)) {
    line[strcspn(line, "\n")] = '\0';
    if (!line[0])
      continue;
    struct stat st;
    if (stat(line, &st) != 0) {
      fprintf(stderr, "\033[1;31mmissing:\033[0m %s\n", line);
      missing++;
    }
  }
  fclose(fp);
  if (missing == 0)
    printf("\033[1;32mok:\033[0m all files present for %s\n", name);
  return missing;
}

#pragma GCC diagnostic pop
