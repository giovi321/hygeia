/*
 * stdio + POSIX file store. No ESP-IDF headers: the same code runs on LittleFS
 * mounted at "/data" on the device and in a temp directory on the host.
 */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "hy/codec.h"
#include "hy/store.h"

#define BASE_MAX    96
#define PATH_MAX_   160
#define LINE_MAX_   1024            /* longest accepted log line incl. '\n' */
#define MEDS_MAX_B  (256 * 1024)    /* larger meds.json is treated as corrupt */

static char s_base[BASE_MAX];
static bool s_open;
static uint64_t s_next_id = 1;   /* 64 bit: max id 0xFFFFFFFF must not wrap */
static size_t s_skipped;
static char s_line[LINE_MAX_];      /* single-threaded by contract */
static char s_wbuf[LINE_MAX_];

/* ---- paths ---- */

static bool make_path(char *out, const char *rel)
{
    int n = snprintf(out, PATH_MAX_, "%s/%s", s_base, rel);
    return n > 0 && n < PATH_MAX_;
}

/* ---- calendar: UTC month of an epoch second, as index = year * 12 + (month - 1) ---- */

#define TS_MIN (-62167219200LL)     /* 0000-01-01 00:00 UTC */
#define TS_MAX 253402300799LL       /* 9999-12-31 23:59:59 UTC */

static int month_index(int64_t ts)
{
    if (ts < TS_MIN) ts = TS_MIN;
    if (ts > TS_MAX) ts = TS_MAX;
    int64_t days = ts / 86400;
    if (ts % 86400 < 0) days--;
    /* civil_from_days (proleptic Gregorian), days since 1970-01-01 */
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int64_t m = mp < 10 ? mp + 3 : mp - 9;
    int64_t y = yoe + era * 400 + (m <= 2 ? 1 : 0);
    return (int)(y * 12 + (m - 1));
}

static bool ts_in_range(int64_t ts) { return ts >= TS_MIN && ts <= TS_MAX; }

static void month_name(int idx, char *out, size_t n)
{
    snprintf(out, n, "log/%04d-%02d.jsonl", idx / 12, idx % 12 + 1);
}

/* Parses "YYYY-MM.jsonl" exactly; returns the month index or -1. */
static int parse_month_name(const char *name)
{
    if (strlen(name) != 13 || strcmp(name + 7, ".jsonl") != 0 || name[4] != '-') return -1;
    for (int i = 0; i < 7; i++) {
        if (i != 4 && (name[i] < '0' || name[i] > '9')) return -1;
    }
    int y = (name[0] - '0') * 1000 + (name[1] - '0') * 100 + (name[2] - '0') * 10 + (name[3] - '0');
    int m = (name[5] - '0') * 10 + (name[6] - '0');
    if (m < 1 || m > 12) return -1;
    return y * 12 + (m - 1);
}

/* ---- month index: sorted month indexes of the existing log files, built once at open ---- */

#define MONTHS_MAX 1200

static int s_months[MONTHS_MAX];
static size_t s_n_months;

/* First position whose month index is >= idx. */
static size_t months_lower(int idx)
{
    size_t lo = 0, hi = s_n_months;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (s_months[mid] < idx) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

static bool months_has(int idx)
{
    size_t p = months_lower(idx);
    return p < s_n_months && s_months[p] == idx;
}

/* Inserts keeping order; false when full. A present index is a no-op. */
static bool months_add(int idx)
{
    size_t p = months_lower(idx);
    if (p < s_n_months && s_months[p] == idx) return true;
    if (s_n_months >= MONTHS_MAX) return false;
    memmove(&s_months[p + 1], &s_months[p], (s_n_months - p) * sizeof s_months[0]);
    s_months[p] = idx;
    s_n_months++;
    return true;
}

/* Lists the directory once. HY_E_FULL when there are more than MONTHS_MAX month files. */
static hy_err_t months_load(void)
{
    s_n_months = 0;
    char p[PATH_MAX_];
    if (!make_path(p, "log")) return HY_E_IO;
    DIR *d = opendir(p);
    if (!d) return HY_E_IO;
    hy_err_t r = HY_OK;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        int idx = parse_month_name(de->d_name);
        if (idx >= 0 && !months_add(idx)) {
            r = HY_E_FULL;
            break;
        }
    }
    closedir(d);
    return r;
}

