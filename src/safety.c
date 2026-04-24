#include "lpm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>

/* ═══════════════════════════════════════════════════════════════════════
 * #5  FILE-LEVEL CONFLICT DETECTION
 *
 * For every file that would be installed by pkgdir, check if it already
 * exists on the real rootfs AND is owned by a *different* package.
 * Whitelisted paths (dirs, symlinks that are expected to be shared) are
 * skipped.  Returns number of conflicts found; 0 = safe to proceed.
 * ═══════════════════════════════════════════════════════════════════════ */

/* Recursively collect all destination paths from a pkgdir into `out`.
 * base     = pkgdir root  (e.g. /var/cache/lpm/gcc/pkg)
 * rel      = current relative path (empty string at top level)
 * out      = caller-allocated array of LPM_PATH_MAX strings
 * maxout   = capacity of out
 * Returns number of paths collected. */
static int collect_files(const char *base, const char *rel,
                         char out[][LPM_PATH_MAX], int maxout) {
    char cur[LPM_PATH_MAX];
    if (rel[0])
        snprintf(cur, sizeof(cur), "%s/%s", base, rel);
    else
        snprintf(cur, sizeof(cur), "%s", base);

    DIR *d = opendir(cur);
    if (!d) return 0;

    int n = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) && n < maxout) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;

        char rel_child[LPM_PATH_MAX];
        if (rel[0])
            snprintf(rel_child, sizeof(rel_child), "%s/%s", rel, ent->d_name);
        else
            snprintf(rel_child, sizeof(rel_child), "%s", ent->d_name);

        char abs[LPM_PATH_MAX];
        snprintf(abs, sizeof(abs), "%s/%s", base, rel_child);

        struct stat st;
        if (lstat(abs, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            n += collect_files(base, rel_child, out + n, maxout - n);
        } else {
            /* destination path on real rootfs */
            snprintf(out[n], LPM_PATH_MAX, "/%s", rel_child);
            n++;
        }
    }
    closedir(d);
    return n;
}

/* Check if `filepath` is owned by any installed package other than
 * `skip_name`.  Returns the owning package name (static buf) or NULL. */
static const char *file_owner(const char *filepath, const char *skip_name) {
    static char owner[LPM_NAME_MAX];

    /* list all packages by scanning LPM_FILES_DIR */
    DIR *d = opendir(LPM_FILES_DIR);
    if (!d) return NULL;

    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        if (skip_name && !strcmp(ent->d_name, skip_name)) continue;

        char listpath[LPM_PATH_MAX];
        snprintf(listpath, sizeof(listpath), "%s/%s/files.list",
                 LPM_FILES_DIR, ent->d_name);

        FILE *fp = fopen(listpath, "r");
        if (!fp) continue;

        char line[LPM_PATH_MAX];
        int found = 0;
        while (fgets(line, sizeof(line), fp)) {
            line[strcspn(line, "\n")] = '\0';
            if (!strcmp(line, filepath)) { found = 1; break; }
        }
        fclose(fp);

        if (found) {
            strncpy(owner, ent->d_name, LPM_NAME_MAX - 1);
            owner[LPM_NAME_MAX - 1] = '\0';
            closedir(d);
            return owner;
        }
    }
    closedir(d);
    return NULL;
}

/* Public API — called before merge.
 * pkgdir   = staged package root (e.g. /var/cache/lpm/btop/pkg)
 * pkgname  = name of package being installed (skip self-ownership)
 * force    = if 1, print warnings but don't abort
 * Returns 0 if safe, -1 if conflicts found (and force == 0). */
int safety_check_file_conflicts(const char *pkgdir, const char *pkgname,
                                int force) {
    /* collect files from pkgdir — heap alloc to avoid huge stack frame */
    int maxf = LPM_MAX_FILES;
    char (*files)[LPM_PATH_MAX] = calloc(maxf, LPM_PATH_MAX);
    if (!files) return -1;

    int nfiles = collect_files(pkgdir, "", files, maxf);
    int conflicts = 0;

    for (int i = 0; i < nfiles; i++) {
        /* skip if file doesn't exist on rootfs yet */
        struct stat st;
        if (lstat(files[i], &st) != 0) continue;

        /* dirs are always shared — skip */
        if (S_ISDIR(st.st_mode)) continue;

        const char *owner = file_owner(files[i], pkgname);
        if (!owner) continue;   /* unowned or owned by self */

        if (force) {
            fprintf(stderr,
                C_YELLOW "warning:" C_RESET
                " %s: owned by %s — will overwrite (--force)\n",
                files[i], owner);
        } else {
            fprintf(stderr,
                C_RED "error:" C_RESET
                " file conflict: %s\n"
                "  owned by: " C_BOLD "%s" C_RESET "\n"
                "  installing: " C_BOLD "%s" C_RESET "\n"
                "  Use --force to override or remove %s first.\n",
                files[i], owner, pkgname, owner);
            conflicts++;
        }
    }

    free(files);
    return conflicts > 0 ? -1 : 0;
}

