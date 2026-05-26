/*
 * lpkg.c — Binary package format (.lpkg) for lpm
 *
 * Commands:
 *   lpm -Pb <pkg>          Build package, then pack into <pkg>-<ver>-<rel>-<arch>.lpkg
 *   lpm -Pi <pkg|path>     Install from a .lpkg file (by path or by name scan)
 *   lpm -Pq [pkg|path]     Query / show info about a .lpkg or installed pkg
 *   lpm -Pe <pkg.lpkg>     Extract .lpkg contents into current directory (inspect)
 *   lpm -Pv <pkg.lpkg>     Verify .lpkg signature + checksum without installing
 *   lpm -Pr <pkg.lpkg>     Remove a built .lpkg file from the cache
 *
 * Format: .lpkg is a zstd-compressed tar archive with the layout:
 *   .lpkg/
 *     meta              — key=value metadata (name, ver, rel, arch, desc, ...)
 *     sha256            — SHA-256 of the inner data.tar.zst
 *     data.tar.zst      — the actual installed files (rooted at /)
 *
 * The archive is created with bsdtar (libarchive) or GNU tar + zstd.
 * Both tools are checked at runtime; the first one available is used.
 */

#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE 1
#include "lpm.h"
#include <ctype.h>
#include <time.h>

/* ── constants ────────────────────────────────────────────────────────── */
#define LPKG_CACHE_DIR   "/var/cache/lpm/packages"
#define LPKG_EXT         ".lpkg"
#define LPKG_ARCH        "amd64"      /* override at compile-time if needed */
#define LPKG_META_FILE   ".lpkg/meta"
#define LPKG_DATA_FILE   ".lpkg/data.tar.zst"
#define LPKG_CKSUM_FILE  ".lpkg/sha256"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
#pragma GCC diagnostic ignored "-Wunused-result"

/* ── internal helpers ─────────────────────────────────────────────────── */

/* Check whether a tool is available in $PATH */
static int have_tool(const char *name) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "command -v '%s' >/dev/null 2>&1", name);
    return system(cmd) == 0;
}

/* Build the canonical .lpkg filename for a package */
static void lpkg_filename(const char *name, const char *ver, const char *rel,
                          char *out, size_t outsz) {
    snprintf(out, outsz, "%s-%s-%s-%s%s", name, ver, rel, LPKG_ARCH, LPKG_EXT);
}

/* Build the full path to the cached .lpkg for a package */
static void lpkg_cache_path(const char *name, const char *ver, const char *rel,
                             char *out, size_t outsz) {
    char fname[LPM_NAME_MAX + LPM_VER_MAX + 32];
    lpkg_filename(name, ver, rel, fname, sizeof(fname));
    snprintf(out, outsz, "%s/%s", LPKG_CACHE_DIR, fname);
}

/*
 * lpkg_find — locate a .lpkg file by bare name (no path, no extension).
 * Scans LPKG_CACHE_DIR for any file matching "<name>-*<arch>.lpkg".
 * On match writes the full path into out[outsz] and returns 0.
 * Returns -1 if nothing found.
 */
static int lpkg_find(const char *name, char *out, size_t outsz) {
    DIR *d = opendir(LPKG_CACHE_DIR);
    if (!d) return -1;

    struct dirent *e;
    size_t nlen = strlen(name);
    int found = 0;

    while ((e = readdir(d)) && !found) {
        if (e->d_name[0] == '.') continue;
        /* must start with "<name>-" and end with LPKG_EXT */
        if (strncmp(e->d_name, name, nlen) == 0 && e->d_name[nlen] == '-') {
            size_t elen = strlen(e->d_name);
            size_t xlen = strlen(LPKG_EXT);
            if (elen > xlen &&
                strcmp(e->d_name + elen - xlen, LPKG_EXT) == 0) {
                snprintf(out, outsz, "%s/%s", LPKG_CACHE_DIR, e->d_name);
                found = 1;
            }
        }
    }
    closedir(d);
    return found ? 0 : -1;
}

/*
 * lpkg_read_meta — read the meta file from an already-extracted .lpkg
 * directory at <workdir>/.lpkg/meta into key=value pairs.
 * Returns 0 on success, -1 if the file cannot be opened.
 */
typedef struct {
    char name[LPM_NAME_MAX];
    char ver[LPM_VER_MAX];
    char rel[16];
    char arch[32];
    char desc[512];
    char license[128];
    char builddate[32];
    char sha256[129];
} LpkgMeta;

