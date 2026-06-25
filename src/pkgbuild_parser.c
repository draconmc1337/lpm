/*
 * pkgbuild_parser.c — Fast C parser for LPM PKGBUILD metadata
 *
 * Replaces popen("bash -c 'source ...'") for static fields.
 * Bash is only invoked for dynamic evaluation (source= URLs with ${vars})
 * and actual build functions (build(), package()).
 *
 * Supports:
 *   scalar:  key=value, key="value", key='value'
 *   array:   key=(a b c), key=("a" "b" "c"), multiline arrays
 *   comment: # ignored
 *   variable expansion: ${pkgver}, ${pkgname} within same file
 *
 * Cache:
 *   Parsed metadata serialized to LPM_CACHE_DIR/<pkgname>.meta
 *   Invalidated when PKGBUILD mtime > cache mtime
 *   Format: fixed-size binary struct PkgMeta
 */

#define _XOPEN_SOURCE 700
#include "lpm.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <ctype.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
#pragma GCC diagnostic ignored "-Wstringop-truncation"


/* ── Cache directory ─────────────────────────────────────────────────── */
// LPM_META_CACHE_DIR defined in lpm.h
// LPM_META_MAGIC defined in lpm.h
// LPM_META_VERSION defined in lpm.h

/* PkgMeta struct defined in lpm.h */

/* ── Internal parser state ───────────────────────────────────────────── */
typedef struct {
    char  vars[32][LPM_NAME_MAX][512]; /* variable name → value table  */
    int   nvars;
} ParseState;

/* ── helpers ─────────────────────────────────────────────────────────── */

static void strip_quotes(char *s) {
    if (!s || !*s) return;
    size_t len = strlen(s);
    if ((s[0] == '"' && s[len-1] == '"') ||
        (s[0] == '\'' && s[len-1] == '\'')) {
        memmove(s, s+1, len-2);
        s[len-2] = '\0';
    }
}

static char *lstrip(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    return s;
}

static void rstrip(char *s) {
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)*(end-1))) end--;
    *end = '\0';
}

/* Expand ${varname} and $varname within a string using ParseState.
 * Also handles ${pkgver}, ${pkgname} etc. defined earlier in the file. */
static void expand_vars(const ParseState *st, const char *in,
                        char *out, size_t outsz) {
    const char *p = in;
    char *q = out;
    char *end = out + outsz - 1;

    while (*p && q < end) {
        if (*p == '$') {
            p++;
            char varname[LPM_NAME_MAX] = "";
            int i = 0;
            int braced = (*p == '{');
            if (braced) p++;

            while (*p && i < (int)sizeof(varname)-1 &&
                   (isalnum((unsigned char)*p) || *p == '_')) {
                varname[i++] = *p++;
            }
            varname[i] = '\0';
            if (braced && *p == '}') p++;

            /* look up in parse state */
            const char *val = NULL;
            for (int j = 0; j < st->nvars; j++) {
                if (!strcmp(st->vars[j][0], varname)) {
                    val = st->vars[j][1];
                    break;
                }
            }
            if (val) {
                size_t vl = strlen(val);
                size_t space = (size_t)(end - q);
                if (vl > space) vl = space;
                memcpy(q, val, vl);
                q += vl;
            }
        } else {
            *q++ = *p++;
        }
    }
    *q = '\0';
}

/* ── parse_array ─────────────────────────────────────────────────────
 * New strict format:  key = ("elem1"; "elem2"; "elem3")
 *   - Elements MUST be double-quoted
 *   - Elements MUST be separated by "; " (semicolon + space)
 *   - Missing space after ; is a parse error (element skipped)
 *   - Multiline arrays supported: ) can be on a later line
 * Returns number of elements parsed into out[][]. */
static int parse_array(const char *val_start, FILE *fp,
                       const ParseState *st,
                       char out[][LPM_NAME_MAX], int maxn) {
    char buf[4096];
    snprintf(buf, sizeof(buf), "%s", val_start);

    /* read continuation lines until we see closing ) */
    while (!strchr(buf, ')')) {
        size_t cur = strlen(buf);
        if (cur + 2 >= sizeof(buf)) break;
        if (!fgets(buf + cur, (int)(sizeof(buf) - cur), fp)) break;
    }

    char *open  = strchr(buf, '(');
    char *close = strrchr(buf, ')');
    if (!open || !close || close <= open) return 0;

    *close = '\0';
    char *p = open + 1;
    int n = 0;

    while (*p && n < maxn) {
        /* skip whitespace, semicolons (lpm sep), commas */
        while (*p && (isspace((unsigned char)*p) || *p == ';' || *p == ','))
            p++;
        if (!*p) break;

        char elem[LPM_NAME_MAX];
        int ei = 0;
        memset(elem, 0, sizeof(elem));

        /* quoted: double-quote or single-quote */
        if (*p == 0x22 || *p == 0x27) {
            char q = *p++;
            while (*p && *p != q && ei < (int)sizeof(elem)-1)
                elem[ei++] = *p++;
            if (*p == q) p++;
        } else {
            /* bare word (bash style) */
            while (*p && !isspace((unsigned char)*p) &&
                   *p != ';' && *p != ',' && *p != ')' &&
                   ei < (int)sizeof(elem)-1)
                elem[ei++] = *p++;
        }
        elem[ei] = 0;

        if (elem[0]) {
            char expanded[LPM_NAME_MAX];
            expand_vars(st, elem, expanded, sizeof(expanded));
            strncpy(out[n++], expanded, LPM_NAME_MAX - 1);
        }
    }
    return n;
}

