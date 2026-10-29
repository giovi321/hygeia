#include "hy/battery.h"

#include <stdio.h>

void hy_batt_init(hy_batt_t *b, int32_t saved_rate_cph)
{
    *b = (hy_batt_t){0};
    b->pct = -1;
    b->low_armed = true;
    b->crit_armed = true;
    b->rate_cph = saved_rate_cph > 0 ? saved_rate_cph : 0;
}

/* reanchor: a battery swap (accepted rise) or a clock that moved back makes the old anchor
 * meaningless, so restart it at (now, shown) and take no sample. rate_cph is kept. */
static void estimate_step(hy_batt_t *b, int64_t now, bool rose)
{
    if (b->raw.vbus) {
        b->est_t0 = 0;
        return;
    }
    if (b->pct < 0)
        return;
    if (b->est_t0 == 0 || rose || now < b->est_t0) {
        b->est_t0 = now;
        b->est_p0 = b->pct;
        return;
    }
    if (now - b->est_t0 >= HY_BATT_EST_MIN_S && b->est_p0 - b->pct >= HY_BATT_EST_MIN_DROP) {
        int32_t sample = (int32_t)((int64_t)(b->est_p0 - b->pct) * 100 * 3600 / (now - b->est_t0));
        b->rate_cph = b->rate_cph == 0 ? sample : (b->rate_cph + sample) / 2;
        b->est_t0 = now;
        b->est_p0 = b->pct;
    }
}

uint32_t hy_batt_update(hy_batt_t *b, const hy_batt_raw_t *r, int64_t now)
{
    int8_t prev = b->pct;
    int8_t shown;
    bool rose = false;
    if (!r->present || r->pct < 0)
        shown = -1;
    else if (r->vbus || prev < 0)
        shown = r->pct;
    else if (r->pct >= prev + 3) {
        shown = r->pct; /* battery swapped */
        rose = true;
    } else
        shown = r->pct < prev ? r->pct : prev;

    uint32_t flags = 0;
    if (shown != prev || r->vbus != b->raw.vbus || r->chg != b->raw.chg || r->present != b->raw.present)
        flags |= HY_BATT_CHANGED;

    b->raw = *r;
    b->pct = shown;

    if (r->vbus || shown >= HY_BATT_LOW_REARM)
        b->low_armed = true;
    if (r->vbus || shown >= HY_BATT_CRIT_REARM)
        b->crit_armed = true;
    if (!r->vbus && shown >= 0) {
        if (b->low_armed && shown <= HY_BATT_LOW_PCT) {
            flags |= HY_BATT_EV_LOW;
            b->low_armed = false;
        }
        if (b->crit_armed && shown <= HY_BATT_CRIT_PCT) {
            flags |= HY_BATT_EV_CRIT;
            b->crit_armed = false;
        }
    }

    estimate_step(b, now, rose);
    return flags;
}

bool hy_batt_crit_confirm(hy_batt_crit_confirm_t *c, int64_t mono_s, int pct, int mv, bool usb)
{
    if (usb || pct < 0 || pct > HY_BATT_CRIT_PCT || mv <= 0 || mv >= HY_BATT_CRIT_MV) {
        *c = (hy_batt_crit_confirm_t){0};
        return false;
    }
    if (c->done) {
        return false;
    }
    if (!c->pending) {
        c->pending = true;
        c->t0 = mono_s;
        return false;
    }
    if (mono_s - c->t0 >= HY_BATT_CRIT_CONFIRM_S) {
        c->done = true;
        return true;
    }
    return false;
}

void hy_batt_text(const hy_batt_t *b, char *buf, size_t len, const char *unknown, bool *red, bool *bolt)
{
    if (b->pct < 0)
        snprintf(buf, len, "%s", unknown);
    else
        snprintf(buf, len, "%d%%", b->pct);
    if (red)
        *red = b->pct >= 0 && b->pct < HY_BATT_LOW_PCT;
    if (bolt)
        *bolt = b->raw.chg == HY_CHG_CHARGING;
}

bool hy_batt_estimate(const hy_batt_t *b, char *buf, size_t len)
{
    if (b->rate_cph <= 0 || b->raw.vbus || b->pct < 0)
        return false;
    int64_t left = (int64_t)b->pct * 100; /* hours = left / rate */
    int64_t rate = b->rate_cph;
    if (left >= 48 * rate)
        snprintf(buf, len, "~%d d", (int)((left + 12 * rate) / (24 * rate)));
    else if (left >= rate)
        snprintf(buf, len, "~%d h", (int)((left + rate / 2) / rate));
    else
        snprintf(buf, len, "<1 h");
    return true;
}