static int lpkg_read_meta(const char *workdir, LpkgMeta *m) {
    memset(m, 0, sizeof(*m));

    char path[LPM_PATH_MAX];
    snprintf(path, sizeof(path), "%s/" LPKG_META_FILE, workdir);

    FILE *f = fopen(path, "r");
    if (!f) return -1;

    char line[640];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = '\0';
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *k = line, *v = eq + 1;
        if      (!strcmp(k, "name"))      snprintf(m->name,      sizeof(m->name),      "%s", v);
        else if (!strcmp(k, "version"))   snprintf(m->ver,       sizeof(m->ver),       "%s", v);
        else if (!strcmp(k, "release"))   snprintf(m->rel,       sizeof(m->rel),       "%s", v);
        else if (!strcmp(k, "arch"))      snprintf(m->arch,      sizeof(m->arch),      "%s", v);
        else if (!strcmp(k, "desc"))      snprintf(m->desc,      sizeof(m->desc),      "%s", v);
        else if (!strcmp(k, "license"))   snprintf(m->license,   sizeof(m->license),   "%s", v);
        else if (!strcmp(k, "builddate")) snprintf(m->builddate, sizeof(m->builddate), "%s", v);
        else if (!strcmp(k, "sha256"))    snprintf(m->sha256,    sizeof(m->sha256),    "%s", v);
    }
    fclose(f);
    return m->name[0] ? 0 : -1;
}

/*
 * lpkg_write_meta — write the meta file into <outdir>/.lpkg/meta.
 */
static int lpkg_write_meta(const char *outdir, const LpkgMeta *m) {
    char metadir[LPM_PATH_MAX];
    snprintf(metadir, sizeof(metadir), "%s/.lpkg", outdir);
    util_mkdirp(metadir, 0755);

    char path[LPM_PATH_MAX];
    snprintf(path, sizeof(path), "%s/meta", metadir);

    FILE *f = fopen(path, "w");
    if (!f) return -1;

    fprintf(f, "name=%s\n",      m->name);
    fprintf(f, "version=%s\n",   m->ver);
    fprintf(f, "release=%s\n",   m->rel);
    fprintf(f, "arch=%s\n",      m->arch);
    fprintf(f, "desc=%s\n",      m->desc);
    fprintf(f, "license=%s\n",   m->license);
    fprintf(f, "builddate=%s\n", m->builddate);
    fclose(f);
    return 0;
}

/* ── cmd_pack (-Pb) ───────────────────────────────────────────────────── *
 * 1. Ensure the package has been built (pkgdir must exist).               *
 * 2. Pack it into <name>-<ver>-<rel>-<arch>.lpkg under LPKG_CACHE_DIR.   */