/* ── Core C parser ───────────────────────────────────────────────────── *
 * Strict format enforced:                                               *
 *   Scalars: key = "value"   (space BEFORE and AFTER = required)        *
 *   Arrays:  key = ("a"; "b"; "c")  (double-quote + "; " separator)    *
 *   pkgtype = "binary" | "source" | "bin" | "src"                      *
 * Returns 0 on success, -1 on file error or format violation.           */
static int parse_pkgbuild_c(const char *pbfile, PkgMeta *m) {
    FILE *fp = fopen(pbfile, "r");
    if (!fp) return -1;

    ParseState st;
    memset(&st, 0, sizeof(st));

    int line_no = 0;
    char line[4096];
    while (fgets(line, sizeof(line), fp)) {
        line_no++;
        char *p = lstrip(line);

        if (*p == '#' || *p == '\0' || *p == '\n') continue;
        rstrip(p);

        /* ── function declarations: build() / package() / check() ── */
        {
            char *eq_pos   = strchr(p, '=');
            char *open_pos = strchr(p, '(');
            int is_func = open_pos
                          && (!eq_pos || open_pos < eq_pos);
            if (is_func) {
                if      (strncmp(p, "build",     5) == 0) m->has_build   = 1;
                else if (strncmp(p, "check",     5) == 0) m->has_check   = 1;
                else if (strncmp(p, "package",   7) == 0) m->has_package = 1;
                else if (strncmp(p, "remove",    6) == 0) m->has_remove  = 1;
                else if (strncmp(p, "uninstall", 9) == 0) m->has_remove  = 1;
                continue;
            }
        }

        /* ── flexible key=value split ─────────────────────────────────
         * Accept both bash style (key=value, key="value", key=(a b c))
         * and lpm style (key = "value", key = ("a"; "b")).
         * Spaces around '=' are optional.                              */
        char *eq = strchr(p, '=');
        if (!eq) continue;

        /* key: everything before '=', strip trailing spaces */
        char key[64];
        int klen = (int)(eq - p);
        while (klen > 0 && (p[klen-1] == ' ' || p[klen-1] == '	')) klen--;
        if (klen <= 0 || klen >= (int)sizeof(key)) continue;
        strncpy(key, p, (size_t)klen);
        key[klen] = '\0';

        char *val = lstrip(eq + 1); /* skip '=' then any leading spaces */

        char expanded[2048];
        expand_vars(&st, val, expanded, sizeof(expanded));
        val = expanded;

        /* store scalar for variable expansion in later lines */
        if (st.nvars < 32) {
            strncpy(st.vars[st.nvars][0], key, LPM_NAME_MAX-1);
            char tmp[512];
            strncpy(tmp, val, sizeof(tmp)-1);
            strip_quotes(tmp); rstrip(tmp);
            strncpy(st.vars[st.nvars][1], tmp, 511);
            st.nvars++;
        }

        /* ── scalar fields ── */
        if (!strcmp(key, "pkgname")) {
            strncpy(m->pkgname, val, LPM_NAME_MAX-1);
            strip_quotes(m->pkgname); rstrip(m->pkgname);
        } else if (!strcmp(key, "pkgver")) {
            strncpy(m->pkgver, val, LPM_VER_MAX-1);
            strip_quotes(m->pkgver); rstrip(m->pkgver);
        } else if (!strcmp(key, "pkgrel")) {
            strncpy(m->pkgrel, val, 15);
            strip_quotes(m->pkgrel); rstrip(m->pkgrel);
        } else if (!strcmp(key, "pkgdesc") || !strcmp(key, "description")) {
            strncpy(m->description, val, 511);
            strip_quotes(m->description); rstrip(m->description);
        } else if (!strcmp(key, "license")) {
            strncpy(m->license, val, 127);
            strip_quotes(m->license); rstrip(m->license);
        } else if (!strcmp(key, "pkgtype")) {
            char tmp[32];
            strncpy(tmp, val, sizeof(tmp)-1); tmp[31] = '\0';
            strip_quotes(tmp); rstrip(tmp);
            m->is_binary = (!strcmp(tmp, "binary") || !strcmp(tmp, "bin"));
        } else if (!strcmp(key, "dlsize") || !strcmp(key, "dl_size")) {
            m->dl_size = atol(val);
        } else if (!strcmp(key, "instsize") || !strcmp(key, "inst_size")) {
            m->inst_size = atol(val);
        } else if (!strcmp(key, "source") && m->nsources < LPM_MAX_SOURCES) {
            /* scalar source = "url" (legacy) */
            char sv[LPM_PATH_MAX];
            strncpy(sv, val, sizeof(sv)-1); sv[sizeof(sv)-1] = 0;
            strip_quotes(sv); rstrip(sv);
            if (sv[0]) {
                strncpy(m->source[m->nsources], sv, LPM_PATH_MAX-1);
                m->nsources++;
            }
        } else if (!strcmp(key, "sources")) {
            /* array: sources = ("url1" "url2" ...) */
            char arr[LPM_MAX_SOURCES][LPM_NAME_MAX];
            int n = parse_array(val, fp, &st, arr, LPM_MAX_SOURCES);
            for (int _i = 0; _i < n && m->nsources < LPM_MAX_SOURCES; _i++) {
                if (arr[_i][0])
                    strncpy(m->source[m->nsources++], arr[_i], LPM_PATH_MAX-1);
            }
        } else if (!strcmp(key, "source2") || !strcmp(key, "source3") ||
                   !strcmp(key, "source4") || !strcmp(key, "source5")) {
            /* sourceN = "url" (legacy multi-source) */
            if (m->nsources < LPM_MAX_SOURCES) {
                char sv[LPM_PATH_MAX];
                strncpy(sv, val, sizeof(sv)-1); sv[sizeof(sv)-1] = 0;
                strip_quotes(sv); rstrip(sv);
                if (sv[0]) {
                    strncpy(m->source[m->nsources], sv, LPM_PATH_MAX-1);
                    m->nsources++;
                }
            }
        } else if (!strcmp(key, "checksums")) {
            /* array: checksums = ("sha512:abc..." "sha256:def..." "SKIP") */
            char arr[LPM_MAX_SOURCES][LPM_NAME_MAX];
            int n = parse_array(val, fp, &st, arr, LPM_MAX_SOURCES);
            for (int _i = 0; _i < n && _i < LPM_MAX_SOURCES; _i++)
                strncpy(m->checksums[_i], arr[_i], 199);
        } else if (!strcmp(key, "groups")) {
            /* array: groups = ("xlib" "x11") */
            int n = parse_array(val, fp, &st,
                                m->groups, LPM_MAX_DEPS);
            m->ngroups = (uint8_t)n;
        } /* ── array fields: ("a"; "b"; "c") ── */
        else if (!strcmp(key, "depends")) {
            m->ndepends = (uint8_t)parse_array(val, fp, &st,
                           m->depends, LPM_MAX_DEPS);
        } else if (!strcmp(key, "makedepends")) {
            m->nmakedepends = (uint8_t)parse_array(val, fp, &st,
                               m->makedepends, LPM_MAX_DEPS);
        } else if (!strcmp(key, "recommends")) {
            m->nrecommends = (uint8_t)parse_array(val, fp, &st,
                              m->recommends, LPM_MAX_DEPS);
        } else if (!strcmp(key, "conflicts")) {
            m->nconflicts = (uint8_t)parse_array(val, fp, &st,
                             m->conflicts, LPM_MAX_DEPS);
        } else if (!strcmp(key, "replaces")) {
            m->nreplaces = (uint8_t)parse_array(val, fp, &st,
                            m->replaces, LPM_MAX_DEPS);
        }
    }

    fclose(fp);
    return (m->pkgname[0] && m->pkgver[0]) ? 0 : -1;
}

