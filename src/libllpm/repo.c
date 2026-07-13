#include "llpm/repo.h"

#include <stdio.h>
#include <string.h>

static int copy_bounded(char *dst, size_t dst_size, const char *src) {
  size_t len;

  if (dst == NULL || src == NULL || dst_size == 0U) {
    return -1;
  }

  len = strlen(src);
  if (len >= dst_size) {
    return -1;
  }

  memcpy(dst, src, len + 1U);
  return 0;
}

int llpm_register_repo(llpm_handle_t *h, const char *name, const char *url) {
  llpm_repo_t *repo;

  if (h == NULL || name == NULL || url == NULL || name[0] == '\0' || url[0] == '\0') {
    llpm_set_errno(h, LLPM_ERR_INVAL);
    return -1;
  }

  if (h->nrepos >= LLPM_REPO_MAX) {
    llpm_set_errno(h, LLPM_ERR_LIMIT);
    return -1;
  }

  repo = &h->repos[h->nrepos];
  if (copy_bounded(repo->name, sizeof(repo->name), name) != 0 ||
      copy_bounded(repo->url, sizeof(repo->url), url) != 0) {
    llpm_set_errno(h, LLPM_ERR_INVAL);
    return -1;
  }

  h->nrepos++;
  llpm_set_errno(h, LLPM_ERR_OK);
  return 0;
}

int llpm_sync_databases(llpm_handle_t *h, int force) {
  size_t i;

  (void)force;

  if (h == NULL) {
    return -1;
  }

  for (i = 0U; i < h->nrepos; ++i) {
    printf(":: [libllpm] sync %s -> %s\n", h->repos[i].name, h->repos[i].url);
  }

  llpm_set_errno(h, LLPM_ERR_OK);
  return 0;
}

/* ── find_in_db ──────────────────────────────────────────────────────── *
 * Scans one repo.db file for an exact pkgname match (want_provides=0) or *
 * a provides= match (want_provides=1). Same on-disk format the main lpm *
 * binary's parse_repo_db() (sync.c) reads:                              *
 *   pkgname=VER-REL pkgtype=binary|source dlsize=N instsize=N           *
 *   desc=... provides=name1,name2  (desc/provides optional)             *
 *                                                                         *
 * libllpm.so cannot link against parse_repo_db() — it lives in the lpm  *
 * binary, not the library (see the cb_* callback comment in handle.h;   *
 * same reason dep.c keeps its own local version-compare instead of      *
 * linking util.c). This is that parser's one counterpart inside         *
 * libllpm's boundary — not a second implementation of the same one.     *
 * Returns 0 and fills *out on match, -1 on no match / I/O failure.      */
static int find_in_db(const char *path, const char *name,
                       int want_provides, llpm_pkg_t *out) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#') continue;

        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *pkgname = p;
        char *rest = eq + 1;

        char version[LLPM_VER_MAX];
        char *vend = rest;
        while (*vend && *vend != ' ' && *vend != '\t') vend++;
        size_t vlen = (size_t)(vend - rest);
        if (vlen >= sizeof(version)) vlen = sizeof(version) - 1;
        memcpy(version, rest, vlen);
        version[vlen] = '\0';

        if (!want_provides) {
            if (strcmp(pkgname, name) != 0) continue;
            if (copy_bounded(out->name, sizeof(out->name), pkgname) != 0 ||
                copy_bounded(out->version, sizeof(out->version), version) != 0)
                continue;
            fclose(f);
            return 0;
        }

        /* provides lookup: walk remaining key=val tokens for provides= */
        char *tok = vend;
        while (*tok) {
            while (*tok == ' ' || *tok == '\t') tok++;
            if (!*tok) break;
            char *feq = strchr(tok, '=');
            if (!feq) break;
            *feq = '\0';
            char *key = tok, *val = feq + 1;
            tok = val;
            while (*tok && *tok != ' ' && *tok != '\t') tok++;
            if (*tok) { *tok = '\0'; tok++; }

            if (strcmp(key, "provides") != 0) continue;

            char *pv = val;
            while (pv && *pv) {
                char *comma = strchr(pv, ',');
                if (comma) *comma = '\0';
                if (!strcmp(pv, name)) {
                    if (copy_bounded(out->name, sizeof(out->name), pkgname) == 0 &&
                        copy_bounded(out->version, sizeof(out->version), version) == 0) {
                        fclose(f);
                        return 0;
                    }
                }
                pv = comma ? comma + 1 : NULL;
            }
        }
    }
    fclose(f);
    return -1;
}

/* ── llpm_repo_find_pkg ──────────────────────────────────────────────── *
 * Looks up a package by exact name, then by provides=, across every repo
 * registered via llpm_register_repo() (reads the local
 * /var/lib/lpm/db/<repo>.db cache — no network I/O, same convention the
 * main binary uses).
 *
 * llpm_pkg_t.depends/conflicts come back empty: repo.db is a lightweight
 * index (name/version/type/size/desc/provides), not the full recipe —
 * dependency data lives in the PKGBUILD/LPDF file, which is main-binary
 * territory (pkgbuild_parse_fast(), lpm.h). A caller needing that should
 * go through the lpm binary, not this library call.
 *
 * Returns 0 and fills *out on success. On failure returns -1 and sets:
 *   LLPM_ERR_INVAL     — h/name/out missing or name is empty
 *   LLPM_ERR_NOT_FOUND — no repos registered, or no match by name/provides
 */
int llpm_repo_find_pkg(llpm_handle_t *h, const char *name, llpm_pkg_t *out) {
    if (h == NULL || name == NULL || name[0] == '\0' || out == NULL) {
        llpm_set_errno(h, LLPM_ERR_INVAL);
        return -1;
    }

    memset(out, 0, sizeof(*out));

    if (h->nrepos == 0) {
        llpm_set_errno(h, LLPM_ERR_NOT_FOUND);
        return -1;
    }

    /* pass 1: exact name match, across all registered repos */
    for (size_t i = 0; i < h->nrepos; i++) {
        char path[LLPM_PATH_MAX];
        snprintf(path, sizeof(path), "/var/lib/lpm/db/%s.db", h->repos[i].name);
        if (find_in_db(path, name, 0, out) == 0) {
            llpm_set_errno(h, LLPM_ERR_OK);
            return 0;
        }
    }

    /* pass 2: provides match — only after every repo's real names miss,
     * same precedence a real package always takes over a virtual one */
    for (size_t i = 0; i < h->nrepos; i++) {
        char path[LLPM_PATH_MAX];
        snprintf(path, sizeof(path), "/var/lib/lpm/db/%s.db", h->repos[i].name);
        if (find_in_db(path, name, 1, out) == 0) {
            llpm_set_errno(h, LLPM_ERR_OK);
            return 0;
        }
    }

    llpm_set_errno(h, LLPM_ERR_NOT_FOUND);
    return -1;
}