void cmd_pack(int argc, char **argv) {
    check_root();
    init_dirs();

    if (argc == 0) {
        fprintf(stderr,
            C_RED "error:" C_RESET " no package specified\n"
            "usage: lpm -Pb <package>\n");
        exit(1);
    }

    /* Check for required packing tools */
    int have_bsdtar = have_tool("bsdtar");
    int have_zstd   = have_tool("zstd");
    int have_tar    = have_tool("tar");

    if (!have_bsdtar && (!have_tar || !have_zstd)) {
        fprintf(stderr,
            C_RED "error:" C_RESET
            " packing requires bsdtar, or tar + zstd\n"
            "  Install: lpm -S libarchive  (for bsdtar)\n"
            "       or: lpm -S zstd\n");
        exit(1);
    }

    util_mkdirp(LPKG_CACHE_DIR, 0755);

    for (int i = 0; i < argc; i++) {
        const char *pkgname = argv[i];

        /* ── locate pkgdir ───────────────────────────────────────────── */
        char pbfile[LPM_PATH_MAX + LPM_NAME_MAX + 16];
        snprintf(pbfile, sizeof(pbfile), "%s/pkgbuild_%s",
                 LPM_PKGBUILD_DIR, pkgname);

        struct stat pbs;
        if (stat(pbfile, &pbs) != 0) {
            fprintf(stderr,
                C_RED "error:" C_RESET " target not found: %s\n"
                "  (no pkgbuild_%s in %s)\n",
                pkgname, pkgname, LPM_PKGBUILD_DIR);
            continue;
        }

        Pkg pkg;
        if (pkgbuild_parse(pbfile, &pkg) != 0) {
            fprintf(stderr, C_RED "error:" C_RESET
                    " failed to parse PKGBUILD for %s\n", pkgname);
            continue;
        }

        /* pkgdir is where package() staged the files */
        char pkgdir[MAX_STR];
        snprintf(pkgdir, sizeof(pkgdir), "%s/%s/pkg/%s",
                 LPM_BUILD_DIR, pkg.pkgname, pkg.pkgname);

        struct stat pkgdir_st;
        if (stat(pkgdir, &pkgdir_st) != 0) {
            fprintf(stderr,
                C_RED "error:" C_RESET
                " pkgdir not found for %s — build it first with 'lpm -S %s'\n"
                "  expected: %s\n",
                pkgname, pkgname, pkgdir);
            continue;
        }

        /* ── build output path ───────────────────────────────────────── */
        char outpath[LPM_PATH_MAX];
        lpkg_cache_path(pkg.pkgname, pkg.pkgver, pkg.pkgrel,
                        outpath, sizeof(outpath));

        printf(C_CYAN "::" C_RESET " Packing " C_BOLD "%s-%s-%s" C_RESET
               " → " C_CYAN "%s" C_RESET "\n",
               pkg.pkgname, pkg.pkgver, pkg.pkgrel, outpath);

        /* ── create staging area ─────────────────────────────────────── */
        char stagedir[MAX_STR];
        snprintf(stagedir, sizeof(stagedir),
                 "/tmp/lpm_lpkg_stage_%s_%d", pkg.pkgname, (int)getpid());
        util_rmrf(stagedir);
        util_mkdirp(stagedir, 0755);

        /* ── write meta ──────────────────────────────────────────────── */
        LpkgMeta m;
        memset(&m, 0, sizeof(m));
        snprintf(m.name,    sizeof(m.name),    "%s", pkg.pkgname);
        snprintf(m.ver,     sizeof(m.ver),     "%s", pkg.pkgver);
        snprintf(m.rel,     sizeof(m.rel),     "%s", pkg.pkgrel);
        snprintf(m.arch,    sizeof(m.arch),    "%s", LPKG_ARCH);
        snprintf(m.license, sizeof(m.license), "unknown");

        /* try to get description from PkgMeta cache */
        PkgMeta pm; memset(&pm, 0, sizeof(pm));
        if (pkgbuild_parse_fast(pbfile, &pm) == 0 && pm.description[0])
            snprintf(m.desc, sizeof(m.desc), "%s", pm.description);

        time_t now = time(NULL);
        struct tm *tm = gmtime(&now);
        strftime(m.builddate, sizeof(m.builddate), "%Y-%m-%dT%H:%M:%SZ", tm);

        if (lpkg_write_meta(stagedir, &m) != 0) {
            fprintf(stderr, C_RED "error:" C_RESET
                    " cannot write meta for %s\n", pkgname);
            util_rmrf(stagedir);
            continue;
        }

        /* ── pack data.tar.zst from pkgdir ───────────────────────────── */
        char datatmp[MAX_STR];
        snprintf(datatmp, sizeof(datatmp), "%s/.lpkg/data.tar.zst", stagedir);

        char pack_cmd[MAX_CMD];
        if (have_bsdtar) {
            snprintf(pack_cmd, sizeof(pack_cmd),
                "bsdtar -C '%s' -czf '%s' --zstd .",
                pkgdir, datatmp);
        } else {
            snprintf(pack_cmd, sizeof(pack_cmd),
                "tar -C '%s' -c . | zstd -T0 -q -o '%s'",
                pkgdir, datatmp);
        }

        printf(C_GRAY "  packing files..." C_RESET "\n");
        if (system(pack_cmd) != 0) {
            fprintf(stderr, C_RED "error:" C_RESET
                    " failed to pack data for %s\n", pkgname);
            util_rmrf(stagedir);
            continue;
        }

        /* ── compute sha256 of data.tar.zst ─────────────────────────── */
        {
            char sha_cmd[MAX_STR];
            snprintf(sha_cmd, sizeof(sha_cmd),
                "sha256sum '%s' | cut -d' ' -f1", datatmp);
            FILE *p = popen(sha_cmd, "r");
            char sha[129] = "";
            if (p) {
                if (fgets(sha, sizeof(sha), p))
                    sha[strcspn(sha, "\n")] = '\0';
                pclose(p);
            }
            /* write sha256 file alongside meta */
            char shapath[MAX_STR];
            snprintf(shapath, sizeof(shapath), "%s/.lpkg/sha256", stagedir);
            FILE *sf = fopen(shapath, "w");
            if (sf) { fprintf(sf, "%s\n", sha); fclose(sf); }
        }

        /* ── wrap into the final .lpkg (tar of the staging area) ─────── */
        char final_cmd[MAX_CMD];
        if (have_bsdtar) {
            snprintf(final_cmd, sizeof(final_cmd),
                "bsdtar -C '%s' -czf '%s' .",
                stagedir, outpath);
        } else {
            snprintf(final_cmd, sizeof(final_cmd),
                "tar -C '%s' -czf '%s' .",
                stagedir, outpath);
        }

        if (system(final_cmd) != 0) {
            fprintf(stderr, C_RED "error:" C_RESET
                    " failed to create .lpkg archive\n");
            util_rmrf(stagedir);
            unlink(outpath);
            continue;
        }

        util_rmrf(stagedir);

        /* print size */
        struct stat out_st;
        if (stat(outpath, &out_st) == 0) {
            double sz_mb = (double)out_st.st_size / (1024.0 * 1024.0);
            printf(C_GREEN "==> Packed:" C_RESET " %s " C_GRAY "(%.2f MiB)" C_RESET "\n",
                   outpath, sz_mb);
        } else {
            printf(C_GREEN "==> Packed:" C_RESET " %s\n", outpath);
        }

        lpm_log("Packed %s-%s-%s → %s", pkg.pkgname, pkg.pkgver, pkg.pkgrel,
                outpath);
    }
}

