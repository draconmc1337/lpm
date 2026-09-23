/*
 * tests/test_resolver.c — dependency resolver regression tests.
 *
 * The real resolver (src/dep.c) is linked into this test binary unchanged.
 * Only its I/O edges are replaced by the stubs below, so the queueing,
 * provider selection, constraint checking and failure reporting under
 * test are the production code paths:
 *
 *   db_is_installed / db_get_version  -> in-memory installed table
 *   pkgbuild_parse_fast               -> reads a tiny fixture format
 *   parse_repo_db                     -> reads the real on-disk repo.db
 *                                        line format from LPM_DB_DIR
 *
 * LPM_DB_DIR and LLPM_PKGBUILD_DIR are overridden on the command line to
 * point at a scratch directory, so nothing here touches the host system.
 * No root privileges are required and no payload is executed.
 */
#include "lpm.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

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

/* ── scratch directories ─────────────────────────────────────────────── */
static char g_pbdir[700];       /* LPM_PKGBUILD_DIR equivalent  */

static int write_file(const char *p, const char *s) {
    FILE *f = fopen(p, "w");
    if (!f) return -1;
    int ok = (fputs(s, f) != EOF);
    if (fclose(f) != 0) ok = 0;
    return ok ? 0 : -1;
}

/* ── installed-package table (stub backing store) ────────────────────── */
#define INST_MAX 64
static struct { char name[LPM_NAME_MAX]; char ver[LPM_VER_MAX]; } g_inst[INST_MAX];
static int g_ninst = 0;

static void inst_clear(void) { g_ninst = 0; }

static void inst_add(const char *name, const char *ver) {
    if (g_ninst >= INST_MAX) return;
    snprintf(g_inst[g_ninst].name, LPM_NAME_MAX, "%s", name);
    snprintf(g_inst[g_ninst].ver, LPM_VER_MAX, "%s", ver);
    g_ninst++;
}

static const char *inst_lookup(const char *name) {
    for (int i = 0; i < g_ninst; i++)
        if (!strcmp(g_inst[i].name, name)) return g_inst[i].ver;
    return NULL;
}

int db_is_installed(const char *pkgname) { return inst_lookup(pkgname) != NULL; }

char *db_get_version(const char *pkgname) {
    const char *v = inst_lookup(pkgname);
    return v ? strdup(v) : NULL;
}

/* ─ fixture PKGBUILD format ─────────────────────────────────────────────
 *   name=<pkgname>
 *   version=<ver>
 *   release=<rel>
 *   type=source|binary
 *   depends=a,b>=2
 *   provides=virtual-bar
 * ─────────────────────────────────────────────────────────────────────── */
static void split_into(char *val, char dst[][LPM_NAME_MAX], int cap, int *n) {
    int k = 0;
    char *p = val;
    while (*p && k < cap) {
        char *comma = strchr(p, ',');
        if (comma) *comma = '\0';
        while (*p == ' ' || *p == '\t') p++;
        size_t l = strlen(p);
        while (l && (p[l-1] == ' ' || p[l-1] == '\t')) p[--l] = '\0';
        if (l && l < LPM_NAME_MAX) snprintf(dst[k++], LPM_NAME_MAX, "%s", p);
        if (!comma) break;
        p = comma + 1;
    }
    *n = k;
}

int pkgbuild_parse_fast(const char *pbfile, Package *pkg) {
    FILE *f = fopen(pbfile, "r");
    if (!f) return -1;
    memset(pkg, 0, sizeof(*pkg));
    pkg->type = PKG_TYPE_SOURCE;

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = '\0';
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line, *val = eq + 1;
        if      (!strcmp(key, "name"))    snprintf(pkg->name, LPM_NAME_MAX, "%s", val);
        else if (!strcmp(key, "version")) snprintf(pkg->version, LPM_VER_MAX, "%s", val);
        else if (!strcmp(key, "release")) snprintf(pkg->release, sizeof(pkg->release), "%s", val);
        else if (!strcmp(key, "type"))
            pkg->type = (!strcmp(val, "binary")) ? PKG_TYPE_BINARY : PKG_TYPE_SOURCE;
        else if (!strcmp(key, "depends"))
            split_into(val, pkg->depends, LPM_MAX_DEPS, &pkg->ndepends);
        else if (!strcmp(key, "provides"))
            split_into(val, pkg->provides, LPM_MAX_DEPS, &pkg->nprovides);
    }
    fclose(f);
    return pkg->name[0] ? 0 : -1;
}

