#pragma once

#include "handle.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct llpm_trans {
  llpm_handle_t *h;
  int flags;
  int prepared;
  int committed;
} llpm_trans_t;

llpm_trans_t *llpm_trans_init(llpm_handle_t *h, int flags);
int llpm_trans_prepare(llpm_trans_t *t);
int llpm_trans_commit(llpm_trans_t *t);
int llpm_trans_release(llpm_trans_t *t);

#ifdef __cplusplus
}
#endif
