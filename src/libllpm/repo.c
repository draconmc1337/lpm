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

/* ── llpm_repo_find_pkg ──────────────────────────────────────────────── *
 * Look up a package by name from the local PKGBUILD cache.              *
 * Tries the letter-bucket layout first (fast path), then flat layout.  *
 *                                                                        *
 * Returns 0 and fills *out on success, -1 if not found.                 */
int llpm_repo_find_pkg(llpm_handle_t *h, const char *name, llpm_pkg_t *out) {
    FILE *f = NULL;
    char pbfile[512];
    char line[1024];

    if (!h || !name || !out) return -1;

    /* ── candidate paths: letter-bucket first, then flat ─── */
    char letter = name[0];
    if (letter >= 'A' && letter <= 'Z') letter += 32;
    if (letter < 'a' || letter > 'z')  letter = '0';

    snprintf(pbfile, sizeof(pbfile),
             LLPM_PKGBUILD_DIR "/%c/pkgbuild_%s", letter, name);
    f = fopen(pbfile, "r");

    if (!f) {
        /* flat layout fallback */
        snprintf(pbfile, sizeof(pbfile),
                 LLPM_PKGBUILD_DIR "/pkgbuild_%s", name);
        f = fopen(pbfile, "r");
    }

    if (!f) return -1;

    memset(out, 0, sizeof(*out));
    strncpy(out->name, name, LLPM_NAME_MAX - 1);

    while (fgets(line, sizeof(line), f)) {
        /* strip newline */
        char *nl = line + strlen(line) - 1;
        while (nl >= line && (*nl == '\n' || *nl == '\r')) *nl-- = '\0';

        /* pkgver= */
        if (strncmp(line, "pkgver=", 7) == 0 && out->version[0] == '\0') {
            { const char *_vs = line + 7;
              size_t _vl = strlen(_vs);
              if (_vl >= LLPM_VER_MAX) _vl = LLPM_VER_MAX - 1;
              memcpy(out->version, _vs, _vl);
              out->version[_vl] = '\0'; }
            continue;
        }

        /* depends=(...) — single-line form only */
        if (strncmp(line, "depends=(", 9) == 0) {
            char *p = line + 9;
            char *end = strchr(p, ')');
            if (end) *end = '\0';
            /* tokenise space-separated entries (quoted or bare) */
            while (*p && out->ndepends < LLPM_DEP_MAX) {
                while (*p == ' ' || *p == '\t' || *p == '"' || *p == '\'') p++;
                if (!*p || *p == ')') break;
                char *tok = p;
                while (*p && *p != ' ' && *p != '\t' &&
                       *p != '"' && *p != '\'' && *p != ')') p++;
                size_t len = (size_t)(p - tok);
                if (len > 0 && len < LLPM_NAME_MAX) {
                    strncpy(out->depends[out->ndepends], tok, len);
                    out->depends[out->ndepends][len] = '\0';
                    out->ndepends++;
                }
            }
            continue;
        }

        /* conflicts=(...) — same pattern */
        if (strncmp(line, "conflicts=(", 11) == 0) {
            char *p = line + 11;
            char *end = strchr(p, ')');
            if (end) *end = '\0';
            while (*p && out->nconflicts < LLPM_DEP_MAX) {
                while (*p == ' ' || *p == '\t' || *p == '"' || *p == '\'') p++;
                if (!*p || *p == ')') break;
                char *tok = p;
                while (*p && *p != ' ' && *p != '\t' &&
                       *p != '"' && *p != '\'' && *p != ')') p++;
                size_t len = (size_t)(p - tok);
                if (len > 0 && len < LLPM_NAME_MAX) {
                    strncpy(out->conflicts[out->nconflicts], tok, len);
                    out->conflicts[out->nconflicts][len] = '\0';
                    out->nconflicts++;
                }
            }
            continue;
        }
    }

    fclose(f);
    return 0;
}