/* Faithful reader for the documented repo.db line format:
 *   pkgname=VER-REL pkgtype=binary|source ... provides=a,b
 * (src/sync.c's parse_repo_db() is the production implementation; it cannot
 * be linked here without dragging in the whole network layer.) */
int parse_repo_db(const char *path, const char *reponame,
                  RepoEntry *out, int maxn) {
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    char line[1024];
    int n = 0;
    while (n < maxn && fgets(line, sizeof(line), fp)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#' || *p == '\n') continue;

        RepoEntry e;
        memset(&e, 0, sizeof(e));
        snprintf(e.repo, sizeof(e.repo), "%s", reponame);

        char *tok = strtok(p, " \t\n\r");
        int first = 1;
        while (tok) {
            char *eq = strchr(tok, '=');
            if (!eq) { tok = strtok(NULL, " \t\n\r"); continue; }
            *eq = '\0';
            char *key = tok, *val = eq + 1;
            if (first) {
                snprintf(e.name, LPM_NAME_MAX, "%s", key);
                snprintf(e.version, sizeof(e.version), "%s", val);
                first = 0;
            } else if (!strcmp(key, "pkgtype")) {
                e.is_binary = (!strcmp(val, "binary") || !strcmp(val, "bin"));
            } else if (!strcmp(key, "provides")) {
                split_into(val, e.provides, LPM_MAX_DEPS, &e.nprovides);
            }
            tok = strtok(NULL, " \t\n\r");
        }
        if (e.name[0] && e.version[0])
            out[n++] = e;
    }
    fclose(fp);
    return n;
}

/* ── captured stderr, for asserting the failure message text ─────────── */
static int g_cap_fds[2];
static int g_cap_saved = -1;

static int capture_begin(void) {
    if (pipe(g_cap_fds) != 0) return -1;
    g_cap_saved = dup(STDERR_FILENO);
    if (g_cap_saved < 0) { close(g_cap_fds[0]); close(g_cap_fds[1]); return -1; }
    dup2(g_cap_fds[1], STDERR_FILENO);
    close(g_cap_fds[1]);
    return 0;
}

static void capture_end(char *buf, size_t sz) {
    if (buf && sz) buf[0] = '\0';
    fflush(stderr);
    if (g_cap_saved >= 0) {
        dup2(g_cap_saved, STDERR_FILENO);
        close(g_cap_saved);
        g_cap_saved = -1;
    }
    size_t off = 0;
    for (;;) {
        char tmp[512];
        if (buf && sz && off + 1 >= sz) {
            /* drain so the writer never blocks */
            if (read(g_cap_fds[0], tmp, sizeof(tmp)) <= 0) break;
            continue;
        }
        ssize_t r = read(g_cap_fds[0], tmp, sizeof(tmp));
        if (r <= 0) break;
        if (buf && sz) {
            size_t room = sz - 1 - off;
            size_t n = (size_t)r < room ? (size_t)r : room;
            memcpy(buf + off, tmp, n);
            off += n;
        }
    }
    if (buf && sz) buf[off] = '\0';
    close(g_cap_fds[0]);
}

/* ── resolver invocation helpers ─────────────────────────────────────── */
static char g_q[16][MAX_STR];

static int q_has(char q[][MAX_STR], int n, const char *name) {
    for (int i = 0; i < n; i++)
        if (!strcmp(q[i], name)) return 1;
    return 0;
}

static int q_index(char q[][MAX_STR], int n, const char *name) {
    for (int i = 0; i < n; i++)
        if (!strcmp(q[i], name)) return i;
    return -1;
}

static int resolve(const char *name, char q[][MAX_STR], int maxq) {
    char *one[1] = { (char *)name };
    return dep_resolve_queue_multi(one, 1, q, maxq, 0);
}

/* ── scratch fixture helpers ─────────────────────────────────────────── */
static void write_pkgbuild(const char *name, const char *body) {
    char path[800];
    snprintf(path, sizeof(path), "%s/pkgbuild_%s", g_pbdir, name);
    if (write_file(path, body) != 0) {
        fprintf(stderr, "  FAIL: cannot write fixture for %s\n", name);
        failures++;
    }
}

