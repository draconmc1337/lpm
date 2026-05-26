/*
 * buildmeta.c — Reproducible build metadata for lpm
 *
 * Saved to: /var/lib/lpm/buildmeta/<pkgname>.meta  (key=value text)
 * Written by do_build_install() after every successful build.
 * Read by cmd_info() for "lpm info <pkg>".
 */
#define _XOPEN_SOURCE 700
#include "lpm.h"
#include <time.h>
#include <sys/stat.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
#pragma GCC diagnostic ignored "-Wunused-result"

/* ── buildmeta_save ──────────────────────────────────────────────────── */
int buildmeta_save(const char *pkgname, const BuildMeta *m) {
    util_mkdirp(LPM_BUILD_META_DIR, 0755);

    char path[LPM_PATH_MAX + LPM_NAME_MAX + 16];
    snprintf(path, sizeof(path), "%s/%s.meta", LPM_BUILD_META_DIR, pkgname);

    char tmp[LPM_PATH_MAX + LPM_NAME_MAX + 32];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, (int)getpid());

    FILE *f = fopen(tmp, "w");
    if (!f) return -1;

    fprintf(f, "pkgname=%s\n",     m->pkgname);
    fprintf(f, "pkgver=%s\n",      m->pkgver);
    fprintf(f, "pkgrel=%s\n",      m->pkgrel);
    fprintf(f, "built_on=%s\n",    m->built_on);
    fprintf(f, "compiler=%s\n",    m->compiler);
    fprintf(f, "libc=%s\n",        m->libc);
    fprintf(f, "build_flags=%s\n", m->build_flags);
    fprintf(f, "build_hash=%s\n",  m->build_hash);
    fprintf(f, "build_date=%s\n",  m->build_date);
    fprintf(f, "is_binary=%d\n",   m->is_binary);

    fflush(f);
    fclose(f);

    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}

/* ── buildmeta_load ──────────────────────────────────────────────────── */
int buildmeta_load(const char *pkgname, BuildMeta *m) {
    memset(m, 0, sizeof(*m));

    char path[LPM_PATH_MAX + LPM_NAME_MAX + 16];
    snprintf(path, sizeof(path), "%s/%s.meta", LPM_BUILD_META_DIR, pkgname);

    FILE *f = fopen(path, "r");
    if (!f) return -1;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = '\0';
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *key = line;
        const char *val = eq + 1;

        if      (!strcmp(key, "pkgname"))     snprintf(m->pkgname,      sizeof(m->pkgname),      "%s", val);
        else if (!strcmp(key, "pkgver"))      snprintf(m->pkgver,       sizeof(m->pkgver),       "%s", val);
        else if (!strcmp(key, "pkgrel"))      snprintf(m->pkgrel,       sizeof(m->pkgrel),       "%s", val);
        else if (!strcmp(key, "built_on"))    snprintf(m->built_on,     sizeof(m->built_on),     "%s", val);
        else if (!strcmp(key, "compiler"))    snprintf(m->compiler,     sizeof(m->compiler),     "%s", val);
        else if (!strcmp(key, "libc"))        snprintf(m->libc,         sizeof(m->libc),         "%s", val);
        else if (!strcmp(key, "build_flags")) snprintf(m->build_flags,  sizeof(m->build_flags),  "%s", val);
        else if (!strcmp(key, "build_hash"))  snprintf(m->build_hash,   sizeof(m->build_hash),   "%s", val);
        else if (!strcmp(key, "build_date"))  snprintf(m->build_date,   sizeof(m->build_date),   "%s", val);
        else if (!strcmp(key, "is_binary"))   m->is_binary = atoi(val);
    }
    fclose(f);
    return (m->pkgname[0]) ? 0 : -1;
}

/* ── buildmeta_collect ───────────────────────────────────────────────── *
 * Called by do_build_install() to fill a BuildMeta before saving.       *
 * Detects compiler, musl version, CFLAGS from environment.              */
