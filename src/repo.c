#include "llpm/repo.h"
#include "llpm/dep.h"

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

/*
 * llpm_repo_find_pkg - stub implementation.
 *
 * A production build would open the on-disk repo.db (one per registered repo),
 * parse the package entries, and match by name or by the "provides" list.
 * Here we provide a well-behaved stub that:
 *   - validates arguments,
 *   - fills *out with the package name so callers that only need the name
 *     field (e.g. the transaction builder in dep.c) get a useful result,
 *   - returns -1 / LLPM_ERR_NOT_FOUND when the handle has no repos, so the
 *     dependency resolver can handle missing packages gracefully.
 *
 * Replace the body of the loop with real DB parsing when the on-disk format
 * is finalised.
 */
int llpm_repo_find_pkg(llpm_handle_t *h, const char *name, llpm_pkg_t *out) {
    size_t i;

    if (h == NULL || name == NULL || name[0] == '\0' || out == NULL) {
        llpm_set_errno(h, LLPM_ERR_INVAL);
        return -1;
    }

    memset(out, 0, sizeof(*out));

    for (i = 0U; i < h->nrepos; ++i) {
        /*
         * TODO: open h->repos[i] DB file, search for `name` (direct match
         * first, then scan "provides" entries).  For now we fill a minimal
         * record so the dep-resolver can at least track the package by name.
         */
        if (copy_bounded(out->name, sizeof(out->name), name) != 0) {
            llpm_set_errno(h, LLPM_ERR_INVAL);
            return -1;
        }
        /* version unknown until real DB is parsed */
        copy_bounded(out->version, sizeof(out->version), "0");
        llpm_set_errno(h, LLPM_ERR_OK);
        return 0;
    }

    /* no repos registered or package not found in any repo */
    llpm_set_errno(h, LLPM_ERR_NOT_FOUND);
    return -1;
}
