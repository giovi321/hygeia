#ifndef CAB_SYNTH_H
#define CAB_SYNTH_H

/* Synthetic movement generators for the cabinet host tests (header only).

   A movement is described by a cab_syn_t and sampled at HY_CAB_RATE_HZ, sample i at i * 20 ms. Sampling is
   stateless: cab_syn_sample() returns sample i for any i, so a test can feed a stream of any length, or
   cab_syn_make() can fill a hy_cab_seq_t. Quantities are in the units of hy_imu6_t (mg, deci-dps) in the device
   frame; the rest reading p.rest is the gravity reference and its direction is the vertical.

   Kinds (nominal size at amp = 1.0 and dur = 1.0):
   - CAB_SYN_DOOR_OPEN   rotation about the vertical, half-sine rate of 120 dps over 0.8 s, with the centripetal
                         and tangential acceleration of a point 0.15 m from the hinge
   - CAB_SYN_DOOR_CLOSE  the same swing in the opposite sense (the sign of the rate and of the tangential
                         acceleration flips)
   - CAB_SYN_TILT        rotation about a horizontal axis by 40 degrees and back over 1.0 s; gravity turns in the
                         device frame and the gyro reports the rate
   - CAB_SYN_LIFT        vertical acceleration pulse of +300 mg then -300 mg over 0.6 s, no rotation

   amp scales the size (rate, angle, acceleration), dur scales the duration, start_ms is the still lead before the
   movement. noise adds a seeded uniform noise of +-10 mg on the accel and +-1 dps on the gyro on every sample,
   also outside the movement; the same seed gives the same noise. Samples before and after the movement read the
   rest vector. */

#include <math.h>
#include <string.h>
#include "hy/cabinet.h"

typedef enum { CAB_SYN_DOOR_OPEN = 0, CAB_SYN_DOOR_CLOSE, CAB_SYN_TILT, CAB_SYN_LIFT } cab_syn_kind_t;

typedef struct {
    cab_syn_kind_t kind;
    float amp, dur;
    uint32_t start_ms;
    bool noise;
    uint32_t seed;
    hy_accel_t rest;
    bool has_gyro;
} cab_syn_t;

#define CAB_SYN_STEP_MS (1000 / HY_CAB_RATE_HZ)

/* Nominal movement, no lead, no noise, rest (0, -1000, 0), gyro present. */
static inline cab_syn_t cab_syn_new(cab_syn_kind_t kind)
{
    return (cab_syn_t){ .kind = kind, .amp = 1.0f, .dur = 1.0f, .rest = { 0, -1000, 0 }, .has_gyro = true };
}

/* Nominal movement duration in ms for p, without the lead. */
static inline uint32_t cab_syn_move_ms(const cab_syn_t *p)
{
    static const float nominal_s[] = { 0.8f, 0.8f, 1.0f, 0.6f };
    return (uint32_t)lroundf(nominal_s[p->kind] * p->dur * 1000.0f);
}

typedef struct { double x, y, z; } cab_v3_t;

