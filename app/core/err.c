#include "core/err.h"

const char * safe_err_str(safe_err_t e)
{
    switch(e) {
        case SAFE_OK:          return "ok";
        case SAFE_ERR_FAIL:    return "fail";
        case SAFE_ERR_PARAM:   return "invalid param";
        case SAFE_ERR_NOMEM:   return "out of memory";
        case SAFE_ERR_NOENT:   return "not found";
        case SAFE_ERR_EXIST:   return "already exists";
        case SAFE_ERR_PERM:    return "permission denied";
        case SAFE_ERR_IO:      return "io error";
        case SAFE_ERR_LOCKED:  return "locked";
        case SAFE_ERR_EXPIRED: return "expired";
        case SAFE_ERR_BUSY:    return "busy";
        case SAFE_ERR_UNSUP:   return "unsupported";
        case SAFE_ERR_TIMEOUT: return "timeout";
        case SAFE_ERR_STATE:   return "bad state";
        default:               return "unknown";
    }
}
