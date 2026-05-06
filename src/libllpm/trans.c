#include "llpm/trans.h"

#include <stdlib.h>

llpm_trans_t *llpm_trans_init(llpm_handle_t *h, int flags) {
  llpm_trans_t *t;

  if (h == NULL) {
    return NULL;
  }

  t = calloc(1U, sizeof(*t));
  if (t == NULL) {
    llpm_set_errno(h, LLPM_ERR_OOM);
    return NULL;
  }

  t->h = h;
  t->flags = flags;
  llpm_set_errno(h, LLPM_ERR_OK);
  return t;
}

int llpm_trans_prepare(llpm_trans_t *t) {
  if (t == NULL || t->h == NULL) {
    return -1;
  }

  t->prepared = 1;
  llpm_set_errno(t->h, LLPM_ERR_OK);
  return 0;
}

int llpm_trans_commit(llpm_trans_t *t) {
  if (t == NULL || t->h == NULL) {
    return -1;
  }

  if (t->prepared == 0) {
    llpm_set_errno(t->h, LLPM_ERR_STATE);
    return -1;
  }

  t->committed = 1;
  llpm_set_errno(t->h, LLPM_ERR_OK);
  return 0;
}

int llpm_trans_release(llpm_trans_t *t) {
  if (t == NULL) {
    return -1;
  }

  free(t);
  return 0;
}
