/* Persistent event outbox. See hy/outbox.h.
 *
 * Files in <dir>: segments "<first seq as 8 lowercase hex>.q" (append-only, at most HY_OB_SEG records) and
 * "state.json". One record per line:
 *   <seq>\t<at>\t<mask>\t<crc32 as 8 hex>\t<payload>\n
 * The CRC-32 covers "<seq>\t<at>\t<mask>\t" followed by the payload bytes (the CRC field itself is left out).
 */
#include "hy/outbox.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#define HY_OB_MALLOC(n) heap_caps_malloc((n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#else
#define HY_OB_MALLOC(n) malloc(n)
#endif
#define HY_OB_FREE(p) free(p)

#define OB_MAXSEG 64
#define OB_DIR_MAX 96
#define OB_PATH_MAX (OB_DIR_MAX + 16)
#define OB_NOSEQ 0xFFFFFFFFu
#define OB_LINEBUF (HY_OB_LINE_MAX + 2)
#define OB_ALLMASK ((1u << HY_OB_DESTS) - 1u)

typedef struct { uint32_t seq; uint8_t mask; int64_t at; } ob_meta_t;
typedef struct { uint32_t first; uint32_t count; } ob_seg_t;

struct hy_ob {
    char dir[OB_DIR_MAX];       /* empty: RAM backend */
    int64_t (*mono)(void);
    int64_t last_write;
    uint32_t tail;              /* the seq the next append takes */
    uint32_t next[HY_OB_DESTS];
    uint32_t dropped, expired, rejected, corrupt;
    uint32_t appends, state_writes;
    uint32_t disk_tail;         /* tail held by the state file on disk */
    bool dirty;
    bool seal;                  /* the last segment ends in a torn line: the next append starts a new one */
    bool del_fail;              /* a segment could not be unlinked: retry at the next state write */
    char *line;                 /* OB_LINEBUF bytes */
    char *arena;                /* RAM backend: HY_OB_CAP slots of HY_OB_LINE_MAX bytes */
    size_t nseg;
    ob_seg_t segs[OB_MAXSEG];
    ob_meta_t meta[HY_OB_CAP];  /* indexed by seq % HY_OB_CAP */
};

/* ---- CRC-32 (IEEE 802.3, reflected) ---- */

static uint32_t crc_update(uint32_t c, const void *data, size_t n)
{
    static const uint32_t tab[16] = {
        0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
        0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu, 0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu};
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        c = (c >> 4) ^ tab[c & 15u];
        c = (c >> 4) ^ tab[c & 15u];
    }
    return c;
}

/* ---- helpers ---- */

static int64_t now_s(const hy_ob_t *ob) { return ob->mono ? ob->mono() : 0; }

static void seg_path(const hy_ob_t *ob, char *out, size_t n, uint32_t first, const char *ext)
{
    snprintf(out, n, "%s/%08x.%s", ob->dir, (unsigned)first, ext);
}

/* First pending seq for dest d; tail when none. Pure. */
static uint32_t first_pending(const hy_ob_t *ob, unsigned d)
{
    uint32_t s = ob->next[d];
    if (s > ob->tail) s = ob->tail;
    if (ob->tail - s > HY_OB_CAP) s = ob->tail - HY_OB_CAP;
    while (s != ob->tail) {
        const ob_meta_t *m = &ob->meta[s % HY_OB_CAP];
        if (m->seq == s && ((m->mask >> d) & 1u)) break;
        s++;
    }
    return s;
}

/* Same, and moves next[d] up to it (the records skipped are done for d either way). */
static uint32_t fp(hy_ob_t *ob, unsigned d)
{
    uint32_t s = first_pending(ob, d);
    ob->next[d] = s;
    return s;
}

static uint32_t low_seq(hy_ob_t *ob)
{
    uint32_t low = ob->tail;
    for (unsigned d = 0; d < HY_OB_DESTS; d++) {
        uint32_t s = fp(ob, d);
        if (s < low) low = s;
    }
    return low;
}

/* ---- line format ---- */