void buildmeta_collect(const char *pkgname, const char *pkgver,
                       const char *pkgrel, int is_binary,
                       const char *pkgdir, BuildMeta *m) {
    memset(m, 0, sizeof(*m));
    snprintf(m->pkgname, sizeof(m->pkgname), "%s", pkgname);
    snprintf(m->pkgver,  sizeof(m->pkgver),  "%s", pkgver);
    snprintf(m->pkgrel,  sizeof(m->pkgrel),  "%s", pkgrel);
    m->is_binary = is_binary;

    /* built_on: distro + release */
    snprintf(m->built_on, sizeof(m->built_on), "Lotus Linux");
    FILE *rel = fopen("/etc/lotus-release", "r");
    if (rel) {
        char rbuf[64];
        if (fgets(rbuf, sizeof(rbuf), rel)) {
            rbuf[strcspn(rbuf, "\n")] = '\0';
            snprintf(m->built_on, sizeof(m->built_on), "%s", rbuf);
        }
        fclose(rel);
    }

    /* compiler: try clang first, then gcc */
    m->compiler[0] = '\0';
    const char *compilers[] = { "clang --version", "gcc --version", NULL };
    for (int i = 0; compilers[i] && !m->compiler[0]; i++) {
        FILE *p = popen(compilers[i], "r");
        if (p) {
            char buf[128];
            if (fgets(buf, sizeof(buf), p)) {
                buf[strcspn(buf, "\n")] = '\0';
                /* extract just "clang 19.1.7" or "gcc 13.2.0" */
                char *ver = strstr(buf, "version ");
                if (ver) {
                    char *name = strstr(buf, "clang") ? "clang" : "gcc";
                    char vnum[32] = "";
                    sscanf(ver + 8, "%31s", vnum);
                    snprintf(m->compiler, sizeof(m->compiler), "%s %s", name, vnum);
                } else {
                    snprintf(m->compiler, sizeof(m->compiler), "%.63s", buf);
                }
            }
            pclose(p);
        }
    }
    if (!m->compiler[0])
        snprintf(m->compiler, sizeof(m->compiler), "unknown");

    /* libc: musl version */
    snprintf(m->libc, sizeof(m->libc), "musl");
    FILE *p = popen("musl-libc --version 2>/dev/null || "
                    "ldd /bin/sh 2>/dev/null | head -1", "r");
    if (p) {
        char buf[64];
        if (fgets(buf, sizeof(buf), p)) {
            buf[strcspn(buf, "\n")] = '\0';
            char *v = strstr(buf, "musl");
            if (v) {
                char *sp = strchr(v, ' ');
                if (sp)
                    snprintf(m->libc, sizeof(m->libc), "musl %s", sp + 1);
            }
        }
        pclose(p);
    }

    /* build_flags: from environment */
    const char *cf = getenv("CFLAGS");
    snprintf(m->build_flags, sizeof(m->build_flags), "%s",
             cf ? cf : "-O2 -pipe");

    /* build_hash: SHA-256 of pkgdir tree (sort + hash file paths+sizes) */
    if (pkgdir && pkgdir[0]) {
        char cmd[MAX_CMD];
        snprintf(cmd, sizeof(cmd),
            "find '%s' -type f | sort | xargs sha256sum 2>/dev/null "
            "| sha256sum | awk '{print $1}'", pkgdir);
        FILE *hp = popen(cmd, "r");
        if (hp) {
            char hbuf[68];
            if (fgets(hbuf, sizeof(hbuf), hp)) {
                hbuf[strcspn(hbuf, "\n")] = '\0';
                snprintf(m->build_hash, sizeof(m->build_hash), "%s", hbuf);
            }
            pclose(hp);
        }
    }
    if (!m->build_hash[0])
        snprintf(m->build_hash, sizeof(m->build_hash), "(not available)");

    /* build_date: ISO-8601 */
    time_t now = time(NULL);
    struct tm *tm = gmtime(&now);
    strftime(m->build_date, sizeof(m->build_date), "%Y-%m-%dT%H:%M:%SZ", tm);
}

#pragma GCC diagnostic pop