/* ── named-conflict check (PKGBUILD conflicts= field) ─────────────────── */
int safety_check_conflicts(Package **pkgs, int n, const char *root) {
    (void)root;
    int found = 0;

    for (int i = 0; i < n; i++) {
        Package *p = pkgs[i];

        for (int c = 0; c < p->nconflicts; c++) {
            if (db_is_installed(p->conflicts[c])) {
                fprintf(stderr,
                    C_RED "error:" C_RESET
                    " %s conflicts with installed package %s\n",
                    p->name, p->conflicts[c]);
                found++;
            }
        }

        for (int j = 0; j < n; j++) {
            if (i == j) continue;
            Package *q = pkgs[j];
            for (int c = 0; c < q->nconflicts; c++) {
                if (!strcmp(q->conflicts[c], p->name)) {
                    fprintf(stderr,
                        C_RED "error:" C_RESET
                        " %s and %s conflict\n", p->name, q->name);
                    found++;
                }
            }
        }

        char *inst_ver = db_get_version(p->name);
        if (inst_ver) {
            if (strcmp(inst_ver, p->version) >= 0)
                printf(C_YELLOW "warning:" C_RESET
                       " %s %s already installed (have %s)\n",
                       p->name, p->version, inst_ver);
            free(inst_ver);
        }
    }
    return found > 0 ? -1 : 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 * #18  DISK SPACE CHECK  (real, called before build + merge)
 * ═══════════════════════════════════════════════════════════════════════ */
int safety_check_space(Package **pkgs, int n, const char *root) {
    /* estimate: source pkg ~200MB build space, binary ~20MB */
    long needed_kb = 0;
    for (int i = 0; i < n; i++)
        needed_kb += (pkgs[i]->type == PKG_TYPE_BINARY) ? 20480 : 204800;

    long free_build = util_disk_free(g_cfg.build_dir[0]
                                     ? g_cfg.build_dir : LPM_BUILD_DIR);
    if (free_build >= 0 && free_build < needed_kb) {
        fprintf(stderr,
            C_RED "error:" C_RESET
            " not enough space in %s\n"
            "  need:  ~%ld MB\n"
            "  have:   %ld MB\n",
            g_cfg.build_dir, needed_kb / 1024, free_build / 1024);
        return -1;
    }

    long free_root = util_disk_free(root && root[0] ? root : "/");
    if (free_root >= 0 && free_root < needed_kb / 8) {
        fprintf(stderr,
            C_YELLOW "warning:" C_RESET
            " low disk space on %s: %ld MB free\n",
            root, free_root / 1024);
        /* warn only — don't abort, admin may know what they're doing */
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 * #10  CONFIG FILE PROTECTION
 *
 * For each path listed in pkg->backup[]:
 *   - If file exists on rootfs AND differs from staged version
 *     → keep existing, save new version as <path>.lpm-new
 *   - If file doesn't exist yet → install normally (no conflict)
 *
 * Called from merge.c BEFORE copying files.
 * ═══════════════════════════════════════════════════════════════════════ */
int safety_backup_configs(Package *pkg, const char *root) {
    if (pkg->nbackup == 0) return 0;

    int backed = 0;
    for (int i = 0; i < pkg->nbackup; i++) {
        /* path on real rootfs */
        char live[LPM_PATH_MAX];
        snprintf(live, sizeof(live), "%s/%s", root, pkg->backup[i]);

        struct stat st;
        if (stat(live, &st) != 0) continue;   /* not installed yet, fine */

        /* backup: <path>.lpm-backup.<timestamp> */
        char bak[LPM_PATH_MAX + 32];
        snprintf(bak, sizeof(bak), "%s.lpm-backup.%ld",
                 live, (long)time(NULL));

        if (util_copy_file(live, bak) == 0) {
            printf(C_GRAY "  -> backup: %s\n" C_RESET, pkg->backup[i]);
            backed++;
        } else {
            fprintf(stderr,
                C_YELLOW "warning:" C_RESET
                " could not backup config %s\n", live);
        }
    }

    if (backed > 0)
        printf(C_GRAY "  -> %d config(s) backed up\n" C_RESET, backed);
    return 0;
}

/* Protect configs during merge: if live config differs from new version,
 * install new version as <path>.lpm-new instead of overwriting.
 * pkgdir = staged package root.
 * Returns number of protected files. */
int safety_protect_configs(Package *pkg, const char *pkgdir,
                            const char *root) {
    if (pkg->nbackup == 0) return 0;

    int protected = 0;
    for (int i = 0; i < pkg->nbackup; i++) {
        char live[LPM_PATH_MAX];
        snprintf(live, sizeof(live), "%s/%s", root, pkg->backup[i]);

        char staged[LPM_PATH_MAX];
        snprintf(staged, sizeof(staged), "%s/%s", pkgdir, pkg->backup[i]);

        struct stat live_st, staged_st;
        if (stat(live, &live_st) != 0) continue;    /* doesn't exist yet */
        if (stat(staged, &staged_st) != 0) continue; /* not in package */

        /* compare via checksum — if identical, safe to overwrite */
        char cmd[LPM_PATH_MAX * 2 + 64];
        snprintf(cmd, sizeof(cmd),
            "cmp -s '%s' '%s'", live, staged);
        if (system(cmd) == 0) continue;   /* files identical, no conflict */

        /* files differ — save staged version as .lpm-new */
        char newpath[LPM_PATH_MAX + 8];
        snprintf(newpath, sizeof(newpath), "%s.lpm-new", live);
        if (util_copy_file(staged, newpath) == 0) {
            printf(C_YELLOW "  -> config protected:" C_RESET
                   " %s (new version saved as %s.lpm-new)\n",
                   pkg->backup[i], pkg->backup[i]);
            protected++;

            /* remove staged file so cp -a in merge won't overwrite live */
            unlink(staged);
        }
    }

    if (protected > 0)
        printf(C_CYAN "::" C_RESET
               " %d config(s) preserved — review *.lpm-new files\n",
               protected);
    return protected;
}

/* ── restore backed-up configs (rollback path) ───────────────────────── */
int safety_restore_configs(Package *pkg, const char *root) {
    if (pkg->nbackup == 0) return 0;

    for (int i = 0; i < pkg->nbackup; i++) {
        char live[LPM_PATH_MAX];
        snprintf(live, sizeof(live), "%s/%s", root, pkg->backup[i]);

        char cmd[LPM_PATH_MAX * 2 + 64];
        snprintf(cmd, sizeof(cmd),
            "f=$(ls -t '%s'.lpm-backup.* 2>/dev/null | head -1);"
            " [ -n \"$f\" ] && cp -f \"$f\" '%s'",
            live, live);
        system(cmd);
    }
    return 0;
}

/* ── symlink guard ───────────────────────────────────────────────────── */
int safety_guard_symlinks(const char *src_path, const char *dest_path) {
    struct stat dest_st, src_st;

    if (lstat(dest_path, &dest_st) != 0) return 0;
    if (lstat(src_path,  &src_st)  != 0) return 0;

    /* src is symlink trying to replace real file → BLOCK */
    if (S_ISLNK(src_st.st_mode) && !S_ISLNK(dest_st.st_mode)) {
        fprintf(stderr,
            C_RED "error:" C_RESET
            " refusing to replace real file with symlink: %s\n", dest_path);
        return -1;
    }

    /* dest is absolute symlink → warn if verbose */
    if (S_ISLNK(dest_st.st_mode)) {
        char target[LPM_PATH_MAX];
        ssize_t n = readlink(dest_path, target, sizeof(target) - 1);
        if (n > 0) {
            target[n] = '\0';
            if (target[0] == '/' && g_verbose)
                printf(C_YELLOW "warning:" C_RESET
                       " absolute symlink: %s -> %s\n", dest_path, target);
        }
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 * #6  MINIMAL TOOLCHAIN LOCK
 * ═══════════════════════════════════════════════════════════════════════ */

static const char *TOOLCHAIN_LOCK[] = {
    "/bin/sh", "/bin/bash", "/usr/bin/bash",
    "/usr/bin/tar", "/usr/bin/xz", "/usr/bin/gzip", "/usr/bin/bzip2",
    "/usr/bin/grep", "/usr/bin/sed", "/usr/bin/awk",
    "/usr/bin/find", "/usr/bin/sort", "/usr/bin/cut",
    "/usr/bin/gcc", "/usr/bin/cc", "/usr/bin/g++",
    "/usr/bin/ld",  "/usr/bin/ar", "/usr/bin/as",
    "/usr/bin/make",
    "/lib/ld-linux-x86-64.so.2", "/lib64/ld-linux-x86-64.so.2",
    "/lib/ld-linux.so.2",
    "/usr/lib/libc.so.6", "/lib/libc.so.6",
    "/usr/lib/libm.so.6", "/lib/libm.so.6",
    NULL
};

int safety_check_toolchain(const char *pkgdir, const char *pkgname) {
    int blocked = 0;

    for (int i = 0; TOOLCHAIN_LOCK[i]; i++) {
        const char *dest = TOOLCHAIN_LOCK[i];

        char staged[LPM_PATH_MAX];
        snprintf(staged, sizeof(staged), "%s%s", pkgdir, dest);

        struct stat staged_st;
        if (lstat(staged, &staged_st) != 0) continue;

        struct stat live_st;
        if (lstat(dest, &live_st) != 0) continue;

        /* allow legitimate self-update (gcc updating gcc) */
        char owner[LPM_NAME_MAX] = "";
        db_query_owner(dest, owner, sizeof(owner));
        if (owner[0] && !strcmp(owner, pkgname)) continue;

        /* BLOCK: symlink replacing real binary */
        if (S_ISLNK(staged_st.st_mode) && !S_ISLNK(live_st.st_mode)) {
            fprintf(stderr,
                C_RED "error:" C_RESET
                " toolchain lock: refusing to replace " C_BOLD "%s" C_RESET
                " with a symlink (package: %s)\n"
                "  This would break your shell/compiler.\n",
                dest, pkgname);
            blocked++;
            continue;
        }

        /* BLOCK: staged file suspiciously small (< 10% of live) = stub */
        if (!S_ISLNK(staged_st.st_mode) && !S_ISLNK(live_st.st_mode)) {
            if (live_st.st_size > 4096 &&
                staged_st.st_size < live_st.st_size / 10) {
                fprintf(stderr,
                    C_RED "error:" C_RESET
                    " toolchain lock: " C_BOLD "%s" C_RESET
                    " in %s is suspiciously small (%ld vs %ld bytes live)\n"
                    "  Refusing to overwrite — likely a stub binary.\n",
                    dest, pkgname,
                    (long)staged_st.st_size, (long)live_st.st_size);
                blocked++;
            }
        }
    }

    return blocked > 0 ? -1 : 0;
}

/* ── lock file ───────────────────────────────────────────────────────── */
int g_lock_fd = -1;

/* Return values:
 *   0  — lock acquired OK
 *  -1  — permission denied (not root)
 *  -2  — another instance is running */
int lpm_lock_acquire(void) {
    g_lock_fd = open(LPM_LOCK_FILE, O_CREAT | O_RDWR, 0644);
    if (g_lock_fd < 0) {
        /* EACCES / EPERM → not root, don't pretend it's a lock conflict */
        return -1;
    }

    struct flock fl = {
        .l_type   = F_WRLCK,
        .l_whence = SEEK_SET,
        .l_start  = 0,
        .l_len    = 0,
    };

    if (fcntl(g_lock_fd, F_SETLK, &fl) != 0) {
        close(g_lock_fd);
        g_lock_fd = -1;
        return -2;
    }

    char pidbuf[32];
    snprintf(pidbuf, sizeof(pidbuf), "%d\n", (int)getpid());
    ftruncate(g_lock_fd, 0);
    write(g_lock_fd, pidbuf, strlen(pidbuf));
    return 0;
}

void lpm_lock_release(void) {
    if (g_lock_fd >= 0) {
        struct flock fl = { .l_type = F_UNLCK, .l_whence = SEEK_SET };
        fcntl(g_lock_fd, F_SETLK, &fl);
        close(g_lock_fd);
        g_lock_fd = -1;
        unlink(LPM_LOCK_FILE);
    }
}