static bool dec(const char **pp, const char *end, uint64_t *v, size_t maxdig)
{
    const char *p = *pp;
    uint64_t x = 0;
    size_t nd = 0;
    while (p < end && *p >= '0' && *p <= '9') {
        if (++nd > maxdig) return false;
        x = x * 10 + (uint64_t)(*p - '0');
        p++;
    }
    if (nd == 0 || p >= end || *p != '\t') return false;
    *v = x;
    *pp = p + 1;
    return true;
}

static bool payload_ok(const char *p, size_t n)
{
    if (n == 0 || n > HY_HOOK_PAYLOAD_MAX) return false;
    for (size_t i = 0; i < n; i++)
        if (p[i] == '\t' || p[i] == '\r' || p[i] == '\n' || p[i] == '\0') return false;
    return true;
}

/* Parses and verifies one line (without its newline). */
static bool parse_line(const char *buf, size_t n, uint32_t *seq, int64_t *at, uint8_t *mask, const char **pl, size_t *pn)
{
    const char *p = buf, *end = buf + n;
    uint64_t a, b, c;
    if (!dec(&p, end, &a, 10) || a >= OB_NOSEQ) return false;
    if (!dec(&p, end, &b, 18)) return false;
    if (!dec(&p, end, &c, 3) || c == 0 || c > OB_ALLMASK) return false;
    const char *crcpos = p;
    if (end - p < 9) return false;
    uint32_t crc = 0;
    for (int i = 0; i < 8; i++) {
        char ch = p[i];
        unsigned v;
        if (ch >= '0' && ch <= '9') v = (unsigned)(ch - '0');
        else if (ch >= 'a' && ch <= 'f') v = (unsigned)(ch - 'a') + 10u;
        else return false;
        crc = (crc << 4) | v;
    }
    if (p[8] != '\t') return false;
    p += 9;
    size_t len = (size_t)(end - p);
    if (!payload_ok(p, len)) return false;
    uint32_t want = crc_update(0xFFFFFFFFu, buf, (size_t)(crcpos - buf));
    want = ~crc_update(want, p, len);
    if (want != crc) return false;
    *seq = (uint32_t)a; *at = (int64_t)b; *mask = (uint8_t)c; *pl = p; *pn = len;
    return true;
}

/* Reads one line into buf (cap bytes, newline not stored, longer content dropped). Returns false at EOF with nothing
 * read. *n is the real length, *term whether a newline ended it. */
static bool rd_line(FILE *f, char *buf, size_t cap, size_t *n, bool *term)
{
    size_t len = 0;
    int ch;
    bool any = false;
    *term = false;
    while ((ch = getc(f)) != EOF) {
        any = true;
        if (ch == '\n') { *term = true; break; }
        if (len < cap) buf[len] = (char)ch;
        len++;
    }
    *n = len;
    return any;
}

/* ---- state file ---- */

static bool num_after(const char *s, const char *key, uint32_t *out)
{
    const char *p = strstr(s, key);
    if (!p) return false;
    p += strlen(key);
    if (*p < '0' || *p > '9') return false;
    unsigned long long v = 0;
    int nd = 0;
    while (*p >= '0' && *p <= '9') {
        if (++nd > 10) return false;
        v = v * 10 + (unsigned)(*p - '0');
        p++;
    }
    if (v > 0xFFFFFFFFull) return false;
    *out = (uint32_t)v;
    return true;
}

