#include "hy/types.h"

const char *hy_err_str(hy_err_t e)
{
    switch (e) {
    case HY_OK:             return "HY_OK";
    case HY_E_INVALID:      return "HY_E_INVALID";
    case HY_E_CODE:         return "HY_E_CODE";
    case HY_E_CODE_DUP:     return "HY_E_CODE_DUP";
    case HY_E_NAME:         return "HY_E_NAME";
    case HY_E_TIMES:        return "HY_E_TIMES";
    case HY_E_NOT_FOUND:    return "HY_E_NOT_FOUND";
    case HY_E_CLOCK:        return "HY_E_CLOCK";
    case HY_E_UNDO_EXPIRED: return "HY_E_UNDO_EXPIRED";
    case HY_E_IO:           return "HY_E_IO";
    case HY_E_FULL:         return "HY_E_FULL";
    case HY_W_TOO_SOON:     return "HY_W_TOO_SOON";
    case HY_E_INTERVAL:     return "HY_E_INTERVAL";
    case HY_E_DOSE:         return "HY_E_DOSE";
    }
    return "HY_E_?";
}