/* Pure constraint-satisfiability checks (lpm_constraint_conflicts). */
static void test_constraints(void) {
    fprintf(stderr, "constraint satisfiability:\n");
    check(lpm_constraint_conflicts("foo>=2", "foo<=1") == 1,
          "lower>upper is a conflict");
    check(lpm_constraint_conflicts("foo>=2", "foo<=2") == 0,
          "touching inclusive bounds are fine");
    check(lpm_constraint_conflicts("foo>2", "foo<=2") == 1,
          "strict lower vs touching upper conflicts");
    check(lpm_constraint_conflicts("foo>=1", "foo<=2") == 0,
          "compatible range is fine");
    check(lpm_constraint_conflicts("foo=1", "foo=2") == 1,
          "two differing pins conflict");
    check(lpm_constraint_conflicts("foo=1", "foo=1") == 0,
          "identical pins are compatible");
    check(lpm_constraint_conflicts("foo=1", "foo>=2") == 1,
          "pin below a lower bound conflicts");
    check(lpm_constraint_conflicts("foo=2", "foo>=2") == 0,
          "pin satisfying a bound is compatible");
    check(lpm_constraint_conflicts("foo", "foo<=1") == 0,
          "unconstrained spec never conflicts");
    check(lpm_constraint_conflicts("foo>=2", "bar<=1") == 0,
          "different package names never conflict");
    check(lpm_constraint_conflicts(NULL, "foo<=1") == 0,
          "NULL input is not a conflict");
}

/* ══ tests ═════════════════════════════════════════════════════════════ */

/* Must run before any repo.db file exists: with no synced index the
 * resolver must still queue an unknown dependency (the PKGBUILD fetch step
 * is the authority on real existence) rather than silently dropping it. */
static void test_no_repo_index(void) {
    fprintf(stderr, "no repository index synced:\n");
    inst_clear();
    write_pkgbuild("norepo", "name=norepo\nversion=1.0\nrelease=1\n"
                             "depends=unknownpkg\n");
    int n = resolve("norepo", g_q, 16);
    check(n == 2, "target + unknown dependency queued (%d)", n);
    check(q_has(g_q, n, "unknownpkg"), "unknown dependency present in queue");
    check(dep_missing_count() == 0,
          "no hard failure while no index is available");
}

static void test_simple_and_missing(void) {
    fprintf(stderr, "simple dependency / missing dependency:\n");
    inst_clear();
    dep_repo_index_reset();

    write_pkgbuild("simple", "name=simple\nversion=1.0\nrelease=1\n"
                             "depends=ncurses\n");
    int n = resolve("simple", g_q, 16);
    check(n == 2 && q_has(g_q, n, "ncurses"),
          "uninstalled unconstrained dependency is queued");
    check(dep_missing_count() == 0, "no failure for a known dependency");

    write_pkgbuild("broken", "name=broken\nversion=1.0\nrelease=1\n"
                             "depends=definitely-absent-pkg\n");
    char err[2048];
    capture_begin();
    n = resolve("broken", g_q, 16);
    int mc = dep_missing_count();
    capture_end(err, sizeof(err));
    check(n == 1, "target queued, phantom dependency is not (%d)", n);
    check(mc != 0, "missing dependency is a hard resolver failure");
    check(!q_has(g_q, n, "definitely-absent-pkg"),
          "unresolvable dependency never enters the queue");
    check(strstr(err, "unable to resolve dependencies") != NULL,
          "error explains the failure");
    check(strstr(err, "broken requires definitely-absent-pkg") != NULL,
          "error names the parent and the missing dependency");
    check(strstr(err, "no matching package/provider found") != NULL,
          "error names the reason");
}

