#pragma once
#include <stddef.h>
#include "error.h"

#define LLPM_REPO_MAX 16

typedef struct {
  char name[32];
  char url[512];
} llpm_repo_t;

typedef struct llpm_handle {
  char root[256];
  char dbpath[256];
  llpm_repo_t repos[LLPM_REPO_MAX];
  int nrepos;
  llpm_errno_t last_err;
} llpm_handle_t;

llpm_handle_t *llpm_initialize(const char *root, const char *dbpath, llpm_errno_t *err);
int llpm_release(llpm_handle_t *h);
llpm_errno_t llpm_errno(const llpm_handle_t *h);