/* ── cmd_pkginstall (-Pi) ─────────────────────────────────────────────── *
 * Install a .lpkg file.  Argument can be:                                 *
 *   - absolute/relative path: /path/to/foo-1.0-1-amd64.lpkg              *
 *   - bare package name:      foo   (scans LPKG_CACHE_DIR automatically)  */
void cmd_pkginstall(int argc, char **argv) {
    check_root();
    init_dirs();

    if (argc == 0) {
        fprintf(stderr,
            C_RED "error:" C_RESET " no package specified\n"
            "usage: lpm -Pi <package.lpkg | package-name>\n");
        exit(1);
    }

    int have_bsdtar = have_tool("bsdtar");
    int have_zstd   = have_tool("zstd");
    int have_tar    = have_tool("tar");

    if (!have_bsdtar && (!have_tar || !have_zstd)) {
        fprintf(stderr,
            C_RED "error:" C_RESET
            " extraction requires bsdtar, or tar + zstd\n");
        exit(1);
    }

    for (int i = 0; i < argc; i++) {
        char lpkg_path[LPM_PATH_MAX];

        /* ── resolve path ────────────────────────────────────────────── */
        if (argv[i][0] == '/' || strstr(argv[i], ".lpkg")) {
            /* explicit path */
            snprintf(lpkg_path, sizeof(lpkg_path), "%s", argv[i]);
        } else {
            /* bare name — scan cache */
            if (lpkg_find(argv[i], lpkg_path, sizeof(lpkg_path)) != 0) {
                fprintf(stderr,
                    C_RED "error:" C_RESET " target not found: %s\n"
                    "  (no matching .lpkg in " LPKG_CACHE_DIR ")\n",
                    argv[i]);
                continue;
            }
            printf(C_CYAN "::" C_RESET " Found: " C_BOLD "%s" C_RESET "\n",
                   lpkg_path);
        }

        struct stat lp_st;
        if (stat(lpkg_path, &lp_st) != 0) {
            fprintf(stderr,
                C_RED "error:" C_RESET " file not found: %s\n", lpkg_path);
            continue;
        }

        /* ── extract to temp dir ─────────────────────────────────────── */
        char workdir[MAX_STR];
        snprintf(workdir, sizeof(workdir),
                 "/tmp/lpm_lpkg_inst_%d", (int)getpid());
        util_rmrf(workdir);
        util_mkdirp(workdir, 0755);

        char ex_cmd[MAX_CMD];
        if (have_bsdtar) {
            snprintf(ex_cmd, sizeof(ex_cmd),
                "bsdtar -C '%s' -xf '%s'", workdir, lpkg_path);
        } else {
            snprintf(ex_cmd, sizeof(ex_cmd),
                "tar -C '%s' -xzf '%s'", workdir, lpkg_path);
        }

        if (system(ex_cmd) != 0) {
            fprintf(stderr, C_RED "error:" C_RESET
                    " failed to extract %s\n", lpkg_path);
            util_rmrf(workdir);
            continue;
        }

        /* ── read meta ───────────────────────────────────────────────── */
        LpkgMeta m;
        if (lpkg_read_meta(workdir, &m) != 0) {
            fprintf(stderr, C_RED "error:" C_RESET
                    " invalid .lpkg (missing/corrupt meta): %s\n", lpkg_path);
            util_rmrf(workdir);
            continue;
        }

        /* ── verify sha256 ───────────────────────────────────────────── */
        {
            char shafile[MAX_STR];
            snprintf(shafile, sizeof(shafile), "%s/.lpkg/sha256", workdir);
            FILE *sf = fopen(shafile, "r");
            char expected[129] = "";
            if (sf) {
                if (fgets(expected, sizeof(expected), sf))
                    expected[strcspn(expected, "\n")] = '\0';
                fclose(sf);
            }

            if (expected[0]) {
                char datafile[MAX_STR];
                snprintf(datafile, sizeof(datafile), "%s/.lpkg/data.tar.zst",
                         workdir);
                char sha_cmd[MAX_STR];
                snprintf(sha_cmd, sizeof(sha_cmd),
                    "sha256sum '%s' | cut -d' ' -f1", datafile);
                FILE *p = popen(sha_cmd, "r");
                char actual[129] = "";
                if (p) {
                    if (fgets(actual, sizeof(actual), p))
                        actual[strcspn(actual, "\n")] = '\0';
                    pclose(p);
                }
                if (strcmp(actual, expected) != 0) {
                    fprintf(stderr,
                        C_RED "error:" C_RESET
                        " sha256 mismatch — package may be corrupt\n"
                        "  expected: %s\n"
                        "  got:      %s\n",
                        expected, actual);
                    util_rmrf(workdir);
                    continue;
                }
                printf(C_GREEN "  ok" C_RESET " [sha256] integrity verified\n");
            }
        }

        printf(C_CYAN "::" C_RESET " Installing " C_BOLD "%s-%s-%s" C_RESET
               " from binary package\n",
               m.name, m.ver, m.rel);

        /* ── check conflicts ─────────────────────────────────────────── */
        if (db_is_installed(m.name)) {
            char *iv = db_get_version(m.name);
            printf(C_YELLOW "warning:" C_RESET " %s is already installed (%s)"
                   " — reinstalling\n", m.name, iv ? iv : "?");
            free(iv);
        }

        /* ── extract data.tar.zst into / ─────────────────────────────── */
        char datafile[MAX_STR];
        snprintf(datafile, sizeof(datafile), "%s/.lpkg/data.tar.zst", workdir);

        char inst_cmd[MAX_CMD];
        if (have_bsdtar) {
            snprintf(inst_cmd, sizeof(inst_cmd),
                "bsdtar -C / -xf '%s'", datafile);
        } else {
            snprintf(inst_cmd, sizeof(inst_cmd),
                "zstd -d -c '%s' | tar -C / -x", datafile);
        }

        printf(C_GRAY "  installing files..." C_RESET "\n");
        if (system(inst_cmd) != 0) {
            fprintf(stderr, C_RED "error:" C_RESET
                    " installation failed for %s\n", m.name);
            util_rmrf(workdir);
            continue;
        }

        /* ── record in db ────────────────────────────────────────────── */
        db_add(m.name, m.ver, m.rel);
        db_files_save(m.name, workdir);

        util_rmrf(workdir);

        printf(C_GREEN "==> Installed %s %s-%s" C_RESET
               " from binary package\n", m.name, m.ver, m.rel);
        lpm_audit("lpkg-install: %s %s-%s from %s",
                  m.name, m.ver, m.rel, lpkg_path);
        lpm_log("Installed (binary) %s %s-%s", m.name, m.ver, m.rel);
    }
}

