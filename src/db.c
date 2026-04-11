#include "lpm.h"

/* format: pkgname=ver-rel  (one per line) */

int db_is_installed(const char *pkgname) {
    FILE *f = fopen(LPM_DB, "r");
    if (!f) return 0;
    char line[MAX_STR];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = '\0';
        /* match "pkgname" or "pkgname=..." */
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

/* returns malloc'd "ver-rel" string or NULL if not found / no version */
char *db_get_version(const char *pkgname) {
    FILE *f = fopen(LPM_DB, "r");
    if (!f) return NULL;
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
    /* remove old entry first */
    db_remove(pkgname);
    FILE *f = fopen(LPM_DB, "a");
    if (!f) die("cannot write to DB: %s", LPM_DB);
    fprintf(f, "%s=%s-%s\n", pkgname, ver, rel);
    fclose(f);
}

void db_remove(const char *pkgname) {
    FILE *f = fopen(LPM_DB, "r");
    if (!f) return;

    /* write to temp file in same dir as DB to avoid cross-filesystem rename */
    char tmp_path[MAX_STR];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", LPM_DB);
    FILE *tmp = fopen(tmp_path, "w");
    if (!tmp) { fclose(f); return; }

    char line[MAX_STR];
    size_t len = strlen(pkgname);
    while (fgets(line, sizeof(line), f)) {
        char stripped[MAX_STR];
        snprintf(stripped, sizeof(stripped), "%s", line);
        stripped[strcspn(stripped, "\n")] = '\0';
        if (strncmp(stripped, pkgname, len) == 0 &&
            (stripped[len] == '=' || stripped[len] == '\0'))
            continue;   /* skip — this is the entry to remove */
        fputs(line, tmp);
    }
    fclose(f);
    fclose(tmp);
    rename(tmp_path, LPM_DB);
}

/* ── file ownership database ─────────────────────────────────────────────
 *
 * After package() runs into $pkgdir, LPM scans every file under pkgdir and
 * records absolute destination paths in LPM_FILES_DIR/<pkgname>/files.list.
 * Removal then unlinks exactly those files — no PKGBUILD code is executed.
 *
 * Security: symlinks that escape pkgdir are rejected during scan to prevent
 * a malicious PKGBUILD from smuggling "../../../etc/passwd" style paths.
 * Paths that resolve to "/" or "" are also rejected.
 * ────────────────────────────────────────────────────────────────────── */

/* Recursively walk pkgdir, write destination paths to fp.
 * base    = pkgdir root (e.g. /var/cache/lpm/gcc/pkg)
 * rel     = current relative path inside pkgdir (starts empty "")
 * pkgdir  = same as base, kept for symlink escape check
 * Returns number of files written. */
static int scan_pkgdir(const char *base, const char *rel,
                        const char *pkgdir, FILE *fp) {
    char cur[MAX_STR];
    if (rel[0])
        snprintf(cur, sizeof(cur), "%s/%s", base, rel);
    else
        snprintf(cur, sizeof(cur), "%s", base);

    DIR *d = opendir(cur);
    if (!d) return 0;

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;

        char rel_child[MAX_STR];
        if (rel[0])
            snprintf(rel_child, sizeof(rel_child), "%s/%s", rel, ent->d_name);
        else
            snprintf(rel_child, sizeof(rel_child), "%s", ent->d_name);

        char abs_in_pkgdir[MAX_STR];
        snprintf(abs_in_pkgdir, sizeof(abs_in_pkgdir), "%s/%s", base, rel_child);

        /* resolve symlinks — reject anything that escapes pkgdir */
        char resolved[MAX_STR];
        if (realpath(abs_in_pkgdir, resolved)) {
            if (strncmp(resolved, pkgdir, strlen(pkgdir)) != 0) {
                /* symlink escapes pkgdir — skip with warning */
                fprintf(stderr,
                    "\033[1;33mwarning:\033[0m skipping symlink escape: %s -> %s\n",
                    abs_in_pkgdir, resolved);
                continue;
            }
        }

        struct stat st;
        if (lstat(abs_in_pkgdir, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            count += scan_pkgdir(base, rel_child, pkgdir, fp);
        } else {
            /* destination path on real rootfs = "/" + rel_child */
            char dest[MAX_STR];
            snprintf(dest, sizeof(dest), "/%s", rel_child);

            /* safety: reject "/" or empty */
            if (!dest[1]) continue;

            fprintf(fp, "%s\n", dest);
            count++;
        }
    }
    closedir(d);
    return count;
}

/*
 * db_files_save — call after package() + merge, before db_add().
 * Scans pkgdir and writes LPM_FILES_DIR/<pkgname>/files.list.
 */
void db_files_save(const char *pkgname, const char *pkgdir) {
    /* ensure directory exists */
    char dir[MAX_STR];
    snprintf(dir, sizeof(dir), "%s/%s", LPM_FILES_DIR, pkgname);
    mkdir(LPM_FILES_DIR, 0755);
    mkdir(dir, 0755);

    char listpath[MAX_STR];
    snprintf(listpath, sizeof(listpath), "%s/files.list", dir);

    FILE *fp = fopen(listpath, "w");
    if (!fp) {
        fprintf(stderr, "\033[0;31merror:\033[0m cannot write files.list for %s\n",
                pkgname);
        return;
    }

    int n = scan_pkgdir(pkgdir, "", pkgdir, fp);
    fclose(fp);

    fprintf(stdout, "\033[0;36m  ->\033[0m Recorded %d file(s) for %s\n", n, pkgname);
}

/*
 * db_files_remove — unlinks every file owned by pkgname.
 * Does NOT rmdir directories (other packages may share them).
 * Returns number of files removed, or -1 if files.list not found.
 */
int db_files_remove(const char *pkgname) {
    char listpath[MAX_STR];
    snprintf(listpath, sizeof(listpath), "%s/%s/files.list", LPM_FILES_DIR, pkgname);

    FILE *fp = fopen(listpath, "r");
    if (!fp) return -1;   /* no files.list — old-style install */

    int removed = 0;
    char line[MAX_STR];
    while (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\n")] = '\0';
        if (!line[0] || !line[1]) continue;   /* skip empty or bare "/" */

        /* extra safety: must start with '/' and not contain ".." */
        if (line[0] != '/' || strstr(line, "..")) {
            fprintf(stderr,
                "\033[1;33mwarning:\033[0m skipping suspicious path in files.list: %s\n",
                line);
            continue;
        }

        if (unlink(line) == 0) {
            removed++;
        } else if (errno != ENOENT) {
            /* ENOENT = already gone, that's fine; other errors are not */
            fprintf(stderr,
                "\033[1;33mwarning:\033[0m could not remove %s: %s\n",
                line, strerror(errno));
        }
    }
    fclose(fp);

    /* clean up the files.list itself */
    char dir[MAX_STR];
    snprintf(dir, sizeof(dir), "%s/%s", LPM_FILES_DIR, pkgname);
    unlink(listpath);
    rmdir(dir);   /* only succeeds if dir is now empty — safe */

    return removed;
}
