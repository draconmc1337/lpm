/*
 * pkgbuild_parser.c — the LPDF v1 parser (lpm 2.0)
 *
 * The only parser: fills Package directly, no bash involved and no
 * fallback parser. LPDF v1 is a static, declarative format by design —
 * a parse failure here means the file is malformed, not that it needs a
 * second parsing strategy. Bash is only ever invoked later, separately,
 * to actually execute build()/check()/package() (do_build_install() in
 * build.c) — never to parse metadata.
 *
 * Supports:
 *   scalar:  key = "value"  (also bash-style key=value, key="value")
 *   array:   key = ("a" "b" "c"), multiline arrays
 *   comment: # ignored
 *   variable expansion: ${pkgver}, ${pkgname} within same file
 *
 * Cache:
 *   Parsed Package serialized to LPM_META_CACHE_DIR/<pkgname>.meta,
 *   wrapped in a small MetaCacheEntry envelope (magic/version/mtime).
 *   Invalidated when PKGBUILD mtime > cache mtime, or by
 *   pkgbuild_invalidate_cache() after `lpm update`.
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
// LPM_META_CACHE_DIR / LPM_META_MAGIC / LPM_META_VERSION / MetaCacheEntry
// all defined in lpm.h

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
 * Bash-compatible array syntax:  key = ("elem1" "elem2" "elem3")
 *   - Elements may be double- or single-quoted, or bare words
 *   - Elements are separated by whitespace (newlines included, so
 *     multi-line arrays with one element per line just work); a
 *     semicolon or comma between elements is also tolerated but never
 *     required — plain `sources=("url1" "url2")` and the multi-line
 *     form are both valid, there is no Lotus-specific separator.
 *   - Multiline arrays supported: the closing ')' can be on a later line
 *
 * out is a flat pointer into a 2D array whose row width is rowsz (e.g.
 * (char *)m->depends, LPM_NAME_MAX or (char *)m->source, LPM_PATH_MAX).
 * Parsing always happens into a LPM_PATH_MAX-sized local scratch buffer
 * regardless of rowsz, so long elements (a full sha512: checksum is 135
 * chars; some source URLs run longer than LPM_NAME_MAX) are only ever
 * truncated at the real row width, not at some earlier, narrower stage.
 *
 * Returns number of elements parsed into out. */