/* ── cmd_pkglist (-Pq) ────────────────────────────────────────────────── *
 * With no arguments: list all .lpkg files in cache.                       *
 * With argument: show detailed info about that .lpkg (by path or name).   */
void cmd_pkglist(int argc, char **argv) {
    if (argc == 0) {
        /* list all cached .lpkg files */
        DIR *d = opendir(LPKG_CACHE_DIR);
        if (!d) {
            printf("(no packages cached in " LPKG_CACHE_DIR ")\n");
            return;
        }

        struct dirent *e;
        int count = 0;
        printf(C_BOLD "Cached binary packages (" LPKG_CACHE_DIR "):\n" C_RESET);

        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            size_t elen = strlen(e->d_name);
            size_t xlen = strlen(LPKG_EXT);
            if (elen < xlen) continue;
            if (strcmp(e->d_name + elen - xlen, LPKG_EXT) != 0) continue;

            char fullpath[LPM_PATH_MAX];
            snprintf(fullpath, sizeof(fullpath), "%s/%s",
                     LPKG_CACHE_DIR, e->d_name);

            struct stat st;
            double sz_mb = 0;
            if (stat(fullpath, &st) == 0)
                sz_mb = (double)st.st_size / (1024.0 * 1024.0);

            printf("  " C_CYAN "%-48s" C_RESET " " C_GRAY "%.2f MiB" C_RESET "\n",
                   e->d_name, sz_mb);
            count++;
        }
        closedir(d);

        if (count == 0)
            printf("  (none)\n");
        else
            printf("\n  Total: " C_BOLD "%d" C_RESET " package(s)\n", count);
        return;
    }

    /* show info for specified package(s) */
    for (int i = 0; i < argc; i++) {
        char lpkg_path[LPM_PATH_MAX];

        if (argv[i][0] == '/' || strstr(argv[i], ".lpkg")) {
            snprintf(lpkg_path, sizeof(lpkg_path), "%s", argv[i]);
        } else {
            if (lpkg_find(argv[i], lpkg_path, sizeof(lpkg_path)) != 0) {
                fprintf(stderr,
                    C_RED "error:" C_RESET " target not found: %s\n"
                    "  (no matching .lpkg in " LPKG_CACHE_DIR ")\n",
                    argv[i]);
                continue;
            }
        }

        struct stat st;
        if (stat(lpkg_path, &st) != 0) {
            fprintf(stderr, C_RED "error:" C_RESET
                    " file not found: %s\n", lpkg_path);
            continue;
        }

        /* extract meta from the archive without full extraction */
        char workdir[MAX_STR];
        snprintf(workdir, sizeof(workdir),
                 "/tmp/lpm_lpkg_q_%d", (int)getpid());
        util_rmrf(workdir);
        util_mkdirp(workdir, 0755);

        int have_bsdtar = have_tool("bsdtar");

        /* extract only the .lpkg/meta file */
        char ex_cmd[MAX_CMD];
        if (have_bsdtar) {
            snprintf(ex_cmd, sizeof(ex_cmd),
                "bsdtar -C '%s' -xf '%s' '.lpkg/meta' '.lpkg/sha256' 2>/dev/null"
                " || bsdtar -C '%s' -xf '%s' 2>/dev/null",
                workdir, lpkg_path, workdir, lpkg_path);
        } else {
            snprintf(ex_cmd, sizeof(ex_cmd),
                "tar -C '%s' -xzf '%s' './.lpkg/meta' './.lpkg/sha256' 2>/dev/null"
                " || tar -C '%s' -xzf '%s' 2>/dev/null",
                workdir, lpkg_path, workdir, lpkg_path);
        }
        system(ex_cmd);

        LpkgMeta m;
        if (lpkg_read_meta(workdir, &m) != 0) {
            fprintf(stderr, C_RED "error:" C_RESET
                    " cannot read metadata from %s\n", lpkg_path);
            util_rmrf(workdir);
            continue;
        }

        /* read sha256 */
        char shapath[MAX_STR];
        snprintf(shapath, sizeof(shapath), "%s/.lpkg/sha256", workdir);
        FILE *sf = fopen(shapath, "r");
        char sha[129] = "(not available)";
        if (sf) {
            if (fgets(sha, sizeof(sha), sf))
                sha[strcspn(sha, "\n")] = '\0';
            fclose(sf);
        }
        util_rmrf(workdir);

        double sz_mb = (double)st.st_size / (1024.0 * 1024.0);

        printf("\n");
        printf(C_BOLD "Name           : " C_RESET "%s\n",      m.name);
        printf(C_BOLD "Version        : " C_RESET "%s-%s\n",   m.ver, m.rel);
        printf(C_BOLD "Architecture   : " C_RESET "%s\n",      m.arch);
        printf(C_BOLD "Description    : " C_RESET "%s\n",      m.desc[0]  ? m.desc  : "(none)");
        printf(C_BOLD "License        : " C_RESET "%s\n",      m.license[0] ? m.license : "(none)");
        printf(C_BOLD "Build date     : " C_RESET "%s\n",      m.builddate[0] ? m.builddate : "(none)");
        printf(C_BOLD "Package size   : " C_RESET "%.2f MiB\n", sz_mb);
        printf(C_BOLD "SHA-256        : " C_RESET "%s\n",      sha);
        printf(C_BOLD "File           : " C_RESET "%s\n",      lpkg_path);
        printf(C_BOLD "Installed      : " C_RESET "%s\n",
               db_is_installed(m.name) ? C_GREEN "yes" C_RESET : C_YELLOW "no" C_RESET);
        printf("\n");
    }
}