static void test_installed_rules(void) {
    fprintf(stderr, "installed packages and constraints:\n");

    /* installed + unconstrained => satisfied (not queued) */
    inst_clear(); inst_add("ncurses", "6.0");
    write_pkgbuild("uses_nc", "name=uses_nc\nversion=1.0\nrelease=1\n"
                              "depends=ncurses\n");
    int n = resolve("uses_nc", g_q, 16);
    check(n == 1 && !q_has(g_q, n, "ncurses"),
          "installed unconstrained dependency is satisfied");
    check(dep_missing_count() == 0, "no failure");

    /* installed + constraint satisfied => satisfied */
    inst_clear(); inst_add("lib", "1.5");
    write_pkgbuild("needs_ge", "name=needs_ge\nversion=1.0\nrelease=1\n"
                               "depends=lib>=1.0\n");
    n = resolve("needs_ge", g_q, 16);
    check(!q_has(g_q, n, "lib"), "installed version satisfies lower bound");

    /* installed + constraint NOT satisfied => queued for upgrade */
    inst_clear(); inst_add("lib", "0.9");
    n = resolve("needs_ge", g_q, 16);
    check(q_has(g_q, n, "lib"), "installed-but-too-old dependency is queued");

    /* upper bound */
    inst_clear(); inst_add("lib", "3.0");
    write_pkgbuild("needs_le", "name=needs_le\nversion=1.0\nrelease=1\n"
                               "depends=lib<=2.0\n");
    n = resolve("needs_le", g_q, 16);
    check(q_has(g_q, n, "lib"), "installed version above upper bound is queued");

    /* exact pin */
    inst_clear(); inst_add("pin", "1.0");
    write_pkgbuild("needs_exact", "name=needs_exact\nversion=1.0\nrelease=1\n"
                                  "depends=pin=1.0\n");
    n = resolve("needs_exact", g_q, 16);
    check(!q_has(g_q, n, "pin"), "exact version match is satisfied");
    inst_clear(); inst_add("pin", "2.0");
    n = resolve("needs_exact", g_q, 16);
    check(q_has(g_q, n, "pin"), "exact version mismatch is queued");

    /* reinstall target: an installed package named explicitly is queued */
    inst_clear(); inst_add("reinst", "1.0");
    write_pkgbuild("reinst", "name=reinst\nversion=1.0\nrelease=1\n");
    n = resolve("reinst", g_q, 16);
    check(n == 1 && q_has(g_q, n, "reinst"),
          "explicit reinstall target stays in the queue");
}

static void test_graph_shapes(void) {
    fprintf(stderr, "graph shapes:\n");

    /* diamond: root -> a,b; a -> c; b -> c. c must precede a and b. */
    inst_clear();
    write_pkgbuild("root",  "name=root\nversion=1.0\nrelease=1\ndepends=a,b\n");
    write_pkgbuild("a",     "name=a\nversion=1.0\nrelease=1\ndepends=c\n");
    write_pkgbuild("b",     "name=b\nversion=1.0\nrelease=1\ndepends=c\n");
    write_pkgbuild("c",     "name=c\nversion=1.0\nrelease=1\n");
    int n = resolve("root", g_q, 16);
    check(n == 4, "diamond resolves to 4 nodes (%d)", n);
    check(q_index(g_q, n, "c") < q_index(g_q, n, "a") &&
          q_index(g_q, n, "c") < q_index(g_q, n, "b") &&
          q_index(g_q, n, "a") < q_index(g_q, n, "root"),
          "dependencies precede dependents");
    check(dep_missing_count() == 0, "diamond has no missing dependency");

    /* duplicate dependency edge is recorded once */
    write_pkgbuild("dup", "name=dup\nversion=1.0\nrelease=1\ndepends=c,c\n");
    n = resolve("dup", g_q, 16);
    check(n == 2, "duplicate dependency appears once (%d)", n);

    /* cycle terminates */
    write_pkgbuild("cy1", "name=cy1\nversion=1.0\nrelease=1\ndepends=cy2\n");
    write_pkgbuild("cy2", "name=cy2\nversion=1.0\nrelease=1\ndepends=cy1\n");
    n = resolve("cy1", g_q, 16);
    check(n == 2, "cycle terminates with both nodes (%d)", n);
    check(q_index(g_q, n, "cy2") < q_index(g_q, n, "cy1"),
          "cycle is emitted in dependency order");
}

