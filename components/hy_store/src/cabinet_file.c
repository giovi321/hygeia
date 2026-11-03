#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include "hy/cabinet_file.h"
#include "hy/store.h"

#define FILE_REL "cabinet.bin"
#define TMP_REL  "cabinet.bin.tmp"

static HY_EXT_BSS uint8_t s_buf[HY_CAB_FILE_MAX];   /* single-threaded by contract */

hy_err_t hy_cab_file_save(const hy_cab_model_t *m, int64_t created)
{
    char p[160], tmp[160];
    if (hy_store_path(p, sizeof p, FILE_REL) != HY_OK || hy_store_path(tmp, sizeof tmp, TMP_REL) != HY_OK) return HY_E_IO;
    size_t n = hy_cab_encode(m, created, s_buf, sizeof s_buf);
    if (n == 0) return HY_E_INVALID;
    FILE *f = fopen(tmp, "wb");
    if (!f) return HY_E_IO;
    bool ok = fwrite(s_buf, 1, n, f) == n && fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (fclose(f) != 0) ok = false;
    if (ok && rename(tmp, p) != 0) ok = false;
    if (!ok) {
        unlink(tmp);
        return HY_E_IO;
    }
    return HY_OK;
}

hy_err_t hy_cab_file_load(hy_cab_model_t *m, int64_t *created)
{
    char p[160];
    if (hy_store_path(p, sizeof p, FILE_REL) != HY_OK) return HY_E_IO;
    FILE *f = fopen(p, "rb");
    if (!f) return errno == ENOENT ? HY_E_NOT_FOUND : HY_E_IO;
    size_t n = fread(s_buf, 1, sizeof s_buf, f);
    bool err = ferror(f) != 0;
    bool more = !err && fgetc(f) != EOF;   /* longer than any valid file */
    fclose(f);
    if (err) return HY_E_IO;
    if (more) return HY_E_CORRUPT;
    return hy_cab_decode(s_buf, n, m, created);
}

hy_err_t hy_cab_file_delete(void)
{
    char p[160], tmp[160];
    if (hy_store_path(p, sizeof p, FILE_REL) != HY_OK || hy_store_path(tmp, sizeof tmp, TMP_REL) != HY_OK) return HY_E_IO;
    unlink(tmp);
    if (unlink(p) != 0 && errno != ENOENT) return HY_E_IO;
    return HY_OK;
}
