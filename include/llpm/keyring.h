#pragma once

#include "handle.h"

#ifdef __cplusplus
extern "C" {
#endif

int llpm_keyring_verify_payload(llpm_handle_t *h, const char *signer, const char *payload);

#ifdef __cplusplus
}
#endif
