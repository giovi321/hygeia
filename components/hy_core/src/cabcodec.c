#include <math.h>
#include <stdio.h>
#include <string.h>
#include "hy/cabinet.h"

#define HDR_LEN   32
#define CRC_LEN   4
#define VERSION   1
#define TAKE_HDR  8
#define SAMPLE_B  12

static const uint8_t MAGIC[4] = { 'H', 'Y', 'C', 'B' };

static uint32_t crc32_ieee(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t *p) { return get16(p) | ((uint32_t)get16(p + 2) << 16); }
static uint64_t get64(const uint8_t *p) { return get32(p) | ((uint64_t)get32(p + 4) << 32); }

size_t hy_cab_encode(const hy_cab_model_t *m, int64_t created, uint8_t *buf, size_t max)
{
    if (!m || !m->valid || !buf) return 0;
    size_t need = HDR_LEN + CRC_LEN;
    for (int t = 0; t < HY_CAB_TAKES; t++) {
        if (m->take[t].n == 0 || m->take[t].n > HY_CAB_MAX_N) return 0;
        if (m->take[t].has_gyro != m->take[0].has_gyro) return 0;   /* the file has one flag for all takes */
        need += TAKE_HDR + (size_t)m->take[t].n * SAMPLE_B;
    }
    if (need > max) return 0;

    memset(buf, 0, HDR_LEN);
    memcpy(buf, MAGIC, 4);
    buf[4] = VERSION;
    buf[5] = HY_CAB_RATE_HZ;
    buf[6] = HY_CAB_TAKES;
    buf[7] = m->take[0].has_gyro ? 1 : 0;
    put16(buf + 8, m->trig_mg);
    put16(buf + 10, HY_CAB_GYRO_START_MS);
    uint32_t d;
    memcpy(&d, &m->d_self, sizeof d);
    put32(buf + 12, d);
    put64(buf + 16, (uint64_t)created);
    /* bytes 24 to 31 reserved, zero */

    uint8_t *p = buf + HDR_LEN;
    for (int t = 0; t < HY_CAB_TAKES; t++) {
        const hy_cab_seq_t *q = &m->take[t];
        put16(p, q->n);
        put16(p + 2, (uint16_t)q->rest.x);
        put16(p + 4, (uint16_t)q->rest.y);
        put16(p + 6, (uint16_t)q->rest.z);
        p += TAKE_HDR;
        for (uint16_t i = 0; i < q->n; i++, p += SAMPLE_B) {
            for (int k = 0; k < 3; k++) {
                put16(p + 2 * k, (uint16_t)q->s[i].a[k]);
                put16(p + 6 + 2 * k, (uint16_t)q->s[i].g[k]);
            }
        }
    }
    put32(p, crc32_ieee(buf, (size_t)(p - buf)));
    return (size_t)(p - buf) + CRC_LEN;
}

hy_err_t hy_cab_decode(const uint8_t *buf, size_t len, hy_cab_model_t *m, int64_t *created)
{
    if (!m) return HY_E_INVALID;
    m->valid = false;
    if (!buf || len < HDR_LEN + CRC_LEN || memcmp(buf, MAGIC, 4) != 0) return HY_E_CORRUPT;
    if (buf[4] != VERSION) return HY_E_VERSION;
    if (get32(buf + len - CRC_LEN) != crc32_ieee(buf, len - CRC_LEN)) return HY_E_CORRUPT;
    if (buf[5] != HY_CAB_RATE_HZ || buf[6] != HY_CAB_TAKES) return HY_E_CORRUPT;

    /* Walk the takes against the length before writing anything. */
    size_t end = len - CRC_LEN, off = HDR_LEN;
    for (int t = 0; t < HY_CAB_TAKES; t++) {
        if (end - off < TAKE_HDR) return HY_E_CORRUPT;
        uint16_t n = get16(buf + off);
        if (n == 0 || n > HY_CAB_MAX_N || (end - off - TAKE_HDR) / SAMPLE_B < n) return HY_E_CORRUPT;
        off += TAKE_HDR + (size_t)n * SAMPLE_B;
    }
    if (off != end) return HY_E_CORRUPT;

    bool gyro = (buf[7] & 1) != 0;
    uint32_t d = get32(buf + 12);
    uint16_t trig = get16(buf + 8);
    float d_self;
    memcpy(&d_self, &d, sizeof d);
    if (trig < HY_CAB_TRIG_MIN_MG || trig > HY_CAB_TRIG_MAX_MG || !isfinite(d_self) || !(d_self > 0.0f))
        return HY_E_CORRUPT;
    memset(m, 0, sizeof *m);
    m->trig_mg = get16(buf + 8);
    memcpy(&m->d_self, &d, sizeof d);
    if (created) *created = (int64_t)get64(buf + 16);
    const uint8_t *p = buf + HDR_LEN;
    for (int t = 0; t < HY_CAB_TAKES; t++) {
        hy_cab_seq_t *q = &m->take[t];
        q->n = get16(p);
        q->has_gyro = gyro;
        q->rest.x = (int16_t)get16(p + 2);
        q->rest.y = (int16_t)get16(p + 4);
        q->rest.z = (int16_t)get16(p + 6);
        p += TAKE_HDR;
        for (uint16_t i = 0; i < q->n; i++, p += SAMPLE_B) {
            for (int k = 0; k < 3; k++) {
                q->s[i].a[k] = (int16_t)get16(p + 2 * k);
                q->s[i].g[k] = (int16_t)get16(p + 6 + 2 * k);
            }
        }
    }
    m->valid = true;
    return HY_OK;
}

size_t hy_cab_csv_rows(const hy_cab_seq_t *q, const char *kind, int take, char *buf, size_t max)
{
    if (!q || !kind || !buf || max == 0) return 0;
    size_t used = 0;
    for (uint16_t i = 0; i < q->n; i++) {
        int w = snprintf(buf + used, max - used, "%s,%d,%u,%d,%d,%d,%d,%d,%d\n", kind, take,
                         (unsigned)i * (1000u / HY_CAB_RATE_HZ), q->s[i].a[0], q->s[i].a[1], q->s[i].a[2],
                         q->s[i].g[0], q->s[i].g[1], q->s[i].g[2]);
        if (w < 0 || (size_t)w >= max - used) {
            buf[0] = '\0';
            return 0;
        }
        used += (size_t)w;
    }
    return used;
}