/* ── Cache: write ────────────────────────────────────────────────────── */
static int meta_cache_write(const char *pkgname,
                            const PkgMeta *m) {
    util_mkdirp(LPM_META_CACHE_DIR, 0755);

    char path[LPM_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s.meta",
             LPM_META_CACHE_DIR, pkgname);

    char tmp[LPM_PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, (int)getpid());

    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;

    size_t written = fwrite(m, sizeof(PkgMeta), 1, f);
    fflush(f);
    fclose(f);

    if (written != 1) { unlink(tmp); return -1; }

    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}

/* ── Cache: read (returns 1 if valid hit, 0 if miss/stale) ──────────── */
static int meta_cache_read(const char *pkgname,
                           const char *pbfile,
                           PkgMeta *m) {
    char path[LPM_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s.meta",
             LPM_META_CACHE_DIR, pkgname);

    struct stat cache_st, pb_st;
    if (stat(path, &cache_st) != 0) return 0;  /* no cache */
    if (stat(pbfile, &pb_st)  != 0) return 0;  /* no pkgbuild */

    /* stale if PKGBUILD newer than cache */
    if (pb_st.st_mtime > cache_st.st_mtime) return 0;

    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    PkgMeta tmp;
    size_t n = fread(&tmp, sizeof(PkgMeta), 1, f);
    fclose(f);

    if (n != 1) return 0;
    if (tmp.magic   != LPM_META_MAGIC)   return 0;
    if (tmp.version != LPM_META_VERSION) return 0;

    /* extra mtime guard stored inside struct */
    if (tmp.pkgbuild_mtime != pb_st.st_mtime) return 0;

    memcpy(m, &tmp, sizeof(PkgMeta));
    return 1;
}