/* ── cmd_pkginstall_dir (-Pe) ─────────────────────────────────────────── *
 * Extract a .lpkg into the current directory for inspection.              *
 * Does NOT install anything to the system.                                */
void cmd_pkginstall_dir(int argc, char **argv) {
    if (argc == 0) {
        fprintf(stderr,
            C_RED "error:" C_RESET " no package specified\n"
            "usage: lpm -Pe <package.lpkg | package-name>\n");
        exit(1);
    }

    int have_bsdtar = have_tool("bsdtar");
    int have_tar    = have_tool("tar");

    if (!have_bsdtar && !have_tar) {
        fprintf(stderr, C_RED "error:" C_RESET " bsdtar or tar required\n");
        exit(1);
    }

    for (int i = 0; i < argc; i++) {
        char lpkg_path[LPM_PATH_MAX];

        if (argv[i][0] == '/' || strstr(argv[i], ".lpkg")) {
            snprintf(lpkg_path, sizeof(lpkg_path), "%s", argv[i]);
        } else {
            if (lpkg_find(argv[i], lpkg_path, sizeof(lpkg_path)) != 0) {
                fprintf(stderr, C_RED "error:" C_RESET
                        " target not found: %s\n", argv[i]);
                continue;
            }
        }

        struct stat st;
        if (stat(lpkg_path, &st) != 0) {
            fprintf(stderr, C_RED "error:" C_RESET
                    " file not found: %s\n", lpkg_path);
            continue;
        }

        /* extract into cwd/<pkgname>/ */
        /* first get name from path */
        const char *base = strrchr(lpkg_path, '/');
        base = base ? base + 1 : lpkg_path;
        char outdir[LPM_NAME_MAX + 8];
        snprintf(outdir, sizeof(outdir), "%s", base);
        /* strip .lpkg extension */
        char *dot = strstr(outdir, LPKG_EXT);
        if (dot) *dot = '\0';

        util_mkdirp(outdir, 0755);

        char ex_cmd[MAX_CMD];
        if (have_bsdtar) {
            snprintf(ex_cmd, sizeof(ex_cmd),
                "bsdtar -C '%s' -xf '%s'", outdir, lpkg_path);
        } else {
            snprintf(ex_cmd, sizeof(ex_cmd),
                "tar -C '%s' -xzf '%s'", outdir, lpkg_path);
        }

        printf(C_CYAN "::" C_RESET " Extracting %s → %s/\n",
               lpkg_path, outdir);

        if (system(ex_cmd) != 0) {
            fprintf(stderr, C_RED "error:" C_RESET
                    " extraction failed\n");
            continue;
        }

        printf(C_GREEN "==> Extracted to" C_RESET " ./%s/\n", outdir);
        printf("    " C_GRAY ".lpkg/meta      — package metadata\n");
        printf("    .lpkg/sha256    — data checksum\n");
        printf("    .lpkg/data.tar.zst — installed files" C_RESET "\n");
    }
}

