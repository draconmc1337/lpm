#include "llpm/repo.h"
#include <stdio.h>
#include <string.h>

int llpm_register_repo(llpm_handle_t *h, const char *name, const char *url) {
  if (!h || !name || !url || !name[0] || !url[0]) return -1;
  if (h->nrepos >= LLPM_REPO_MAX) {
    h->last_err = LLPM_ERR_STATE;
    return -1;
  }

  llpm_repo_t *r = &h->repos[h->nrepos++];
  strncpy(r->name, name, sizeof(r->name) - 1);
  strncpy(r->url, url, sizeof(r->url) - 1);
  h->last_err = LLPM_ERR_OK;
  return 0;
}

int llpm_sync_databases(llpm_handle_t *h, int force) {
  (void)force;
  if (!h) return -1;
  for (int i = 0; i < h->nrepos; i++) {
    printf(":: [libllpm] sync %s -> %s\n", h->repos[i].name, h->repos[i].url);
  }
  h->last_err = LLPM_ERR_OK;
  return 0;
}
