#include "llpm/handle.h"
#include <stdlib.h>
#include <string.h>

llpm_handle_t *llpm_initialize(const char *root, const char *dbpath, llpm_errno_t *err) {
  if (!root || !dbpath) {
    if (err) *err = LLPM_ERR_INVAL;
    return NULL;
  }

  llpm_handle_t *h = calloc(1, sizeof(*h));
  if (!h) {
    if (err) *err = LLPM_ERR_OOM;
    return NULL;
  }

  strncpy(h->root, root, sizeof(h->root) - 1);
  strncpy(h->dbpath, dbpath, sizeof(h->dbpath) - 1);
  h->last_err = LLPM_ERR_OK;
  if (err) *err = LLPM_ERR_OK;
  return h;
}

int llpm_release(llpm_handle_t *h) {
  if (!h) return -1;
  free(h);
  return 0;
}

llpm_errno_t llpm_errno(const llpm_handle_t *h) {
  if (!h) return LLPM_ERR_INVAL;
  return h->last_err;
}
