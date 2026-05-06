#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  LLPM_ERR_OK = 0,
  LLPM_ERR_INVAL,
  LLPM_ERR_OOM,
  LLPM_ERR_IO,
  LLPM_ERR_NOT_FOUND,
  LLPM_ERR_STATE,
  LLPM_ERR_INTERNAL,
  LLPM_ERR_LIMIT,
  LLPM_ERR_VERIFY,
  LLPM_ERR_NOSPC
} llpm_errno_t;

const char *llpm_strerror(llpm_errno_t err);

#ifdef __cplusplus
}
#endif