/* ── cmd_pkgverify (-Pv) ──────────────────────────────────────────────── *
 * Verify the integrity of a .lpkg file (sha256 check, meta sanity).      *
 * Does not install anything.                                              */
void cmd_pkgverify(int argc, char **argv) {
    if (argc == 0) {
        fprintf(stderr,
            C_RED "error:" C_RESET " no package specified\n"
            "usage: lpm -Pv <package.lpkg | package-name>\n");
        exit(1);
    }

    int have_bsdtar = have_tool("bsdtar");

    for (int i = 0; i < argc; i++) {
        char lpkg_path[LPM_PATH_MAX];

        if (argv[i][0] == '/' || strstr(argv[i], ".lpkg")) {
            snprintf(lpkg_path, sizeof(lpkg_path), "%s", argv[i]);
        } else {
            if (lpkg_find(argv[i], lpkg_path, sizeof(lpkg_path)) != 0) {
                fprintf(stderr, C_RED "error:" C_RESET
                        " target not found: %s\n", argv[i]);
                continue;
            }
        }

        struct stat st;
        if (stat(lpkg_path, &st) != 0) {
            fprintf(stderr, C_RED "error:" C_RESET
                    " file not found: %s\n", lpkg_path);
            continue;
        }

        printf(C_CYAN "::" C_RESET " Verifying " C_BOLD "%s" C_RESET "\n",
               lpkg_path);

        /* extract meta only */
        char workdir[MAX_STR];
        snprintf(workdir, sizeof(workdir),
                 "/tmp/lpm_lpkg_v_%d", (int)getpid());
        util_rmrf(workdir);
        util_mkdirp(workdir, 0755);

        char ex_cmd[MAX_CMD];
        if (have_bsdtar) {
            snprintf(ex_cmd, sizeof(ex_cmd),
                "bsdtar -C '%s' -xf '%s' 2>/dev/null", workdir, lpkg_path);
        } else {
            snprintf(ex_cmd, sizeof(ex_cmd),
                "tar -C '%s' -xzf '%s' 2>/dev/null", workdir, lpkg_path);
        }
        system(ex_cmd);

        LpkgMeta m;
        int meta_ok = (lpkg_read_meta(workdir, &m) == 0);

        /* read expected sha256 */
        char shapath[MAX_STR];
        snprintf(shapath, sizeof(shapath), "%s/.lpkg/sha256", workdir);
        FILE *sf = fopen(shapath, "r");
        char expected[129] = "";
        if (sf) {
            if (fgets(expected, sizeof(expected), sf))
                expected[strcspn(expected, "\n")] = '\0';
            fclose(sf);
        }

        int sha_ok = 0;
        if (expected[0]) {
            char datafile[MAX_STR];
            snprintf(datafile, sizeof(datafile), "%s/.lpkg/data.tar.zst", workdir);
            char sha_cmd[MAX_STR];
            snprintf(sha_cmd, sizeof(sha_cmd),
                "sha256sum '%s' | cut -d' ' -f1", datafile);
            FILE *p = popen(sha_cmd, "r");
            char actual[129] = "";
            if (p) {
                if (fgets(actual, sizeof(actual), p))
                    actual[strcspn(actual, "\n")] = '\0';
                pclose(p);
            }
            sha_ok = (strcmp(actual, expected) == 0);
        }

        util_rmrf(workdir);

        /* print results */
        printf("  %-16s %s\n", "meta:",
               meta_ok ? C_GREEN "[OK]" C_RESET : C_RED "[FAIL]" C_RESET);
        if (meta_ok) {
            printf("    name=%s ver=%s-%s arch=%s\n",
                   m.name, m.ver, m.rel, m.arch);
        }

        if (expected[0]) {
            printf("  %-16s %s\n", "sha256:",
                   sha_ok ? C_GREEN "[OK]" C_RESET : C_RED "[MISMATCH]" C_RESET);
        } else {
            printf("  %-16s " C_YELLOW "[SKIP]" C_RESET
                   " (no sha256 file)\n", "sha256:");
            sha_ok = 1; /* no checksum to verify — not a failure */
        }

        if (meta_ok && sha_ok)
            printf(C_GREEN "==> OK:" C_RESET " %s is valid\n", lpkg_path);
        else
            printf(C_RED "==> FAIL:" C_RESET " %s is corrupt or invalid\n",
                   lpkg_path);
    }
}