/* ---- line reading ---- */

enum { LINE_EOF = 0, LINE_OK, LINE_BAD };

/*
 * Reads one line into s_line without the trailing newline. LINE_BAD means the
 * line did not fit and the rest of it was consumed. A last line without '\n'
 * is returned as LINE_OK (the decoder rejects it if truncated).
 */
static int read_line(FILE *f)
{
    if (!fgets(s_line, sizeof s_line, f)) return LINE_EOF;
    size_t n = strlen(s_line);
    if (n > 0 && s_line[n - 1] == '\n') {
        s_line[n - 1] = '\0';
        return LINE_OK;
    }
    if (feof(f)) return LINE_OK;
    int c;
    while ((c = fgetc(f)) != EOF && c != '\n') {
    }
    return LINE_BAD;
}

static bool blank(const char *s)
{
    for (; *s; s++) {
        if (*s != ' ' && *s != '\t' && *s != '\r') return false;
    }
    return true;
}

/*
 * Iterates the entries of one month file in file order. For each valid line
 * `cb` gets the entry and the raw line text; corrupt lines are counted. cb
 * returns false to stop. Returns false if the file cannot be opened.
 */
typedef bool (*entry_cb)(const hy_log_t *e, const char *raw, void *arg);

static bool scan_file(const char *path, entry_cb cb, void *arg)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    for (;;) {
        int r = read_line(f);
        if (r == LINE_EOF) break;
        if (r == LINE_BAD) {
            s_skipped++;
            continue;
        }
        if (blank(s_line)) continue;
        hy_log_t e;
        if (hy_log_from_jsonl(s_line, &e) != HY_OK) {
            s_skipped++;
            continue;
        }
        if (!cb(&e, s_line, arg)) break;
    }
    fclose(f);
    return true;
}

/* ---- durable writes ---- */

static bool sync_close(FILE *f)
{
    bool ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (fclose(f) != 0) ok = false;
    return ok;
}

/* Appends one line (with '\n'); starts a fresh line first if the file ends mid-line. */
static hy_err_t append_line(const char *path, const char *line)
{
    FILE *f = fopen(path, "a+b");
    if (!f) return HY_E_IO;
    bool lead_nl = false;
    if (fseek(f, 0, SEEK_END) == 0) {
        long sz = ftell(f);
        if (sz > 0 && fseek(f, -1, SEEK_END) == 0) {
            lead_nl = fgetc(f) != '\n';
            fseek(f, 0, SEEK_END);
        }
    }
    size_t n = strlen(line);
    if (n + 3 > sizeof s_wbuf) {
        fclose(f);
        return HY_E_INVALID;
    }
    size_t off = 0;
    if (lead_nl) s_wbuf[off++] = '\n';
    memcpy(s_wbuf + off, line, n);
    off += n;
    s_wbuf[off++] = '\n';
    bool ok = fwrite(s_wbuf, 1, off, f) == off;
    if (!sync_close(f)) ok = false;
    return ok ? HY_OK : HY_E_IO;
}

typedef enum { RW_UPDATE, RW_DELETE } rw_mode;

typedef struct {
    FILE *tmp;
    uint32_t id;
    rw_mode mode;
    const hy_log_t *upd;
    bool found, io_err;
} rw_ctx;