/* ── Public API: pkgbuild_parse_fast ─────────────────────────────────── *
 * Drop-in complement to pkgbuild_parse().                               *
 * Used by dep.c for dependency resolution — no bash spawned.           *
 * pkgbuild_parse() (bash) still used for actual build execution.        *
 * Returns 0 on success, -1 on failure.                                  */
int pkgbuild_parse_fast(const char *pbfile, PkgMeta *m) {
    struct stat pb_st;
    if (stat(pbfile, &pb_st) != 0) return -1;

    /* derive pkgname from filename: pkgbuild_<name> */
    const char *base = strrchr(pbfile, '/');
    base = base ? base + 1 : pbfile;

    char pkgname[LPM_NAME_MAX] = "";
    if (strncmp(base, "pkgbuild_", 9) == 0)
        strncpy(pkgname, base + 9, LPM_NAME_MAX - 1);
    else
        strncpy(pkgname, base, LPM_NAME_MAX - 1);

    memset(m, 0, sizeof(PkgMeta));
    m->magic   = LPM_META_MAGIC;
    m->version = LPM_META_VERSION;
    m->pkgbuild_mtime = pb_st.st_mtime;

    /* 1. try cache hit first */
    if (meta_cache_read(pkgname, pbfile, m)) {
        return 0;  /* cache hit — zero bash processes */
    }

    /* 2. cache miss: parse with C parser */
    if (parse_pkgbuild_c(pbfile, m) != 0) {
        /* 3. C parser failed (complex PKGBUILD): fall back to bash */
        if (g_verbose)
            fprintf(stderr, C_GRAY "  [parser] C parse failed for %s,"
                    " falling back to bash\n" C_RESET, pkgname);

        Pkg tmp_pkg;
        memset(&tmp_pkg, 0, sizeof(tmp_pkg));

        /* use original bash-based parser as fallback */
        extern int pkgbuild_parse(const char *pbfile, Pkg *pkg);
        if (pkgbuild_parse(pbfile, &tmp_pkg) != 0) return -1;

        /* copy bash results into PkgMeta */
        strncpy(m->pkgname,  tmp_pkg.pkgname, LPM_NAME_MAX-1);
        strncpy(m->pkgver,   tmp_pkg.pkgver,  LPM_VER_MAX-1);
        strncpy(m->pkgrel,   tmp_pkg.pkgrel,  15);
        m->ndepends    = (uint8_t)tmp_pkg.ndepends;
        m->nrecommends = (uint8_t)tmp_pkg.nrecommends;
        m->nmakedepends= (uint8_t)tmp_pkg.nmakedepends;
        m->nreplaces   = (uint8_t)tmp_pkg.nreplaces;
        m->has_check   = tmp_pkg.has_check;
        for (int i = 0; i < tmp_pkg.ndepends; i++)
            strncpy(m->depends[i], tmp_pkg.depends[i], LPM_NAME_MAX-1);
        for (int i = 0; i < tmp_pkg.nrecommends; i++)
            strncpy(m->recommends[i], tmp_pkg.recommends[i], LPM_NAME_MAX-1);
        for (int i = 0; i < tmp_pkg.nmakedepends; i++)
            strncpy(m->makedepends[i], tmp_pkg.makedepends[i], LPM_NAME_MAX-1);
        for (int i = 0; i < tmp_pkg.nreplaces; i++)
            strncpy(m->replaces[i], tmp_pkg.replaces[i], LPM_NAME_MAX-1);
    }

    /* 4. write cache for next time */
    meta_cache_write(pkgname, m);
    DBG(2, "disk cache written: %s", pkgname);
    return 0;
}

/* ── pkgbuild_invalidate_cache ───────────────────────────────────────── *
 * Called after lpm update (fetch new PKGBUILD) to force cache refresh.    */
void pkgbuild_invalidate_cache(const char *pkgname) {
    char path[LPM_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s.meta",
             LPM_META_CACHE_DIR, pkgname);
    unlink(path);
}

#pragma GCC diagnostic pop