static void test_providers(void) {
    fprintf(stderr, "provides / virtual packages:\n");
    inst_clear();
    dep_repo_index_reset();     /* reload index now that base.db exists */

    /* virtual-bar is provided by realpkg in the repo; nobody installed it */
    write_pkgbuild("consumer", "name=consumer\nversion=1.0\nrelease=1\n"
                               "depends=virtual-bar\n");
    int n = resolve("consumer", g_q, 16);
    check(q_has(g_q, n, "realpkg"),
          "provider is queued for a virtual dependency");
    check(!q_has(g_q, n, "virtual-bar"),
          "virtual name itself is never queued as a package");
    check(dep_missing_count() == 0, "provider satisfies the dependency");

    /* installed provider satisfies it — nothing to queue */
    inst_clear(); inst_add("realpkg", "1.0");
    n = resolve("consumer", g_q, 16);
    check(!q_has(g_q, n, "realpkg"),
          "installed provider satisfies the virtual dependency");

    /* provider version constraint honoured: virtual-num is provided at 1.0
     * by realpkg, consumer2 needs virtual-num>=2 — no provider matches, so
     * this must be a hard failure. */
    write_pkgbuild("consumer2", "name=consumer2\nversion=1.0\nrelease=1\n"
                                "depends=virtual-num>=2\n");
    char err[2048];
    capture_begin();
    n = resolve("consumer2", g_q, 16);
    int mc = dep_missing_count();
    capture_end(err, sizeof(err));
    check(mc != 0 && !q_has(g_q, n, "realpkg"),
          "provider failing the version constraint is rejected");
    check(strstr(err, "consumer2 requires virtual-num") != NULL,
          "rejection names the unsatisfied virtual dependency");
}

static void test_conflicts_and_overflow(void) {
    fprintf(stderr, "conflicting constraints / queue overflow:\n");

    inst_clear();
    write_pkgbuild("wants_new", "name=wants_new\nversion=1.0\nrelease=1\n"
                                "depends=x>=2\n");
    write_pkgbuild("wants_old", "name=wants_old\nversion=1.0\nrelease=1\n"
                                "depends=x<=1\n");
    char *both[2] = { (char *)"wants_new", (char *)"wants_old" };
    char err[2048];
    capture_begin();
    int n = dep_resolve_queue_multi(both, 2, g_q, 16, 0);
    int mc = dep_missing_count();
    capture_end(err, sizeof(err));
    check(mc != 0, "contradictory constraints are a hard failure");
    check(strstr(err, "conflicting version constraints") != NULL,
          "conflict is reported explicitly");
    (void)n;

    /* >256 packages: overflow must fail, never truncate silently. */
    inst_clear();
    for (int i = 0; i < 300; i++) {
        char nm[64], body[192];
        snprintf(nm, sizeof(nm), "deep%03d", i);
        snprintf(body, sizeof(body),
                 "name=deep%03d\nversion=1.0\nrelease=1\ndepends=deep%03d\n",
                 i, i + 1);
        write_pkgbuild(nm, body);
    }
    write_pkgbuild("deep300", "name=deep300\nversion=1.0\nrelease=1\n");

    {
        char err2[256];
        capture_begin();
        int m = resolve("deep000", g_q, 16);
        int r = dep_missing_count();
        capture_end(err2, sizeof(err2));
        check(r != 0, "dependency queue overflow fails explicitly (%d)", m);
        check(strstr(err2, "overflow") != NULL,
              "overflow is named in the error");
    }
}

int main(void) {
    /* LPM_DB_DIR and LPM_PKGBUILD_DIR are overridden to fixed scratch paths
     * on the compile line (see Makefile). Start from a clean slate. */
    (void)util_rmrf(LPM_DB_DIR);
    (void)util_rmrf(LLPM_PKGBUILD_DIR);
    snprintf(g_pbdir, sizeof(g_pbdir), "%s", LLPM_PKGBUILD_DIR);
    if (util_mkdirp(LPM_DB_DIR, 0700) != 0 ||
        util_mkdirp(g_pbdir, 0700) != 0) {
        perror("mkdir scratch");
        return 1;
    }

    test_no_repo_index();
    test_constraints();

    /* first real repo database */
    {
        char dbpath[600];
        snprintf(dbpath, sizeof(dbpath), "%s/base.db", LPM_DB_DIR);
        write_file(dbpath,
            "# name=ver-rel pkgtype=... provides=...\n"
            "ncurses=6.0-1 pkgtype=source\n"
            "lib=1.5-1 pkgtype=source\n"
            "pin=1.0-1 pkgtype=source\n"
            "x=1.5-1 pkgtype=source\n"
            "realpkg=1.0-1 pkgtype=source provides=virtual-bar,virtual-num\n");
    }

    test_simple_and_missing();
    test_installed_rules();
    test_graph_shapes();
    test_providers();
    test_conflicts_and_overflow();

    (void)util_rmrf(LPM_DB_DIR);
    (void)util_rmrf(g_pbdir);

    if (failures) {
        fprintf(stderr, "test_resolver: %d FAILURE(S)\n", failures);
        return 1;
    }
    fprintf(stderr, "test_resolver: all checks passed\n");
    return 0;
}