static int parse_array(const char *val_start, FILE *fp,
                       const ParseState *st,
                       char *out, size_t rowsz, int maxn) {
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
        /* skip whitespace, semicolons, commas — any of these separate
         * elements, but none of them is required (see docstring above) */
        while (*p && (isspace((unsigned char)*p) || *p == ';' || *p == ','))
            p++;
        if (!*p) break;

        char elem[LPM_PATH_MAX];
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
            char expanded[LPM_PATH_MAX];
            expand_vars(st, elem, expanded, sizeof(expanded));
            char *dst = out + (size_t)n * rowsz;
            strncpy(dst, expanded, rowsz - 1);
            dst[rowsz - 1] = '\0';
            n++;
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
static int parse_pkgbuild_c(const char *pbfile, Package *m) {
    FILE *fp = fopen(pbfile, "r");
    if (!fp) return -1;

    ParseState st;
    memset(&st, 0, sizeof(st));

    /* staged sources=/checksums= — combined into m->sources[] (unified
     * Source struct) after the whole file is read, since the two keys
     * can appear in either order. */
    char src_urls[LPM_MAX_SOURCES][LPM_PATH_MAX];
    char src_cksums[LPM_MAX_SOURCES][200];
    int  n_src_urls = 0, n_src_cksums = 0;
    memset(src_urls, 0, sizeof(src_urls));
    memset(src_cksums, 0, sizeof(src_cksums));

    int line_no = 0;
    char line[4096];
    while (fgets(line, sizeof(line), fp)) {
        line_no++;
        char *p = lstrip(line);

        if (*p == '#' || *p == '\0' || *p == '\n') continue;
        rstrip(p);

        /* ── function declarations: only ones anything actually consumes.
         * build()/package() run unconditionally in do_build_install(), so
         * they don't need a has_* flag. check() gates check() execution
         * there; pre_install()/post_install() gate pkg_run_hook() from
         * tx_commit() (transaction.c). remove()/uninstall() have no
         * consumer yet — package removal doesn't run hooks (tx_commit()
         * only processes tx->install, not tx->remove) — add that back
         * together with the removal hook path, not before it exists. */
        {
            char *eq_pos   = strchr(p, '=');
            char *open_pos = strchr(p, '(');
            int is_func = open_pos
                          && (!eq_pos || open_pos < eq_pos);
            if (is_func) {
                if      (strncmp(p, "check",        5)  == 0) m->has_check        = 1;
                else if (strncmp(p, "pre_install",  11) == 0) m->has_pre_install  = 1;
                else if (strncmp(p, "post_install", 12) == 0) m->has_post_install = 1;
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
            strncpy(m->name, val, LPM_NAME_MAX-1);
            strip_quotes(m->name); rstrip(m->name);
        } else if (!strcmp(key, "pkgver")) {
            strncpy(m->version, val, LPM_VER_MAX-1);
            strip_quotes(m->version); rstrip(m->version);
        } else if (!strcmp(key, "pkgrel")) {
            strncpy(m->release, val, 15);
            strip_quotes(m->release); rstrip(m->release);
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
            m->type = (!strcmp(tmp, "binary") || !strcmp(tmp, "bin"))
                      ? PKG_TYPE_BINARY : PKG_TYPE_SOURCE;
        } else if (!strcmp(key, "dlsize") || !strcmp(key, "dl_size")) {
            m->dl_size = atol(val);
        } else if (!strcmp(key, "instsize") || !strcmp(key, "inst_size")) {
            m->inst_size = atol(val);
        } else if (!strcmp(key, "sources")) {
            /* array: sources = ("url1" "url2" "patches/foo.patch" ...)
             * Staged here; combined with checksums[] into m->sources[]
             * (the unified Source struct) once the whole file is read —
             * sources= and checksums= can appear in either order. */
            n_src_urls = (int)parse_array(val, fp, &st,
                          (char *)src_urls, LPM_PATH_MAX, LPM_MAX_SOURCES);
        } else if (!strcmp(key, "checksums")) {
            /* array: checksums = ("sha512:abc..." "SKIP" ...), one entry
             * per sources[] at the same index. Parsed at the real
             * 200-byte width — a previous 128-byte staging buffer
             * silently truncated any sha512: hash (135 chars: 7-char
             * prefix + 128 hex digits). */
            n_src_cksums = (int)parse_array(val, fp, &st,
                            (char *)src_cksums, 200, LPM_MAX_SOURCES);
        } else if (!strcmp(key, "backup")) {
            /* array: backup = ("etc/foo.conf" "etc/bar.conf") */
            m->nbackup = (uint8_t)parse_array(val, fp, &st,
                         (char *)m->backup, LPM_PATH_MAX, LPM_MAX_BACKUP);
        } else if (!strcmp(key, "groups")) {
            /* array: groups = ("xlib" "x11") */
            int n = parse_array(val, fp, &st,
                                (char *)m->groups, LPM_NAME_MAX, LPM_MAX_DEPS);
            m->ngroups = (uint8_t)n;
        } /* ── array fields: ("a" "b" "c") ── */
        else if (!strcmp(key, "depends")) {
            m->ndepends = (uint8_t)parse_array(val, fp, &st,
                           (char *)m->depends, LPM_NAME_MAX, LPM_MAX_DEPS);
        } else if (!strcmp(key, "makedepends")) {
            m->nmakedepends = (uint8_t)parse_array(val, fp, &st,
                               (char *)m->makedepends, LPM_NAME_MAX, LPM_MAX_DEPS);
        } else if (!strcmp(key, "recommends")) {
            m->nrecommends = (uint8_t)parse_array(val, fp, &st,
                              (char *)m->recommends, LPM_NAME_MAX, LPM_MAX_DEPS);
        } else if (!strcmp(key, "conflicts")) {
            m->nconflicts = (uint8_t)parse_array(val, fp, &st,
                             (char *)m->conflicts, LPM_NAME_MAX, LPM_MAX_DEPS);
        } else if (!strcmp(key, "replaces")) {
            m->nreplaces = (uint8_t)parse_array(val, fp, &st,
                            (char *)m->replaces, LPM_NAME_MAX, LPM_MAX_DEPS);
        } else if (!strcmp(key, "provides")) {
            m->nprovides = (uint8_t)parse_array(val, fp, &st,
                            (char *)m->provides, LPM_NAME_MAX, LPM_MAX_DEPS);
        }
    }

    fclose(fp);

    if (!m->name[0] || !m->version[0]) return -1;

    /* ── combine sources[] + checksums[] into m->sources[] ─────────────
     * LPDF v1: every source needs a checksum entry (SKIP if genuinely
     * unwanted) — a missing entry is a spec violation, not something to
     * default silently. checksum_parse_unified() is the single source of
     * truth for what's valid; CKSUM_INVALID here fails the whole parse. */
    int nsrc = n_src_urls;
    if (n_src_cksums > nsrc) nsrc = n_src_cksums;
    if (nsrc > LPM_MAX_SOURCES) nsrc = LPM_MAX_SOURCES;

    for (int i = 0; i < nsrc; i++) {
        Source *s = &m->sources[i];
        strncpy(s->url, src_urls[i], LPM_URL_MAX - 1);

        const char *base = strrchr(s->url, '/');
        strncpy(s->filename, base ? base + 1 : s->url, LPM_NAME_MAX - 1);

        strncpy(s->checksum, src_cksums[i], sizeof(s->checksum) - 1);
        s->cksum_type = checksum_parse_unified(s->checksum, NULL, 0);
        if (s->cksum_type == CKSUM_INVALID) {
            fprintf(stderr,
                "error: %s: invalid checksum format for source[%d] (%s)\n"
                "  expected:\n"
                "    sha512:\n"
                "    sha256:\n"
                "    md5:\n"
                "    SKIP\n",
                pbfile, i, s->url[0] ? s->url : "?");
            return -1;
        }
    }
    m->nsources = nsrc;

    return 0;
}

/* ── Cache: write ────────────────────────────────────────────────────── */
static int meta_cache_write(const char *pkgname,
                            const MetaCacheEntry *e) {
    util_mkdirp(LPM_META_CACHE_DIR, 0755);

    char path[LPM_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s.meta",
             LPM_META_CACHE_DIR, pkgname);

    char tmp[LPM_PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, (int)getpid());

    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;

    size_t written = fwrite(e, sizeof(MetaCacheEntry), 1, f);
    fflush(f);
    fclose(f);

    if (written != 1) { unlink(tmp); return -1; }

    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}

/* ── Cache: read (returns 1 if valid hit, 0 if miss/stale) ──────────── */
static int meta_cache_read(const char *pkgname,
                           const char *pbfile,
                           MetaCacheEntry *e) {
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

    MetaCacheEntry tmp;
    size_t n = fread(&tmp, sizeof(MetaCacheEntry), 1, f);
    fclose(f);

    if (n != 1) return 0;
    if (tmp.magic   != LPM_META_MAGIC)   return 0;
    if (tmp.version != LPM_META_VERSION) return 0;

    /* extra mtime guard stored inside struct */
    if (tmp.pkgbuild_mtime != pb_st.st_mtime) return 0;

    memcpy(e, &tmp, sizeof(MetaCacheEntry));
    return 1;
}

/* ── Public API: pkgbuild_parse_fast ─────────────────────────────────── *
 * The one LPDF parser. Fills pkg directly — no intermediate struct, no  *
 * bash fallback: parse_pkgbuild_c() is a strict, deterministic C parser *
 * for a spec that's fully static/declarative by design (see its own    *
 * docstring), so a parse failure means the file is genuinely malformed,*
 * not that it needs a second parsing strategy. Returns 0 on success,   *
 * -1 on failure (missing pkgname/pkgver, or an invalid checksum —      *
 * parse_pkgbuild_c() prints the reason either way).                    */
int pkgbuild_parse_fast(const char *pbfile, Package *pkg) {
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

    MetaCacheEntry e;
    memset(&e, 0, sizeof(e));
    e.magic   = LPM_META_MAGIC;
    e.version = LPM_META_VERSION;
    e.pkgbuild_mtime = pb_st.st_mtime;

    /* 1. try cache hit first */
    if (meta_cache_read(pkgname, pbfile, &e)) {
        *pkg = e.pkg;
        return 0;
    }

    /* 2. cache miss: parse */
    if (parse_pkgbuild_c(pbfile, &e.pkg) != 0)
        return -1;

    /* 3. write cache for next time */
    meta_cache_write(pkgname, &e);
    DBG(2, "disk cache written: %s", pkgname);

    *pkg = e.pkg;
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