static bool rw_cb(const hy_log_t *e, const char *raw, void *arg)
{
    rw_ctx *c = arg;
    const char *line = raw;
    if (!c->found && e->id == c->id) {
        c->found = true;
        if (c->mode == RW_DELETE) return true;
        if (hy_log_to_jsonl(c->upd, s_wbuf, sizeof s_wbuf) < 0) {
            c->io_err = true;
            return false;
        }
        line = s_wbuf;
    }
    if (fputs(line, c->tmp) < 0 || fputc('\n', c->tmp) == EOF) {
        c->io_err = true;
        return false;
    }
    return true;
}

/*
 * Atomically rewrites one month file with the entry `id` replaced or removed.
 * Corrupt lines are dropped by the rewrite (and counted). HY_E_NOT_FOUND when
 * the file or the id does not exist; the file is then left untouched.
 */
static hy_err_t rewrite_month(const char *path, uint32_t id, rw_mode mode, const hy_log_t *upd)
{
    char tmp[PATH_MAX_ + 8];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) return HY_E_IO;
    FILE *probe = fopen(path, "rb");
    if (!probe) return errno == ENOENT ? HY_E_NOT_FOUND : HY_E_IO;
    fclose(probe);

    rw_ctx c = { fopen(tmp, "wb"), id, mode, upd, false, false };
    if (!c.tmp) return HY_E_IO;
    size_t skipped_before = s_skipped;
    bool opened = scan_file(path, rw_cb, &c);
    if (!opened || c.io_err || !c.found) {
        fclose(c.tmp);
        unlink(tmp);
        s_skipped = skipped_before;   /* nothing was rewritten: do not count twice */
        if (c.io_err || !opened) return HY_E_IO;
        return HY_E_NOT_FOUND;
    }
    if (!sync_close(c.tmp)) {
        unlink(tmp);
        return HY_E_IO;
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return HY_E_IO;
    }
    return HY_OK;
}

/* ---- presence / max id ---- */

typedef struct { uint32_t id; bool found; uint32_t max; } probe_ctx;

static bool probe_cb(const hy_log_t *e, const char *raw, void *arg)
{
    (void)raw;
    probe_ctx *c = arg;
    if (e->id > c->max) c->max = e->id;
    if (e->id == c->id) {
        c->found = true;
        return false;
    }
    return true;
}

/* ---- open ---- */

static void remove_tmp_in(const char *dir_path)
{
    DIR *d = opendir(dir_path);
    if (!d) return;
    struct dirent *de;
    char p[PATH_MAX_ + 64];
    while ((de = readdir(d)) != NULL) {
        size_t n = strlen(de->d_name);
        if (n > 4 && strcmp(de->d_name + n - 4, ".tmp") == 0) {
            if (snprintf(p, sizeof p, "%s/%s", dir_path, de->d_name) < (int)sizeof p) unlink(p);
        }
    }
    closedir(d);
}

hy_err_t hy_store_open(const char *base_dir)
{
    s_open = false;
    if (!base_dir || strlen(base_dir) >= BASE_MAX) return HY_E_INVALID;
    struct stat st;
    if (stat(base_dir, &st) != 0 || !S_ISDIR(st.st_mode)) return HY_E_IO;
    strcpy(s_base, base_dir);

    char p[PATH_MAX_];
    if (!make_path(p, "log")) return HY_E_IO;
    if (mkdir(p, 0775) != 0 && errno != EEXIST) return HY_E_IO;

    remove_tmp_in(s_base);
    remove_tmp_in(p);

    /* next id = max id over all month files + 1 (ids are global; entries can be
     * backdated into older months or moved between months) */
    uint32_t max = 0;
    hy_err_t lr = months_load();
    if (lr != HY_OK) return lr;
    for (size_t k = 0; k < s_n_months; k++) {
        char rel[32], fp[PATH_MAX_];
        month_name(s_months[k], rel, sizeof rel);
        if (!make_path(fp, rel)) return HY_E_IO;
        probe_ctx pc = { 0, false, 0 };
        scan_file(fp, probe_cb, &pc);
        if (pc.max > max) max = pc.max;
    }
    s_next_id = (uint64_t)max + 1;
    s_skipped = 0;
    s_open = true;
    return HY_OK;
}

