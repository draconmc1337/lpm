#pragma once

#include "handle.h"

#ifdef __cplusplus
extern "C" {
#endif

int llpm_register_repo(llpm_handle_t *h, const char *name, const char *url);
int llpm_sync_databases(llpm_handle_t *h, int force);

#ifdef __cplusplus
}
#endif