/* ── cmd_pkgremove_file (-Pr) ─────────────────────────────────────────── *
 * Remove a .lpkg file from the cache (not from the system).              *
 * This is NOT 'lpm -R'; it only deletes the binary package file.         */
void cmd_pkgremove_file(int argc, char **argv) {
    if (argc == 0) {
        fprintf(stderr,
            C_RED "error:" C_RESET " no package specified\n"
            "usage: lpm -Pr <package.lpkg | package-name>\n");
        exit(1);
    }

    for (int i = 0; i < argc; i++) {
        char lpkg_path[LPM_PATH_MAX];

        if (argv[i][0] == '/' || strstr(argv[i], ".lpkg")) {
            snprintf(lpkg_path, sizeof(lpkg_path), "%s", argv[i]);
        } else {
            if (lpkg_find(argv[i], lpkg_path, sizeof(lpkg_path)) != 0) {
                fprintf(stderr, C_RED "error:" C_RESET
                        " target not found: %s\n", argv[i]);
                continue;
            }
        }

        struct stat st;
        if (stat(lpkg_path, &st) != 0) {
            fprintf(stderr, C_RED "error:" C_RESET
                    " file not found: %s\n", lpkg_path);
            continue;
        }

        if (unlink(lpkg_path) == 0) {
            printf(C_GREEN "removed:" C_RESET " %s\n", lpkg_path);
            lpm_log("Removed package file: %s", lpkg_path);
        } else {
            fprintf(stderr, C_RED "error:" C_RESET
                    " cannot remove %s: %s\n", lpkg_path, strerror(errno));
        }
    }
}

#pragma GCC diagnostic pop
