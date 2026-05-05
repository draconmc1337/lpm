#include "llpm/trans.h"
#include <stdlib.h>

llpm_trans_t *llpm_trans_init(llpm_handle_t *h, int flags) {
  if (!h) return NULL;
  llpm_trans_t *t = calloc(1, sizeof(*t));
  if (!t) {
    h->last_err = LLPM_ERR_OOM;
    return NULL;
  }
  t->h = h;
  t->flags = flags;
  return t;
}

int llpm_trans_prepare(llpm_trans_t *t) {
  if (!t || !t->h) return -1;
  t->prepared = 1;
  t->h->last_err = LLPM_ERR_OK;
  return 0;
}

int llpm_trans_commit(llpm_trans_t *t) {
  if (!t || !t->h || !t->prepared) {
    if (t && t->h) t->h->last_err = LLPM_ERR_STATE;
    return -1;
  }
  t->committed = 1;
  t->h->last_err = LLPM_ERR_OK;
  return 0;
}

int llpm_trans_release(llpm_trans_t *t) {
  if (!t) return -1;
  free(t);
  return 0;
}
