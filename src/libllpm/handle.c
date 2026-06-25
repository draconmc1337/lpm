#include "llpm/handle.h"

#include <stdlib.h>
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

void llpm_set_errno(llpm_handle_t *h, llpm_errno_t err) {
  if (h != NULL) {
    h->last_err = err;
  }
}

llpm_handle_t *llpm_initialize(const char *root, const char *dbpath, llpm_errno_t *err) {
  llpm_handle_t *h;

  if (root == NULL || dbpath == NULL) {
    if (err != NULL) {
      *err = LLPM_ERR_INVAL;
    }
    return NULL;
  }

  h = calloc(1U, sizeof(*h));
  if (h == NULL) {
    if (err != NULL) {
      *err = LLPM_ERR_OOM;
    }
    return NULL;
  }

  if (copy_bounded(h->root, sizeof(h->root), root) != 0 ||
      copy_bounded(h->dbpath, sizeof(h->dbpath), dbpath) != 0) {
    free(h);
    if (err != NULL) {
      *err = LLPM_ERR_INVAL;
    }
    return NULL;
  }

  h->last_err = LLPM_ERR_OK;
  if (err != NULL) {
    *err = LLPM_ERR_OK;
  }
  return h;
}

int llpm_release(llpm_handle_t *h) {
  if (h == NULL) {
    return -1;
  }

  free(h);
  return 0;
}

llpm_errno_t llpm_errno(const llpm_handle_t *h) {
  if (h == NULL) {
    return LLPM_ERR_INVAL;
  }

  return h->last_err;
}

/* ── llpm_handle_set_db_ops ─────────────────────────────────────────── *
 * Register db callbacks from the lpm binary into the handle.           *
 * Must be called before llpm_trans_prepare/commit for REMOVE ops.      */
void llpm_handle_set_db_ops(llpm_handle_t *h,
    int (*is_installed)(const char *),
    int (*files_remove)(const char *),
    void (*db_remove_fn)(const char *)) {
    if (!h) return;
    h->cb_is_installed = is_installed;
    h->cb_files_remove = files_remove;
    h->cb_remove       = db_remove_fn;
}
