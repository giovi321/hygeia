#include "hy/chime.h"

#include <math.h>

#define LEAD_MS   30
#define TONE_MS   180
#define GAP_MS    70
#define TAIL_MS   30
#define ATTACK_MS 8
#define DECAY_MS  120
#define PEAK      16384.0

static const double s_freq[3] = {880.0, 1109.0, 1319.0};

static size_t ms_to_samples(uint32_t ms, uint32_t rate)
{
    return (size_t)(((uint64_t)ms * rate + 500) / 1000);
}

size_t hy_chime_len(uint32_t rate)
{
    return ms_to_samples(LEAD_MS, rate) + 3 * ms_to_samples(TONE_MS, rate) +
           2 * ms_to_samples(GAP_MS, rate) + ms_to_samples(TAIL_MS, rate);
}

size_t hy_chime_render(int16_t *out, size_t max, uint32_t rate)
{
    size_t total = hy_chime_len(rate);
    if (!out || rate == 0 || max < total) {
        return 0;
    }
    for (size_t i = 0; i < total; i++) {
        out[i] = 0;
    }
    size_t tone_n = ms_to_samples(TONE_MS, rate);
    size_t attack_n = ms_to_samples(ATTACK_MS, rate);
    size_t decay_n = ms_to_samples(DECAY_MS, rate);
    size_t pos = ms_to_samples(LEAD_MS, rate);
    for (int t = 0; t < 3; t++) {
        for (size_t i = 0; i < tone_n; i++) {
            double env = 1.0;
            if (i < attack_n) {
                env = (double)i / (double)attack_n;
            }
            if (i >= tone_n - decay_n) {
                double d = (double)(tone_n - i) / (double)decay_n;
                if (d < env) {
                    env = d;
                }
            }
            double v = PEAK * env * sin(2.0 * M_PI * s_freq[t] * (double)i / (double)rate);
            out[pos + i] = (int16_t)lrint(v);
        }
        pos += tone_n + ms_to_samples(GAP_MS, rate);
    }
    return total;
}
