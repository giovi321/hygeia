#ifndef HY_CABINET_FILE_H
#define HY_CABINET_FILE_H

#include "hy/cabinet.h"

/* The taught movement in <base>/cabinet.bin ("/data/cabinet.bin" on the device), format in hy/cabinet.h. The
   store must be open. Single-threaded like the rest of the store. */

/* Writes <file>.tmp, syncs it and renames it over the file, so a cut leaves the old file. HY_E_INVALID for a
   model hy_cab_encode refuses, HY_E_IO on a file error. */
hy_err_t hy_cab_file_save(const hy_cab_model_t *m, int64_t created);
/* HY_E_NOT_FOUND when absent; HY_E_CORRUPT or HY_E_VERSION as hy_cab_decode (the file is left in place);
   HY_E_IO on a read error. A leftover temp file is ignored. */
hy_err_t hy_cab_file_load(hy_cab_model_t *m, int64_t *created);
/* Removes the file and any leftover temp file; HY_OK when there was none. */
hy_err_t hy_cab_file_delete(void);

#endif