size_t hy_store_skipped_lines(void) { return s_skipped; }

/* ---- medicines ---- */

static hy_err_t st_load_meds(void *ctx, hy_med_t *out, size_t max, size_t *n)
{
    (void)ctx;
    if (!s_open || !n) return HY_E_IO;
    char p[PATH_MAX_];
    if (!make_path(p, "meds.json")) return HY_E_IO;
    FILE *f = fopen(p, "rb");
    if (!f) {
        if (errno == ENOENT) {
            *n = 0;
            return HY_OK;
        }
        return HY_E_IO;
    }
    char *buf = NULL;
    hy_err_t r = HY_E_IO;
    if (fseek(f, 0, SEEK_END) == 0) {
        long sz = ftell(f);
        if (sz >= 0 && sz <= MEDS_MAX_B && fseek(f, 0, SEEK_SET) == 0) {
            buf = malloc((size_t)sz + 1);
            if (buf && fread(buf, 1, (size_t)sz, f) == (size_t)sz) {
                buf[sz] = '\0';
                hy_err_t d = hy_meds_from_json(buf, out, max, n);
                r = (d == HY_OK || d == HY_E_FULL) ? d : HY_E_IO;
            }
        }
    }
    free(buf);
    fclose(f);
    return r;
}

static hy_err_t st_save_meds(void *ctx, const hy_med_t *meds, size_t n)
{
    (void)ctx;
    if (!s_open) return HY_E_IO;
    char p[PATH_MAX_], tmp[PATH_MAX_ + 8];
    if (!make_path(p, "meds.json")) return HY_E_IO;
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    char *js = hy_meds_to_json(meds, n);
    if (!js) return HY_E_IO;
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        free(js);
        return HY_E_IO;
    }
    size_t len = strlen(js);
    bool ok = fwrite(js, 1, len, f) == len;
    free(js);
    if (!sync_close(f)) ok = false;
    if (ok && rename(tmp, p) != 0) ok = false;
    if (!ok) {
        unlink(tmp);
        return HY_E_IO;
    }
    return HY_OK;
}

/* ---- log ---- */

static hy_err_t st_append_log(void *ctx, hy_log_t *e)
{
    (void)ctx;
    if (!s_open || !e) return HY_E_IO;
    if (!ts_in_range(e->ts)) return HY_E_INVALID;
    if (s_next_id > 0xFFFFFFFEULL) return HY_E_FULL;
    hy_log_t tmp = *e;
    tmp.id = (uint32_t)s_next_id;
    char line[LINE_MAX_];
    if (hy_log_to_jsonl(&tmp, line, sizeof line) < 0) return HY_E_INVALID;
    char rel[32], p[PATH_MAX_];
    int idx = month_index(e->ts);
    /* A new month with a full index would be written but never seen: refuse before writing. */
    if (!months_has(idx) && s_n_months >= MONTHS_MAX) return HY_E_FULL;
    month_name(idx, rel, sizeof rel);
    if (!make_path(p, rel)) return HY_E_IO;
    hy_err_t r = append_line(p, line);
    if (r != HY_OK) return r;
    months_add(idx);   /* cannot fail: the index had room or already held this month */
    e->id = (uint32_t)s_next_id++;
    return HY_OK;
}

