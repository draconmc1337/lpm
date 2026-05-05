#include "llpm/error.h"

const char *llpm_strerror(llpm_errno_t e) {
  switch (e) {
    case LLPM_ERR_OK: return "ok";
    case LLPM_ERR_INVAL: return "invalid argument";
    case LLPM_ERR_OOM: return "out of memory";
    case LLPM_ERR_IO: return "i/o error";
    case LLPM_ERR_NOT_FOUND: return "not found";
    case LLPM_ERR_STATE: return "invalid state";
    default: return "internal error";
  }
}
