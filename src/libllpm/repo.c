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
