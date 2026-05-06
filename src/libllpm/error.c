#include "llpm/error.h"

const char *llpm_strerror(llpm_errno_t err) {
  switch (err) {
    case LLPM_ERR_OK:
      return "ok";
    case LLPM_ERR_INVAL:
      return "invalid argument";
    case LLPM_ERR_OOM:
      return "out of memory";
    case LLPM_ERR_IO:
      return "i/o error";
    case LLPM_ERR_NOT_FOUND:
      return "not found";
    case LLPM_ERR_STATE:
      return "invalid state";
    case LLPM_ERR_LIMIT:
      return "limit reached";
    case LLPM_ERR_VERIFY:
      return "verification failed";
    case LLPM_ERR_NOSPC:
      return "no space left on device";
    case LLPM_ERR_INTERNAL:
    default:
      return "internal error";
  }
}
