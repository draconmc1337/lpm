#pragma once

#include "handle.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LLPM_KEY_MAX  64

typedef enum {
    LLPM_TRUST_UNKNOWN  = 0,
    LLPM_TRUST_MARGINAL = 1,
    LLPM_TRUST_FULL     = 2,
    LLPM_TRUST_ULTIMATE = 3,
} llpm_trust_t;

int llpm_keyring_load(llpm_handle_t *h);
int llpm_keyring_save(llpm_handle_t *h);
int llpm_keyring_recv(llpm_handle_t *h, const char *keyid);
int llpm_keyring_import(llpm_handle_t *h, const char *filepath);
int llpm_keyring_set_trust(llpm_handle_t *h, const char *fingerprint, llpm_trust_t level);
int llpm_keyring_verify(llpm_handle_t *h, const char *filepath, const char *sigpath, int sig_required);
int llpm_keyring_verify_payload(llpm_handle_t *h, const char *signer, const char *payload);
int llpm_keyring_delete(llpm_handle_t *h, const char *fingerprint);
const llpm_key_t *llpm_keyring_find(const llpm_handle_t *h, const char *fingerprint);
const llpm_key_t *llpm_keyring_list(const llpm_handle_t *h, int *out_n);

#ifdef __cplusplus
}
#endif
