/*
 * tests/test_security.c — trust-boundary regression tests.
 *
 * Covers the P0 issues fixed in this pass:
 *   - metadata validators reject shell syntax / traversal / control bytes
 *   - unsafe metadata never executes anything (no marker file appears)
 *   - util_rmrf / util_copy_file are shell-free and never follow a
 *     destination symlink (the classic /tmp symlink attack)
 *   - util_mkdtemp creates private 0700 directories
 *   - checksums are verified and mismatches rejected
 *
 * Everything runs in /tmp as a normal user; nothing touches the real root.
 */
#include "lpm.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>

static int failures = 0;

static void check(int cond, const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "  %s: ", cond ? "ok  " : "FAIL");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    if (!cond) failures++;
}

static int exists(const char *p) {
    struct stat st;
    return lstat(p, &st) == 0;
}

static int write_file(const char *p, const char *s) {
    FILE *f = fopen(p, "w");
    if (!f) return -1;
    int ok = (fputs(s, f) != EOF);
    if (fclose(f) != 0) ok = 0;
    return ok ? 0 : -1;
}

static char *slurp(const char *p) {
    static char buf[4096];
    FILE *f = fopen(p, "r");
    if (!f) return NULL;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

/* ── validators ──────────────────────────────────────────────────────── */

static void test_validators(void) {
    fprintf(stderr, "validators:\n");

    const char *bad[] = {
        "x'; touch /tmp/lpm-sec-pwned; #",
        "../../etc/passwd",
        "foo/bar",
        "foo\nbar",
        "foo$(touch /tmp/lpm-sec-pwned)",
        "foo`touch /tmp/lpm-sec-pwned`",
        "foo;bar", "foo&bar", "foo|bar", "foo\"bar", "foo'bar",
        "foo(bar)", "foo)bar", "foo$bar", "foo\\bar", "foo bar",
        "", ".", "..", "-rf", "+x", "foo*", "foo?", "foo[0]",
        NULL
    };
    for (int i = 0; bad[i]; i++)
        check(!lpm_valid_pkgname(bad[i]), "reject pkgname '%s'", bad[i]);

    const char *good_names[] = {
        "musl", "busybox", "libx11", "gtk+2", "python3.11", "foo-bar_baz@x",
        "a", NULL
    };
    for (int i = 0; good_names[i]; i++)
        check(lpm_valid_pkgname(good_names[i]), "accept pkgname '%s'",
              good_names[i]);

    check(lpm_valid_version("1.2.3"), "accept version 1.2.3");
    check(lpm_valid_version("1:2.0~rc1-2"), "accept version with epoch");
    check(!lpm_valid_version("1.0; rm -rf /"), "reject version with shell");
    check(!lpm_valid_version("1.0/2"), "reject version with slash");
    check(!lpm_valid_version(""), "reject empty version");

    check(lpm_valid_release("1"), "accept release 1");
    check(lpm_valid_release("2.1"), "accept release 2.1");
    check(!lpm_valid_release("1;x"), "reject release with shell");

    check(lpm_valid_filename("foo-1.0.tar.gz"), "accept plain filename");
    check(!lpm_valid_filename("../foo"), "reject filename with traversal");
    check(!lpm_valid_filename("a/b"), "reject filename with slash");
    check(!lpm_valid_filename(""), "reject empty filename");

    check(lpm_valid_relative_path("etc/foo.conf"), "accept relative path");
    check(!lpm_valid_relative_path("/etc/passwd"), "reject absolute path");
    check(!lpm_valid_relative_path("../../etc/passwd"),
          "reject traversing path");
    check(!lpm_valid_relative_path("etc/../../x"), "reject inner traversal");
    check(!lpm_valid_relative_path("etc//foo"), "reject empty component");
    check(!lpm_valid_relative_path("etc/foo\nbar"), "reject control byte");

    check(lpm_valid_url("https://example.org/a/b.tar.gz"), "accept https url");
    check(lpm_valid_url("http://example.org/x?y=1&z=2"), "accept http url");

    const char *bad_urls[] = {
        "ftp:/x", "javascript:alert(1)", "/etc/passwd", "",
        "https://a b/c", "https://a/\"b\"", "https://a/\nb", NULL
    };
    for (int i = 0; bad_urls[i]; i++)
        check(!lpm_valid_url(bad_urls[i]), "reject url '%s'", bad_urls[i]);

    check(lpm_valid_depspec("ncurses"), "accept bare depspec");
    check(lpm_valid_depspec("ncurses>=6"), "accept versioned depspec");
    check(lpm_valid_depspec("virtual-bar=1.2"), "accept provides spec");
    check(!lpm_valid_depspec("ncurses; touch /tmp/lpm-sec-pwned"),
          "reject depspec with shell syntax");
    check(!lpm_valid_depspec("../../x>=1"), "reject depspec with traversal");
}

/* ── shell-injection resistance of the path helpers ──────────────────── */

static void test_no_shell_execution(void) {
    fprintf(stderr, "shell-injection resistance:\n");

    /* The injected payload is `touch lpm-sec-pwned`, a relative path, and
     * the tests chdir into a scratch dir: if a shell ever interpreted the
     * value, the marker would appear in the scratch dir. */
    const char *marker = "lpm-sec-pwned";

    char root[] = "/tmp/lpm-sec-XXXXXX";
    if (!mkdtemp(root)) { perror("mkdtemp"); failures++; return; }

    char cwd[1024];
    if (!getcwd(cwd, sizeof(cwd))) { perror("getcwd"); failures++; return; }
    if (chdir(root) != 0) { perror("chdir"); failures++; return; }
    unlink(marker);

    /* A directory whose name contains shell syntax. Under the old
     * `rm -rf '%s'` implementation the embedded quote broke out of the
     * single-quoted string and ran the injected command. */
    const char *evil = "x'; touch lpm-sec-pwned; #";
    check(mkdir(evil, 0700) == 0, "create dir with shell metacharacters");
    check(util_rmrf(evil) == 0, "util_rmrf removes such a dir");
    check(!exists(marker), "no injected command ran (rmrf)");
    check(!exists(evil), "the directory itself is gone");

    /* Same for the copy helper: neither source nor destination name may
     * be interpreted by a shell. */
    const char *src = "src$(touch lpm-sec-pwned)";
    const char *dst = "dst`touch lpm-sec-pwned`";
    check(write_file(src, "payload") == 0, "create source with metacharacters");
    check(util_copy_file(src, dst) == 0, "util_copy_file with metacharacters");
    check(!exists(marker), "no injected command ran (copy)");
    char *got = slurp(dst);
    check(got && !strcmp(got, "payload"), "copied contents intact");

    /* Argument with shell metacharacters is passed literally, never run. */
    char out[256];
    const char *argv[] = { "/bin/echo", "a;b$(id)`id`", NULL };
    check(util_capture(argv, out, sizeof(out)) == 0, "util_capture runs argv");
    check(out[0] && strstr(out, "a;b$(id)`id`") != NULL,
          "metacharacters arrive as one literal argument");
    check(!exists(marker), "no injected command ran (capture)");

    if (chdir(cwd) != 0) { perror("chdir back"); failures++; }
    check(util_rmrf(root) == 0, "cleanup test root");
}

/* ── temp-file safety ────────────────────────────────────────────────── */

static void test_tempfiles(void) {
    fprintf(stderr, "temp files:\n");

    char dir[512];
    check(util_mkdtemp("/tmp", dir, sizeof(dir)) == 0, "util_mkdtemp succeeds");
    struct stat st;
    check(stat(dir, &st) == 0 && (st.st_mode & 07777) == 0700,
          "temp dir mode is 0700");

    char dir2[512];
    check(util_mkdtemp("/tmp", dir2, sizeof(dir2)) == 0 &&
          strcmp(dir, dir2) != 0, "temp dir names are unpredictable");

    /* A pre-created symlink at a destination path must never be written
     * through: util_copy_file replaces the link itself. */
    char src[600], dst[600], victim[600];
    snprintf(src, sizeof(src), "%s/src", dir);
    snprintf(dst, sizeof(dst), "%s/dst", dir);
    snprintf(victim, sizeof(victim), "%s/victim", dir);

    check(write_file(victim, "ORIGINAL") == 0, "create victim file");
    check(write_file(src, "NEWDATA") == 0, "create source file");
    check(symlink(victim, dst) == 0, "pre-create dst symlink -> victim");

    check(util_copy_file(src, dst) == 0, "copy over the symlink");
    char *v = slurp(victim);
    check(v && !strcmp(v, "ORIGINAL"), "symlink target not overwritten");
    check(lstat(dst, &st) == 0 && S_ISREG(st.st_mode),
          "destination is now a regular file");

    check(util_rmrf(dir) == 0, "cleanup temp dir");
    check(util_rmrf(dir2) == 0, "cleanup second temp dir");

    /* util_rmrf on a symlink removes the link, not its target. */
    char r[] = "/tmp/lpm-sec-rm-XXXXXX";
    if (mkdtemp(r)) {
        char target[600], link[600];
        snprintf(target, sizeof(target), "%s/target", r);
        snprintf(link, sizeof(link), "%s/link", r);
        write_file(target, "keep");
        symlink(target, link);
        check(util_rmrf(link) == 0, "util_rmrf removes a symlink");
        check(exists(target), "symlink target preserved");
        util_rmrf(r);
    }
}

/* ── checksum verification ───────────────────────────────────────────── */

static void test_checksums(void) {
    fprintf(stderr, "checksums:\n");

    char dir[] = "/tmp/lpm-sec-ck-XXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); failures++; return; }
    char path[600];
    snprintf(path, sizeof(path), "%s/f", dir);
    write_file(path, "hello");

    /* sha256("hello") */
    const char *want =
        "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824";
    check(cksum_verify(path, want, CKSUM_SHA256) == 0,
          "accept correct sha256");
    check(cksum_verify(path, "deadbeef", CKSUM_SHA256) != 0,
          "reject wrong sha256");
    check(cksum_verify(path, want, CKSUM_INVALID) != 0,
          "reject invalid checksum spec outright");
    check(cksum_verify(path, "SKIP", CKSUM_SKIP) == 0,
          "SKIP is an explicit no-op");

    char computed[129];
    check(cksum_compute(path, CKSUM_SHA256, computed, sizeof(computed)) == 0 &&
          !strcmp(computed, want), "cksum_compute matches sha256sum");

    util_rmrf(dir);
}

/* ── package metadata validation ─────────────────────────────────────── */

static void test_metadata(void) {
    fprintf(stderr, "metadata validation:\n");

    Package *p = calloc(1, sizeof(Package));
    if (!p) { failures++; return; }

    snprintf(p->name, sizeof(p->name), "goodpkg");
    snprintf(p->version, sizeof(p->version), "1.2.3");
    snprintf(p->release, sizeof(p->release), "1");
    check(lpm_pkg_validate_metadata(p, "PKGBUILD") == 0,
          "accept well-formed metadata");

    snprintf(p->name, sizeof(p->name), "x'; touch /tmp/lpm-sec-pwned; #");
    check(lpm_pkg_validate_metadata(p, "PKGBUILD") != 0,
          "reject malicious package name");
    snprintf(p->name, sizeof(p->name), "goodpkg");

    snprintf(p->version, sizeof(p->version), "1.0/../../etc");
    check(lpm_pkg_validate_metadata(p, "PKGBUILD") != 0,
          "reject malicious version");
    snprintf(p->version, sizeof(p->version), "1.2.3");

    snprintf(p->depends[0], LPM_NAME_MAX, "ncurses; touch /tmp/lpm-sec-pwned");
    p->ndepends = 1;
    check(lpm_pkg_validate_metadata(p, "PKGBUILD") != 0,
          "reject malicious dependency");
    p->ndepends = 0;

    snprintf(p->backup[0], LPM_PATH_MAX, "../../etc/passwd");
    p->nbackup = 1;
    check(lpm_pkg_validate_metadata(p, "PKGBUILD") != 0,
          "reject traversing backup path");
    p->nbackup = 0;

    /* a relative local patch path is legitimate … */
    snprintf(p->sources[0].url, LPM_URL_MAX, "patches/musl-fix.patch");
    snprintf(p->sources[0].filename, LPM_NAME_MAX, "musl-fix.patch");
    p->nsources = 1;
    check(lpm_pkg_validate_metadata(p, "PKGBUILD") == 0,
          "accept relative patch source path");

    /* … but a malicious one is not */
    snprintf(p->sources[0].url, LPM_URL_MAX, "patches/x; touch pwned");
    check(lpm_pkg_validate_metadata(p, "PKGBUILD") != 0,
          "reject source path with shell metacharacters");
    snprintf(p->sources[0].url, LPM_URL_MAX, "../outside.patch");
    check(lpm_pkg_validate_metadata(p, "PKGBUILD") != 0,
          "reject traversing source path");

    snprintf(p->sources[0].url, LPM_URL_MAX, "/etc/passwd");
    snprintf(p->sources[0].filename, LPM_NAME_MAX, "evil");
    check(lpm_pkg_validate_metadata(p, "PKGBUILD") != 0,
          "reject absolute source path");

    snprintf(p->sources[0].url, LPM_URL_MAX, "https://x/\"y\"");
    check(lpm_pkg_validate_metadata(p, "PKGBUILD") != 0,
          "reject source with quote in URL");

    snprintf(p->sources[0].url, LPM_URL_MAX,
             "https://ok/x.tar.gz#bad\nfragment");
    check(lpm_pkg_validate_metadata(p, "PKGBUILD") != 0,
          "reject source URL with embedded newline");
    p->nsources = 0;

    free(p);
    check(!exists("/tmp/lpm-sec-pwned"), "no marker file was ever created");
}

int main(void) {
    test_validators();
    test_no_shell_execution();
    test_tempfiles();
    test_checksums();
    test_metadata();

    if (failures) {
        fprintf(stderr, "test_security: %d FAILURE(S)\n", failures);
        return 1;
    }
    fprintf(stderr, "test_security: all checks passed\n");
    return 0;
}