static hy_err_t st_update_log(void *ctx, const hy_log_t *e)
{
    (void)ctx;
    if (!s_open || !e) return HY_E_IO;
    if (!ts_in_range(e->ts)) return HY_E_INVALID;
    int target = month_index(e->ts);
    char rel[32], tp[PATH_MAX_];
    month_name(target, rel, sizeof rel);
    if (!make_path(tp, rel)) return HY_E_IO;

    hy_err_t r = months_has(target) ? rewrite_month(tp, e->id, RW_UPDATE, e) : HY_E_NOT_FOUND;
    if (r != HY_E_NOT_FOUND) return r;

    /* Not in the month of the new ts: look in the other month files (ruling R7). */
    for (size_t k = 0; k < s_n_months; k++) {
        int idx = s_months[k];
        if (idx == target) continue;
        char op[PATH_MAX_];
        month_name(idx, rel, sizeof rel);
        if (!make_path(op, rel)) return HY_E_IO;
        probe_ctx pc = { e->id, false, 0 };
        scan_file(op, probe_cb, &pc);
        if (!pc.found) continue;
        if (!months_has(target) && s_n_months >= MONTHS_MAX) return HY_E_FULL;   /* before any write */
        /* Write the new copy first: a crash leaves a duplicate, never a loss. */
        char line[LINE_MAX_];
        if (hy_log_to_jsonl(e, line, sizeof line) < 0) return HY_E_INVALID;
        r = append_line(tp, line);
        if (r != HY_OK) return r;
        months_add(target);   /* room was checked above */
        r = rewrite_month(op, e->id, RW_DELETE, NULL);
        return r == HY_OK ? HY_OK : HY_E_IO;
    }
    return HY_E_NOT_FOUND;
}

static hy_err_t st_delete_log(void *ctx, uint32_t id, int64_t ts)
{
    (void)ctx;
    if (!s_open) return HY_E_IO;
    if (!ts_in_range(ts)) return HY_E_NOT_FOUND;
    char rel[32], p[PATH_MAX_];
    int idx = month_index(ts);
    if (!months_has(idx)) return HY_E_NOT_FOUND;
    month_name(idx, rel, sizeof rel);
    if (!make_path(p, rel)) return HY_E_IO;
    return rewrite_month(p, id, RW_DELETE, NULL);
}

typedef struct {
    int64_t from, to;
    uint32_t med_id;
    hy_log_t *out;
    size_t max, n;
} query_ctx;

static int cmp_log(const void *a, const void *b)
{
    const hy_log_t *x = a, *y = b;
    if (x->ts != y->ts) return x->ts < y->ts ? -1 : 1;
    if (x->id != y->id) return x->id < y->id ? -1 : 1;
    return 0;
}

static bool query_cb(const hy_log_t *e, const char *raw, void *arg)
{
    (void)raw;
    query_ctx *c = arg;
    if (e->ts < c->from || e->ts >= c->to) return true;
    if (c->med_id != 0 && e->med_id != c->med_id) return true;
    if (c->n < c->max) {
        c->out[c->n++] = *e;
        return true;
    }
    /* full: replace the latest kept entry if this one is earlier */
    size_t worst = 0;
    for (size_t i = 1; i < c->n; i++) {
        if (cmp_log(&c->out[i], &c->out[worst]) > 0) worst = i;
    }
    if (cmp_log(e, &c->out[worst]) < 0) c->out[worst] = *e;
    return true;
}

static size_t st_query_log(void *ctx, int64_t from, int64_t to, uint32_t med_id,
                           hy_log_t *out, size_t max)
{
    (void)ctx;
    if (!s_open || !out || max == 0 || to <= from) return 0;
    int first = month_index(from);
    int last = month_index(to - 1);
    query_ctx c = { from, to, med_id, out, max, 0 };
    for (size_t k = months_lower(first); k < s_n_months && s_months[k] <= last; k++) {
        int idx = s_months[k];
        char rel[32], p[PATH_MAX_];
        month_name(idx, rel, sizeof rel);
        if (!make_path(p, rel)) break;
        scan_file(p, query_cb, &c);
        if (c.n >= c.max) break;   /* later months only hold later entries */
    }
    qsort(out, c.n, sizeof *out, cmp_log);
    return c.n;
}

static const hy_store_port_t PORT = {
    .load_meds = st_load_meds,
    .save_meds = st_save_meds,
    .append_log = st_append_log,
    .update_log = st_update_log,
    .delete_log = st_delete_log,
    .query_log = st_query_log,
};

const hy_store_port_t *hy_store_port(void) { return &PORT; }