static void state_read(hy_ob_t *ob)
{
    static char buf[320] HY_EXT_BSS; /* only the owning task opens an outbox */
    char path[OB_PATH_MAX];
    snprintf(path, sizeof path, "%s/state.json", ob->dir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    uint32_t v, tail, nx[HY_OB_DESTS], dr, ex, rj;
    const char *p;
    if (!num_after(buf, "\"v\":", &v) || v != 1) return;
    if (!num_after(buf, "\"tail\":", &tail)) return;
    p = strstr(buf, "\"next\":[");
    if (!p) return;
    p += 8;
    for (int i = 0; i < HY_OB_DESTS; i++) {
        if (*p < '0' || *p > '9') return;
        unsigned long long x = 0;
        int nd = 0;
        while (*p >= '0' && *p <= '9') {
            if (++nd > 10) return;
            x = x * 10 + (unsigned)(*p - '0');
            p++;
        }
        if (x > 0xFFFFFFFFull) return;
        nx[i] = (uint32_t)x;
        if (*p == ',' || *p == ']') p++;
        else return;
    }
    if (!num_after(buf, "\"dropped\":", &dr) || !num_after(buf, "\"expired\":", &ex) ||
        !num_after(buf, "\"rejected\":", &rj))
        return;
    ob->tail = tail;
    ob->disk_tail = tail;
    for (int i = 0; i < HY_OB_DESTS; i++) ob->next[i] = nx[i];
    ob->dropped = dr; ob->expired = ex; ob->rejected = rj;
}

static bool state_write(hy_ob_t *ob)
{
    char tmp[OB_PATH_MAX], fin[OB_PATH_MAX], buf[200];
    snprintf(tmp, sizeof tmp, "%s/state.tmp", ob->dir);
    snprintf(fin, sizeof fin, "%s/state.json", ob->dir);
    int n = snprintf(buf, sizeof buf,
                     "{\"v\":1,\"tail\":%u,\"next\":[%u,%u,%u,%u,%u],\"dropped\":%u,\"expired\":%u,\"rejected\":%u}",
                     (unsigned)ob->tail, (unsigned)ob->next[0], (unsigned)ob->next[1], (unsigned)ob->next[2],
                     (unsigned)ob->next[3], (unsigned)ob->next[4], (unsigned)ob->dropped, (unsigned)ob->expired,
                     (unsigned)ob->rejected);
    if (n <= 0 || (size_t)n >= sizeof buf) return false;
    FILE *f = fopen(tmp, "w");
    if (!f) return false;
    bool ok = fwrite(buf, 1, (size_t)n, f) == (size_t)n && fflush(f) == 0 && fsync(fileno(f)) == 0;
    ok = (fclose(f) == 0) && ok;
    if (ok && rename(tmp, fin) != 0) ok = false;
    if (!ok) unlink(tmp);
    return ok;
}

/* ---- segments ---- */

/* Number of leading segments that can go: every record below low, and not the segment still being filled. */
static size_t ndeletable(hy_ob_t *ob)
{
    if (!ob->dir[0] || ob->nseg == 0) return 0;
    uint32_t low = low_seq(ob);
    size_t n = 0;
    for (size_t i = 0; i < ob->nseg; i++) {
        bool last = i + 1 == ob->nseg;
        if (last && ob->segs[i].count < HY_OB_SEG) break;
        if (ob->segs[i].count == 0) break;
        uint32_t lastseq = last ? ob->segs[i].first + ob->segs[i].count - 1 : ob->segs[i + 1].first - 1;
        if (lastseq >= low) break;
        n++;
    }
    return n;
}

static bool delete_segments(hy_ob_t *ob, size_t n)
{
    size_t gone = 0;
    for (size_t i = 0; i < n; i++) {
        char p[OB_PATH_MAX];
        seg_path(ob, p, sizeof p, ob->segs[i].first, "q");
        if (unlink(p) != 0 && errno != ENOENT) break;
        gone++;
    }
    if (gone) {
        memmove(ob->segs, ob->segs + gone, (ob->nseg - gone) * sizeof ob->segs[0]);
        ob->nseg -= gone;
    }
    return gone == n;
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static void meta_set(hy_ob_t *ob, uint32_t seq, uint8_t mask, int64_t at)
{
    ob_meta_t *m = &ob->meta[seq % HY_OB_CAP];
    m->seq = seq; m->mask = mask; m->at = at;
}

/* Copies the first keep bytes of the segment to a temp file and renames it over the segment. */
static bool seg_cut(hy_ob_t *ob, uint32_t first, long keep)
{
    char src[OB_PATH_MAX], tmp[OB_PATH_MAX];
    seg_path(ob, src, sizeof src, first, "q");
    seg_path(ob, tmp, sizeof tmp, first, "tmp");
    FILE *in = fopen(src, "r"), *out = fopen(tmp, "w");
    bool ok = in && out;
    long left = keep;
    while (ok && left > 0) {
        size_t want = left > (long)OB_LINEBUF ? (size_t)OB_LINEBUF : (size_t)left;
        size_t got = fread(ob->line, 1, want, in);
        if (got == 0 || fwrite(ob->line, 1, got, out) != got) ok = false;
        left -= (long)got;
    }
    if (out) {
        ok = ok && fflush(out) == 0 && fsync(fileno(out)) == 0;
        ok = (fclose(out) == 0) && ok;
    }
    if (in) fclose(in);
    if (ok && rename(tmp, src) != 0) ok = false;
    if (!ok) unlink(tmp);
    return ok;
}

/* Loads one segment into the meta ring. For the last segment a torn or bad final line is cut. Returns the highest
 * good seq + 1 (0 when none). */
static uint32_t seg_load(hy_ob_t *ob, size_t idx, bool is_last, uint32_t *min_good)
{
    char path[OB_PATH_MAX];
    seg_path(ob, path, sizeof path, ob->segs[idx].first, "q");
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    uint32_t hi = 0;
    long pos = 0, line_start = 0;
    bool last_bad = false;
    size_t n;
    bool term;
    while (rd_line(f, ob->line, OB_LINEBUF, &n, &term)) {
        line_start = pos;
        pos += (long)n + (term ? 1 : 0);
        uint32_t seq; int64_t at; uint8_t mask; const char *pl; size_t pn;
        bool good = term && n < HY_OB_LINE_MAX && parse_line(ob->line, n, &seq, &at, &mask, &pl, &pn);
        last_bad = !good;
        if (!good) { ob->corrupt++; continue; }
        meta_set(ob, seq, mask, at);
        if (seq + 1 > hi) hi = seq + 1;
        if (seq < *min_good) *min_good = seq;
    }
    fclose(f);
    if (is_last && last_bad && !seg_cut(ob, ob->segs[idx].first, line_start)) ob->seal = true;
    return hi;
}

static hy_err_t scan_dir(hy_ob_t *ob)
{
    DIR *d = opendir(ob->dir);
    if (!d) return HY_E_IO;
    static uint32_t firsts[OB_MAXSEG * 2] HY_EXT_BSS;
    static char tpath[OB_DIR_MAX + 300] HY_EXT_BSS;
    size_t nf = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *nm = e->d_name;
        size_t len = strlen(nm);
        if (len > 4 && strcmp(nm + len - 4, ".tmp") == 0) {
            snprintf(tpath, sizeof tpath, "%s/%s", ob->dir, nm);
            unlink(tpath);
            continue;
        }
        if (len != 10 || strcmp(nm + 8, ".q") != 0) continue;
        uint32_t v = 0;
        bool ok = true;
        for (int i = 0; i < 8 && ok; i++) {
            char ch = nm[i];
            if (ch >= '0' && ch <= '9') v = (v << 4) | (unsigned)(ch - '0');
            else if (ch >= 'a' && ch <= 'f') v = (v << 4) | ((unsigned)(ch - 'a') + 10u);
            else ok = false;
        }
        if (ok && nf < sizeof firsts / sizeof firsts[0]) firsts[nf++] = v;
    }
    closedir(d);
    qsort(firsts, nf, sizeof firsts[0], cmp_u32);
    size_t skip = nf > OB_MAXSEG ? nf - OB_MAXSEG : 0; /* far below the live window: drop the oldest files */
    for (size_t i = 0; i < skip; i++) {
        char p[OB_PATH_MAX];
        seg_path(ob, p, sizeof p, firsts[i], "q");
        unlink(p);
    }
    for (size_t i = skip; i < nf; i++) {
        ob->segs[ob->nseg].first = firsts[i];
        ob->segs[ob->nseg].count = 0;
        ob->nseg++;
    }
    return HY_OK;
}

/* ---- public API ---- */

hy_err_t hy_ob_open(const char *dir, int64_t (*mono_s)(void), hy_ob_t **out)
{
    if (!out) return HY_E_INVALID;
    *out = NULL;
    if (dir && (dir[0] == '\0' || strlen(dir) >= OB_DIR_MAX)) return HY_E_IO;
    hy_ob_t *ob = (hy_ob_t *)HY_OB_MALLOC(sizeof *ob);
    if (!ob) return HY_E_FULL;
    memset(ob, 0, sizeof *ob);
    for (size_t i = 0; i < HY_OB_CAP; i++) ob->meta[i].seq = OB_NOSEQ;
    ob->mono = mono_s;
    ob->last_write = now_s(ob);
    ob->line = (char *)HY_OB_MALLOC(OB_LINEBUF);
    if (!ob->line) { HY_OB_FREE(ob); return HY_E_FULL; }
    if (!dir) {
        ob->arena = (char *)HY_OB_MALLOC((size_t)HY_OB_CAP * HY_OB_LINE_MAX);
        if (!ob->arena) { HY_OB_FREE(ob->line); HY_OB_FREE(ob); return HY_E_FULL; }
        ob->tail = 1; /* seq 0 is never an event: the test event carries it */
        *out = ob;
        return HY_OK;
    }
    strcpy(ob->dir, dir);
    if (mkdir(dir, 0775) != 0 && errno != EEXIST) goto io;
    if (scan_dir(ob) != HY_OK) goto io;
    state_read(ob);

    uint32_t hi = 0, min_good = OB_NOSEQ;
    for (size_t i = 0; i < ob->nseg; i++) {
        bool is_last = i + 1 == ob->nseg;
        uint32_t h = seg_load(ob, i, is_last, &min_good);
        if (h > hi) hi = h;
        if (is_last) ob->segs[i].count = h > ob->segs[i].first ? h - ob->segs[i].first : 0;
    }
    for (size_t i = 0; i + 1 < ob->nseg; i++) ob->segs[i].count = ob->segs[i + 1].first - ob->segs[i].first;
    if (hi > ob->tail) ob->tail = hi;
    if (ob->nseg && ob->segs[ob->nseg - 1].first > ob->tail) ob->tail = ob->segs[ob->nseg - 1].first;
    if (ob->tail == 0 && ob->nseg == 0) ob->tail = 1; /* a fresh store starts at 1: seq 0 is the test event's id */
    for (unsigned d = 0; d < HY_OB_DESTS; d++) {
        if (ob->next[d] > ob->tail) ob->next[d] = ob->tail;
        if (min_good != OB_NOSEQ && ob->next[d] < min_good) ob->next[d] = min_good;
        if (ob->tail - ob->next[d] > HY_OB_CAP) ob->next[d] = ob->tail - HY_OB_CAP;
    }
    *out = ob;
    return HY_OK;
io:
    HY_OB_FREE(ob->line);
    HY_OB_FREE(ob);
    return HY_E_IO;
}

void hy_ob_close(hy_ob_t *ob)
{
    if (!ob) return;
    (void)hy_ob_flush(ob, true);
    HY_OB_FREE(ob->arena);
    HY_OB_FREE(ob->line);
    HY_OB_FREE(ob);
}

uint32_t hy_ob_next_seq(const hy_ob_t *ob) { return ob ? ob->tail : 0; }

/* Appends the record to the current segment (a new one when needed). */
static hy_err_t file_append(hy_ob_t *ob, int64_t at, uint8_t mask, const char *payload, size_t plen)
{
    uint32_t seq = ob->tail;
    bool fresh = ob->seal || !(ob->nseg > 0 && ob->segs[ob->nseg - 1].count < HY_OB_SEG &&
                   ob->segs[ob->nseg - 1].first + ob->segs[ob->nseg - 1].count == seq);
    if (fresh && ob->nseg == OB_MAXSEG) {
        (void)hy_ob_flush(ob, true);
        if (ob->nseg == OB_MAXSEG) return HY_E_IO;
    }
    uint32_t first = fresh ? seq : ob->segs[ob->nseg - 1].first;
    char path[OB_PATH_MAX];
    seg_path(ob, path, sizeof path, first, "q");
    struct stat st;
    off_t old = stat(path, &st) == 0 ? st.st_size : 0;

    int hl = snprintf(ob->line, OB_LINEBUF, "%u\t%lld\t%u\t", (unsigned)seq, (long long)at, (unsigned)mask);
    if (hl <= 0 || (size_t)hl + 9 + plen + 1 > OB_LINEBUF) return HY_E_INVALID;
    uint32_t crc = ~crc_update(crc_update(0xFFFFFFFFu, ob->line, (size_t)hl), payload, plen);
    snprintf(ob->line + hl, 10, "%08x\t", (unsigned)crc);
    memcpy(ob->line + hl + 9, payload, plen);
    size_t total = (size_t)hl + 9 + plen;
    ob->line[total++] = '\n';

    /* A new segment under the name of a file that still holds a torn line starts on a fresh line. */
    bool lead = fresh && old > 0;
    FILE *f = fopen(path, "a");
    if (!f) return HY_E_IO;
    bool ok = (!lead || fputc('\n', f) != EOF) && fwrite(ob->line, 1, total, f) == total && fflush(f) == 0 && fsync(fileno(f)) == 0;
    ok = (fclose(f) == 0) && ok;
    if (!ok) {
        if (fresh && old == 0) unlink(path);
        else if (truncate(path, old) != 0) ob->seal = true; /* the torn line stays; never append to it */
        return HY_E_IO;
    }
    if (fresh) {
        if (ob->nseg > 0 && ob->segs[ob->nseg - 1].first == seq) {
            ob->segs[ob->nseg - 1].count = 1; /* the torn file's own entry */
        } else {
            ob->segs[ob->nseg].first = seq;
            ob->segs[ob->nseg].count = 1;
            ob->nseg++;
        }
        ob->seal = false;
    } else {
        ob->segs[ob->nseg - 1].count++;
    }
    return HY_OK;
}

hy_err_t hy_ob_append(hy_ob_t *ob, int64_t at, uint8_t mask, const char *payload, uint32_t *seq)
{
    if (!ob || !payload || at < 0 || mask == 0 || (mask & ~OB_ALLMASK)) return HY_E_INVALID;
    size_t plen = strlen(payload);
    if (!payload_ok(payload, plen)) return HY_E_INVALID;
    if (ob->tail >= OB_NOSEQ - 1) return HY_E_FULL;
    uint32_t s = ob->tail;

    if (ob->dir[0]) {
        hy_err_t r = file_append(ob, at, mask, payload, plen);
        if (r != HY_OK) return r;
    }
    /* Full: the oldest waiting record goes for every destination before the new one takes its slot. */
    uint32_t low = low_seq(ob);
    if (ob->tail - low >= HY_OB_CAP) {
        for (unsigned d = 0; d < HY_OB_DESTS; d++)
            if (ob->next[d] <= low) ob->next[d] = low + 1;
        ob->dropped++;
        ob->dirty = true;
    }
    if (!ob->dir[0]) memcpy(ob->arena + (size_t)(s % HY_OB_CAP) * HY_OB_LINE_MAX, payload, plen + 1);
    meta_set(ob, s, mask, at);
    ob->tail = s + 1;
    ob->appends++;
    if (seq) *seq = s;
    return HY_OK;
}

/* Reads and verifies the record with this seq from its segment file. */
/* 1 read, 0 not there or failing its CRC (a bad record), -1 the file cannot be opened or read (try again later). */
static int file_read(hy_ob_t *ob, uint32_t seq, hy_ob_rec_t *out)
{
    size_t i = ob->nseg;
    while (i > 0 && ob->segs[i - 1].first > seq) i--;
    if (i == 0) return 0;
    char path[OB_PATH_MAX];
    seg_path(ob, path, sizeof path, ob->segs[i - 1].first, "q");
    FILE *f = fopen(path, "r");
    if (!f) return errno == ENOENT ? 0 : -1; /* a segment that is gone loses its records; other errors retry */
    size_t n;
    bool term, found = false;
    while (!found && rd_line(f, ob->line, OB_LINEBUF, &n, &term)) {
        uint32_t s; int64_t at; uint8_t mask; const char *pl; size_t pn;
        if (!term || n >= HY_OB_LINE_MAX || !parse_line(ob->line, n, &s, &at, &mask, &pl, &pn) || s != seq) continue;
        out->seq = s; out->at = at; out->mask = mask;
        memcpy(out->payload, pl, pn);
        out->payload[pn] = '\0';
        found = true;
    }
    bool bad = ferror(f) != 0;
    fclose(f);
    if (found) return 1;
    return bad ? -1 : 0;
}

hy_err_t hy_ob_peek(hy_ob_t *ob, uint8_t dest, hy_ob_rec_t *out)
{
    if (!ob || !out || dest >= HY_OB_DESTS) return HY_E_INVALID;
    for (;;) {
        uint32_t s = fp(ob, dest);
        if (s == ob->tail) return HY_E_NOT_FOUND;
        const ob_meta_t *m = &ob->meta[s % HY_OB_CAP];
        if (!ob->dir[0]) {
            out->seq = s; out->at = m->at; out->mask = m->mask;
            strcpy(out->payload, ob->arena + (size_t)(s % HY_OB_CAP) * HY_OB_LINE_MAX);
            return HY_OK;
        }
        int r = file_read(ob, s, out);
        if (r > 0) return HY_OK;
        if (r < 0) return HY_E_IO;
        /* Unreadable record: skip it for this destination. */
        ob->next[dest] = s + 1;
        ob->corrupt++;
        ob->dirty = true;
    }
}

void hy_ob_done(hy_ob_t *ob, uint8_t dest, uint32_t seq, hy_ob_outcome_t how)
{
    if (!ob || dest >= HY_OB_DESTS) return;
    if (seq == ob->tail || fp(ob, dest) != seq) return;
    ob->next[dest] = seq + 1;
    if (how == HY_OBO_EXPIRED) ob->expired++;
    else if (how == HY_OBO_REJECTED) ob->rejected++;
    ob->dirty = true;
}

void hy_ob_drop_dest(hy_ob_t *ob, uint8_t dest)
{
    if (!ob || dest >= HY_OB_DESTS) return;
    if (ob->next[dest] != ob->tail) {
        ob->next[dest] = ob->tail;
        ob->dirty = true;
    }
}

bool hy_ob_flush(hy_ob_t *ob, bool force)
{
    if (!ob) return false;
    size_t del = ndeletable(ob);
    bool need = ob->dirty || (del > 0 && ob->disk_tail < ob->tail);
    bool wrote = false;
    bool forcedel = del > 0 && !ob->del_fail;
    if (need && (force || forcedel || now_s(ob) - ob->last_write >= HY_OB_FLUSH_S)) {
        bool ok = true;
        if (ob->dir[0]) ok = state_write(ob);
        ob->last_write = now_s(ob);
        if (!ok) return false;
        ob->dirty = false;
        ob->disk_tail = ob->tail;
        ob->state_writes++;
        wrote = true;
    }
    /* Segments go only once the state holding the current tail and cursors is on disk. */
    if (del > 0 && ob->disk_tail >= ob->tail && !ob->dirty && (wrote || !ob->del_fail))
        ob->del_fail = !delete_segments(ob, del);
    return wrote;
}

bool hy_ob_dirty(const hy_ob_t *ob)
{
    return ob && ob->dirty;
}

void hy_ob_stats(const hy_ob_t *ob, hy_ob_stats_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!ob) return;
    uint32_t low = ob->tail;
    for (unsigned d = 0; d < HY_OB_DESTS; d++) {
        uint32_t s = first_pending(ob, d);
        if (s < low) low = s;
        uint32_t c = 0;
        for (; s != ob->tail; s++) {
            const ob_meta_t *m = &ob->meta[s % HY_OB_CAP];
            if (m->seq == s && ((m->mask >> d) & 1u)) c++;
        }
        out->pending[d] = c;
    }
    out->live = ob->tail - low;
    out->dropped = ob->dropped; out->expired = ob->expired; out->rejected = ob->rejected; out->corrupt = ob->corrupt;
    out->tail = ob->tail;
    out->appends = ob->appends; out->state_writes = ob->state_writes;
    out->segments = (uint32_t)ob->nseg;
}