static inline cab_v3_t cab_v3(double x, double y, double z) { return (cab_v3_t){ x, y, z }; }
static inline cab_v3_t cab_v3_add(cab_v3_t a, cab_v3_t b) { return cab_v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline cab_v3_t cab_v3_mul(cab_v3_t a, double k) { return cab_v3(a.x * k, a.y * k, a.z * k); }
static inline double cab_v3_dot(cab_v3_t a, cab_v3_t b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline cab_v3_t cab_v3_cross(cab_v3_t a, cab_v3_t b)
{
    return cab_v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static inline cab_v3_t cab_v3_unit(cab_v3_t a)
{
    double n = sqrt(cab_v3_dot(a, a));
    return n > 0 ? cab_v3_mul(a, 1.0 / n) : a;
}

/* v turned by angle (radians, right-handed) about the unit vector k (Rodrigues). */
static inline cab_v3_t cab_v3_rot(cab_v3_t v, cab_v3_t k, double angle)
{
    double c = cos(angle), s = sin(angle);
    return cab_v3_add(cab_v3_add(cab_v3_mul(v, c), cab_v3_mul(cab_v3_cross(k, v), s)),
                      cab_v3_mul(k, cab_v3_dot(k, v) * (1.0 - c)));
}

static inline int16_t cab_syn_i16(double v)
{
    long r = lround(v);
    return (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
}

/* Uniform value in [-1, 1] from a seed, a sample index and a channel (splitmix64 style hash). */
static inline double cab_syn_rand(uint32_t seed, uint32_t i, uint32_t ch)
{
    uint64_t z = ((uint64_t)seed << 32) + (uint64_t)i * 8u + ch + 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return (double)(z >> 11) / 4503599627370496.0 - 1.0;   /* 2^52 */
}

/* A horizontal unit vector (perpendicular to the vertical u): the hinge radius direction and the tilt axis. */
static inline cab_v3_t cab_syn_horizontal(cab_v3_t u)
{
    cab_v3_t helper = fabs(u.x) < 0.9 ? cab_v3(1, 0, 0) : cab_v3(0, 0, 1);
    return cab_v3_unit(cab_v3_cross(u, helper));
}

/* Sample i of the movement p. */
static inline hy_imu6_t cab_syn_sample(const cab_syn_t *p, uint32_t i)
{
    const double pi = 3.14159265358979323846;
    const double mg_per_ms2 = 1000.0 / 9.80665;
    cab_v3_t rest = cab_v3(p->rest.x, p->rest.y, p->rest.z);
    cab_v3_t u = cab_v3_unit(rest);
    cab_v3_t e1 = cab_syn_horizontal(u);
    cab_v3_t a = rest, w = cab_v3(0, 0, 0);   /* mg, dps */

    double t = ((double)i * CAB_SYN_STEP_MS - (double)p->start_ms) / 1000.0;
    double span = cab_syn_move_ms(p) / 1000.0;
    if (t >= 0.0 && t <= span) {
        switch (p->kind) {
        case CAB_SYN_DOOR_OPEN:
        case CAB_SYN_DOOR_CLOSE: {
            const double radius = 0.15;
            double sign = p->kind == CAB_SYN_DOOR_OPEN ? 1.0 : -1.0;
            double peak = 120.0 * p->amp * pi / 180.0;               /* rad/s */
            double om = sign * peak * sin(pi * t / span);
            double al = sign * peak * (pi / span) * cos(pi * t / span);
            cab_v3_t e_t = cab_v3_cross(u, e1);
            a = cab_v3_add(rest, cab_v3_mul(cab_v3_add(cab_v3_mul(e1, -om * om * radius), cab_v3_mul(e_t, al * radius)),
                                            mg_per_ms2));
            w = cab_v3_mul(u, om * 180.0 / pi);
            break;
        }
        case CAB_SYN_TILT: {
            double phi_max = 40.0 * p->amp * pi / 180.0;
            double phi = phi_max * 0.5 * (1.0 - cos(2.0 * pi * t / span));
            double dphi = phi_max * (pi / span) * sin(2.0 * pi * t / span);
            a = cab_v3_rot(rest, e1, -phi);
            w = cab_v3_mul(e1, dphi * 180.0 / pi);
            break;
        }
        case CAB_SYN_LIFT:
            a = cab_v3_add(rest, cab_v3_mul(u, 300.0 * p->amp * sin(2.0 * pi * t / span)));
            break;
        }
    }
    if (p->noise) {
        a = cab_v3_add(a, cab_v3(10.0 * cab_syn_rand(p->seed, i, 0), 10.0 * cab_syn_rand(p->seed, i, 1),
                                 10.0 * cab_syn_rand(p->seed, i, 2)));
        w = cab_v3_add(w, cab_v3(cab_syn_rand(p->seed, i, 3), cab_syn_rand(p->seed, i, 4),
                                 cab_syn_rand(p->seed, i, 5)));
    }
    hy_imu6_t s = { { cab_syn_i16(a.x), cab_syn_i16(a.y), cab_syn_i16(a.z) },
                    { cab_syn_i16(w.x * 10.0), cab_syn_i16(w.y * 10.0), cab_syn_i16(w.z * 10.0) } };
    if (!p->has_gyro) memset(s.g, 0, sizeof s.g);
    return s;
}

/* Fills q with the first total_ms of the movement (capped at HY_CAB_MAX_N samples); rest and has_gyro from p. */
static inline void cab_syn_make(const cab_syn_t *p, uint32_t total_ms, hy_cab_seq_t *q)
{
    uint32_t n = total_ms / CAB_SYN_STEP_MS;
    if (n > HY_CAB_MAX_N) n = HY_CAB_MAX_N;
    memset(q, 0, sizeof *q);
    q->rest = p->rest;
    q->has_gyro = p->has_gyro;
    q->n = (uint16_t)n;
    for (uint32_t i = 0; i < n; i++) q->s[i] = cab_syn_sample(p, i);
}

/* Turns every vector of q (accel, gyro and the rest reading) by deg degrees about the axis (x, y, z), as when
   the device is remounted or the screen rotates. Rounds to the integer units of hy_imu6_t. */
static inline void cab_syn_rotate(hy_cab_seq_t *q, double x, double y, double z, double deg)
{
    cab_v3_t k = cab_v3_unit(cab_v3(x, y, z));
    double ang = deg * 3.14159265358979323846 / 180.0;
    for (uint16_t i = 0; i < q->n; i++) {
        cab_v3_t a = cab_v3_rot(cab_v3(q->s[i].a[0], q->s[i].a[1], q->s[i].a[2]), k, ang);
        cab_v3_t w = cab_v3_rot(cab_v3(q->s[i].g[0], q->s[i].g[1], q->s[i].g[2]), k, ang);
        q->s[i] = (hy_imu6_t){ { cab_syn_i16(a.x), cab_syn_i16(a.y), cab_syn_i16(a.z) },
                               { cab_syn_i16(w.x), cab_syn_i16(w.y), cab_syn_i16(w.z) } };
    }
    cab_v3_t r = cab_v3_rot(cab_v3(q->rest.x, q->rest.y, q->rest.z), k, ang);
    q->rest = (hy_accel_t){ cab_syn_i16(r.x), cab_syn_i16(r.y), cab_syn_i16(r.z) };
}

#endif
