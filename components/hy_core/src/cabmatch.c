#include <math.h>
#include <string.h>
#include "hy/cabinet.h"

#define STEP_MS      (1000 / HY_CAB_RATE_HZ)
#define GYRO_MASK_N  ((HY_CAB_GYRO_START_MS + STEP_MS - 1) / STEP_MS)   /* rows whose gyro is masked */
#define GYRO_SCALE   (1.0f / 50.0f)                                       /* per dps */
#define ACC_SCALE    (1.0f / 100.0f)                                      /* per mg */
#define LEN_MIN_DIV  2u   /* capture at least L / 2 long; no upper bound: open end absorbs later motion */
#define ENERGY_RATIO 3.0f
#define BAND_MIN     10

/* Scratch for hy_cab_dist: template and capture features, two DTW rows indexed by capture sample. */
static HY_EXT_BSS hy_cab_feat_t s_ft[HY_CAB_MAX_N];
static HY_EXT_BSS hy_cab_feat_t s_fc[HY_CAB_MAX_N];
static HY_EXT_BSS float s_row[2][HY_CAB_MAX_N];

static void scaled_features(const hy_cab_seq_t *q, hy_cab_feat_t *f)
{
    hy_cab_features(q, f);
    for (uint16_t i = 0; i < q->n; i++) {
        f[i].gv *= GYRO_SCALE;
        f[i].gh *= GYRO_SCALE;
        f[i].av *= ACC_SCALE;
        f[i].ah *= ACC_SCALE;
    }
}

static float energy(const hy_cab_feat_t *f, bool gyro)
{
    float e = fabsf(f->av) + fabsf(f->ah);
    if (gyro) e += fabsf(f->gv) + fabsf(f->gh);
    return e;
}

static float cell_cost(const hy_cab_feat_t *a, const hy_cab_feat_t *b, bool gyro)
{
    float c = fabsf(a->av - b->av) + fabsf(a->ah - b->ah);
    if (gyro) c += fabsf(a->gv - b->gv) + fabsf(a->gh - b->gh);
    return c;
}

static float fmin2(float a, float b)
{
    return b < a ? b : a;
}

float hy_cab_dist(const hy_cab_seq_t *tmpl, const hy_cab_seq_t *cap)
{
    const int L = tmpl->n, N = cap->n;
    if (L == 0 || N == 0 || L > HY_CAB_MAX_N || N > HY_CAB_MAX_N) return INFINITY;
    if (LEN_MIN_DIV * (unsigned)N < (unsigned)L) return INFINITY;
    const bool gyro = tmpl->has_gyro && cap->has_gyro;
    scaled_features(tmpl, s_ft);
    scaled_features(cap, s_fc);

    /* Masked gyro rows are already 0, so the energies need no mask. */
    const int span = N < L ? N : L;
    float et = 0.0f, ec = 0.0f;
    for (int i = 0; i < L; i++) et += energy(&s_ft[i], gyro);
    for (int j = 0; j < span; j++) ec += energy(&s_fc[j], gyro);
    et /= (float)L;
    ec /= (float)span;
    if (ec * ENERGY_RATIO < et || ec > ENERGY_RATIO * et) return INFINITY;

    const int r = L / 4 > BAND_MIN ? L / 4 : BAND_MIN;
    float *prev = s_row[0], *cur = s_row[1];
    int plo = 0, phi = -1;   /* band of the previous row; empty before row 0 */
    for (int i = 0; i < L; i++) {
        int lo = i - r < 0 ? 0 : i - r;
        int hi = i + HY_CAB_OPEN_BEGIN_N + r > N - 1 ? N - 1 : i + HY_CAB_OPEN_BEGIN_N + r;
        for (int j = lo; j <= hi; j++) {
            float best = INFINITY;
            if (i == 0) {
                if (j <= HY_CAB_OPEN_BEGIN_N) best = 0.0f;   /* open begin */
            } else {
                if (j - 1 >= plo && j - 1 <= phi) best = prev[j - 1];
                if (j >= plo && j <= phi) best = fmin2(best, prev[j]);
            }
            if (j > lo) best = fmin2(best, cur[j - 1]);
            bool g = gyro && i >= GYRO_MASK_N && j >= GYRO_MASK_N;
            cur[j] = best + cell_cost(&s_ft[i], &s_fc[j], g);
        }
        float *t = prev;
        prev = cur;
        cur = t;
        plo = lo;
        phi = hi;
    }
    float d = INFINITY;
    int jend = L / 2 < N ? L / 2 : N - 1;   /* open end, from L / 2 or the last capture sample if earlier */
    for (int j = (jend > plo ? jend : plo); j <= phi; j++) d = fmin2(d, prev[j]);
    return d / (float)L;
}

