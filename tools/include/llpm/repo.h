#pragma once

#include "handle.h"
#include "dep.h"

#ifdef __cplusplus
extern "C" {
#endif

int llpm_register_repo(llpm_handle_t *h, const char *name, const char *url);
int llpm_sync_databases(llpm_handle_t *h, int force);
int llpm_repo_find_pkg(llpm_handle_t *h, const char *name, llpm_pkg_t *out);

#ifdef __cplusplus
}
#endif
