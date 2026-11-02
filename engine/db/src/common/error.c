#include "error.h"

const char *err_name(int rc)
{
    switch (rc) {
    case OK:         return "OK";
    case E_NOSPACE:  return "E_NOSPACE";
    case E_CORRUPT:  return "E_CORRUPT";
    case E_IO:       return "E_IO";
    case E_NOTFOUND: return "E_NOTFOUND";
    case E_TXN:      return "E_TXN";
    case E_INVAL:    return "E_INVAL";
    case E_EOF:      return "E_EOF";
    case E_NOTSUP:   return "E_NOTSUP";
    default:         return "E_UNKNOWN";
    }
}
