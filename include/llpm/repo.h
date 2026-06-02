#pragma once

#include "handle.h"
#include "dep.h"

#ifdef __cplusplus
extern "C" {
#endif

int llpm_register_repo(llpm_handle_t *h, const char *name, const char *url);
int llpm_sync_databases(llpm_handle_t *h, int force);

/*
 * llpm_repo_find_pkg - look up a package by name (or provides alias) across
 * all registered repos.
 *
 * Returns 0 and fills *out on success.
 * Returns -1 and sets LLPM_ERR_NOT_FOUND when no repo contains the package.
 */
int llpm_repo_find_pkg(llpm_handle_t *h, const char *name, llpm_pkg_t *out);

#ifdef __cplusplus
}
#endif
