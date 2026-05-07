#pragma once

#include "handle.h"

#ifdef __cplusplus
extern "C" {
#endif

int llpm_check_dependencies(llpm_handle_t *h, const char *const *pkgs, size_t count);

#ifdef __cplusplus
}
#endif