static bool any_hit(const hy_cab_seq_t *q, uint16_t thr)
{
    for (uint16_t i = 0; i < q->n; i++) {
        if (hy_cab_wom_hit_at(q, i, thr)) return true;
    }
    return false;
}

/* The peak as the trigger sees it: the smallest threshold at which no sample fires. Found by bisection on
   hy_cab_wom_hit_at, so it follows the trigger's comparison rule (the change over one sensor sample period). */
static uint16_t peak_mg(const hy_cab_seq_t *q)
{
    if (!any_hit(q, 0)) return 0;
    uint16_t lo = 0, hi = UINT16_MAX;   /* any_hit(lo) holds; any_hit(hi) does not (an int16 axis differs by less) */
    while (hi - lo > 1) {
        uint16_t mid = (uint16_t)(lo + (hi - lo) / 2);
        if (any_hit(q, mid)) lo = mid;
        else hi = mid;
    }
    return hi;
}

static void trim_to_trigger(hy_cab_seq_t *q, uint16_t trig)
{
    uint16_t k = 0;
    while (k < q->n && !hy_cab_wom_hit_at(q, k, trig)) k++;
    if (k == 0 || k == q->n) return;
    memmove(q->s, &q->s[k], (size_t)(q->n - k) * sizeof q->s[0]);
    q->n = (uint16_t)(q->n - k);
}

hy_err_t hy_cab_model_build(hy_cab_model_t *m, const hy_cab_seq_t takes[HY_CAB_TAKES], int *odd)
{
    memset(m, 0, sizeof *m);
    *odd = -1;
    uint16_t min_peak = UINT16_MAX;
    for (int k = 0; k < HY_CAB_TAKES; k++) {
        if (takes[k].n == 0 || takes[k].n > HY_CAB_MAX_N) {
            *odd = k;
            return HY_E_INVALID;
        }
        uint16_t p = peak_mg(&takes[k]);
        if (p < min_peak) min_peak = p;
    }
    uint16_t trig = (uint16_t)(min_peak / 2);
    if (trig < HY_CAB_TRIG_MIN_MG) trig = HY_CAB_TRIG_MIN_MG;
    if (trig > HY_CAB_TRIG_MAX_MG) trig = HY_CAB_TRIG_MAX_MG;
    m->trig_mg = trig;
    for (int k = 0; k < HY_CAB_TAKES; k++) {
        m->take[k] = takes[k];
        trim_to_trigger(&m->take[k], trig);
    }

    float s[HY_CAB_TAKES];
    for (int k = 0; k < HY_CAB_TAKES; k++) {
        s[k] = INFINITY;
        for (int j = 0; j < HY_CAB_TAKES; j++)
            if (j != k) s[k] = fmin2(s[k], hy_cab_dist(&m->take[j], &m->take[k]));
    }
    int worst = 0, outlier = -1;
    for (int k = 0; k < HY_CAB_TAKES; k++) {
        if (s[k] > s[worst]) worst = k;
        float others = 0.0f;
        for (int j = 0; j < HY_CAB_TAKES; j++)
            if (j != k) others += s[j];
        float med = others / (HY_CAB_TAKES - 1);   /* the median of two values is their mean */
        if (s[k] > HY_CAB_ODD_RATIO * med && s[k] > HY_CAB_THR_MIN && (outlier < 0 || s[k] > s[outlier]))
            outlier = k;
    }
    m->d_self = s[worst];   /* filled on HY_E_INVALID too, so the takes can be accepted as they are */
    if (outlier >= 0 || !isfinite(s[worst])) {
        *odd = outlier >= 0 ? outlier : worst;
        return HY_E_INVALID;
    }
    m->valid = true;
    return HY_OK;
}

float hy_cab_threshold(const hy_cab_model_t *m, uint8_t sens)
{
    static const float k[5] = { 1.0f, 1.3f, 1.6f, 2.0f, 2.5f };
    if (sens < 1 || sens > 5) sens = 3;
    float t = k[sens - 1] * m->d_self;
    if (!(t >= HY_CAB_THR_MIN)) return HY_CAB_THR_MIN;
    return t > HY_CAB_THR_MAX ? HY_CAB_THR_MAX : t;
}

bool hy_cab_match(const hy_cab_model_t *m, uint8_t sens, const hy_cab_seq_t *cap, float *best)
{
    float d = INFINITY;
    if (m->valid)
        for (int k = 0; k < HY_CAB_TAKES; k++) d = fmin2(d, hy_cab_dist(&m->take[k], cap));
    if (best) *best = d;
    return m->valid && d <= hy_cab_threshold(m, sens);
}
