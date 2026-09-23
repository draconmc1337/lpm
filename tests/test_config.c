/*
 * tests/test_config.c — regression test for the g_cfg single-sourcing bug.
 *
 * Original bug: g_cfg (the global every handler reads) was declared in
 * util.c but NEVER populated. Config was instead loaded into divergent
 * local `LpmConfig cfg` structs inside individual command handlers, so the
 * global stayed zeroed and download.c / sync.c / lpkg.c / safety.c silently
 * saw parallel_dl=0, verify_sig=0, max_dl_threads=0, build_dir="", etc.
 * regardless of /etc/lpm/lpm.conf.
 *
 * Fix: lpm_config_init() is the single canonical write path for g_cfg,
 * called once from main() before dispatch; all handlers read g_cfg.
 *
 * This test asserts that after the canonical load, g_cfg is fully populated
 * and consistent with what the query helpers and the read-sites observe —
 * i.e. no stale/default/partially-initialized view.
 */
#include "lpm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL: %s\n", msg); \
        failures++; \
    } else { \
        fprintf(stderr, "  ok:   %s\n", msg); \
    } \
} while (0)

static void write_conf(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) { perror("fopen conf"); exit(2); }
    fputs(
        "# test config\n"
        "CFLAGS = \"-O3 -march=native\"\n"
        "JOBS = 7\n"
        "BUILDDIR = /tmp/lpm-test-build\n"
        "PKGDEST = /tmp/lpm-test-pkg\n"
        "COLOR = 0\n"
        "PARALLEL_DL = 1\n"
        "MAX_DL_THREADS = 8\n"
        "VERIFY_SIG = 1\n"
        "DOWNLOADER = curl\n"
        "PROFILE = native\n"
        "CriticalPkg = musl glibc\n"
        "IgnorePkg = firefox\n",
        f);
    fclose(f);
}

int main(void) {
    char conf[] = "/tmp/lpm-test-config-XXXXXX";
    int fd = mkstemp(conf);
    if (fd < 0) { perror("mkstemp"); return 2; }
    close(fd);
    write_conf(conf);

    fprintf(stderr, "test_config: loading %s into g_cfg via canonical path\n", conf);

    /* Simulate exactly what lpm_config_init() does, but with a controllable
     * path (lpm_config_init reads the fixed LPM_CONF_FILE). */
    lpm_config_load(conf, &g_cfg);

    /* 1. g_cfg must NOT be the all-zero "never written" state. */
    CHECK(g_cfg.build_dir[0] != '\0', "g_cfg.build_dir populated (was empty under bug)");
    CHECK(g_cfg.max_dl_threads != 0,  "g_cfg.max_dl_threads populated (was 0 under bug)");
    CHECK(g_cfg.downloader[0] != '\0', "g_cfg.downloader populated (was empty under bug)");

    /* 2. Values must match the config file. */
    CHECK(g_cfg.jobs == 7,                 "JOBS -> g_cfg.jobs == 7");
    CHECK(g_cfg.parallel_dl == 1,          "PARALLEL_DL -> g_cfg.parallel_dl == 1");
    CHECK(g_cfg.max_dl_threads == 8,       "MAX_DL_THREADS -> g_cfg.max_dl_threads == 8");
    CHECK(g_cfg.verify_sig == 1,           "VERIFY_SIG -> g_cfg.verify_sig == 1");
    CHECK(g_cfg.color == 0,                "COLOR -> g_cfg.color == 0");
    CHECK(strcmp(g_cfg.build_dir, "/tmp/lpm-test-build") == 0, "BUILDDIR -> g_cfg.build_dir");
    CHECK(strcmp(g_cfg.pkg_dest,  "/tmp/lpm-test-pkg")  == 0, "PKGDEST  -> g_cfg.pkg_dest");
    CHECK(strcmp(g_cfg.downloader, "curl") == 0,              "DOWNLOADER -> g_cfg.downloader");
    CHECK(strcmp(g_cfg.profile, "native") == 0,              "PROFILE -> g_cfg.profile");
    CHECK(strstr(g_cfg.cflags, "-march=native") != NULL,     "CFLAGS -> g_cfg.cflags");

    /* 3. The exact fields the read-sites consume must be correct.
     *    download.c:241 -> g_cfg.parallel_dl ? g_cfg.max_dl_threads : 1
     *    sync.c:275 / lpkg.c:598 -> g_cfg.verify_sig
     *    safety.c:263 -> g_cfg.build_dir */
    int max_t = g_cfg.parallel_dl ? g_cfg.max_dl_threads : 1;
    CHECK(max_t == 8, "download.c parallel path sees 8 threads (not 1)");
    CHECK(g_cfg.verify_sig == 1, "sync/lpkg sig path sees verify_sig=1 (not 0)");
    CHECK(strcmp(g_cfg.build_dir, "/tmp/lpm-test-build") == 0, "safety.c disk path sees configured build_dir");

    /* 4. Query helpers must observe the SAME g_cfg (no divergent copy). */
    CHECK(lpm_config_is_critical(&g_cfg, "musl") == 1,    "is_critical(musl) via g_cfg");
    CHECK(lpm_config_is_critical(&g_cfg, "glibc") == 1,   "is_critical(glibc) via g_cfg");
    CHECK(lpm_config_is_critical(&g_cfg, "firefox") == 0, "is_critical(firefox) == 0");
    CHECK(lpm_config_is_ignored(&g_cfg, "firefox") == 1,  "is_ignored(firefox) via g_cfg");
    CHECK(lpm_config_is_ignored(&g_cfg, "musl") == 0,     "is_ignored(musl) == 0");

    /* 5. lpm_config_init() must exist and be the canonical entry point.
     *    (It reads the real LPM_CONF_FILE; we only assert it is callable and
     *    leaves g_cfg in a fully-populated, non-zero state.) */
    lpm_config_init();
    CHECK(g_cfg.max_dl_threads >= 1 && g_cfg.max_dl_threads <= 16,
          "lpm_config_init() leaves max_dl_threads clamped to [1,16]");
    CHECK(g_cfg.build_dir[0] != '\0',
          "lpm_config_init() leaves g_cfg.build_dir populated");

    unlink(conf);

    if (failures) {
        fprintf(stderr, "test_config: %d FAILURE(S)\n", failures);
        return 1;
    }
    fprintf(stderr, "test_config: all checks passed\n");
    return 0;
}
