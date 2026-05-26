#pragma once

#include <stddef.h>

#include "error.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LLPM_REPO_MAX 32
#define LLPM_NAME_MAX 64
#define LLPM_URL_MAX 512
#define LLPM_PATH_MAX 512

typedef struct {
  char name[LLPM_NAME_MAX];
  char url[LLPM_URL_MAX];
} llpm_repo_t;

/* Forward-declare key type so handle.h doesn't need keyring.h */
typedef struct llpm_key {
    char fingerprint[64];
    char uid[256];
    int  trust;    /* llpm_trust_t */
    long expires;
    int  revoked;
} llpm_key_t;

typedef struct llpm_handle {
  char root[LLPM_PATH_MAX];
  char dbpath[LLPM_PATH_MAX];
  llpm_repo_t repos[LLPM_REPO_MAX];
  size_t nrepos;
  llpm_errno_t last_err;
  char last_err_detail[256];
  /* keyring */
  llpm_key_t keys[64];
  int  nkeys;
  int  keyring_loaded;
} llpm_handle_t;

llpm_handle_t *llpm_initialize(const char *root, const char *dbpath, llpm_errno_t *err);
int llpm_release(llpm_handle_t *h);
llpm_errno_t llpm_errno(const llpm_handle_t *h);
void llpm_set_errno(llpm_handle_t *h, llpm_errno_t err);

#ifdef __cplusplus
}
#endif
