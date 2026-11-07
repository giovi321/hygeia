#ifndef HY_OUTBOX_H
#define HY_OUTBOX_H

/* Persistent event outbox: append-only segment files plus a small state file, with per-destination cursors.
 * Delivery is at least once. Only one task may use a handle. */

#include <stdbool.h>
#include <stdint.h>
#include "hy/hook.h"
#include "hy/types.h"

#define HY_OB_CAP 1000
#define HY_OB_SEG 100
#define HY_OB_DESTS 5
#define HY_OB_LINE_MAX 1100
#define HY_OB_FLUSH_S 10

typedef struct hy_ob hy_ob_t;
typedef struct { uint32_t seq; int64_t at; uint8_t mask; char payload[HY_HOOK_PAYLOAD_MAX + 1]; } hy_ob_rec_t;
typedef enum { HY_OBO_DELIVERED = 0, HY_OBO_EXPIRED, HY_OBO_REJECTED } hy_ob_outcome_t;
typedef struct {
    uint32_t pending[HY_OB_DESTS];
    uint32_t live, dropped, expired, rejected, corrupt;
    uint32_t tail;
    uint32_t appends, state_writes, segments;
} hy_ob_stats_t;

/* dir NULL: RAM backend (no file is touched; same rules, nothing survives close). mono_s: monotonic seconds for
 * the flush limit (NULL counts as always 0). HY_E_IO when the directory cannot be made or read; HY_E_FULL when the
 * arena or a buffer cannot be allocated. */
hy_err_t hy_ob_open(const char *dir, int64_t (*mono_s)(void), hy_ob_t **out);
/* Forced flush (then the pending deletions), frees. */
void hy_ob_close(hy_ob_t *ob);
/* The seq the next append takes. */
uint32_t hy_ob_next_seq(const hy_ob_t *ob);
/* at: enqueue time, 0 when the clock is invalid. mask 0 (or bits at or above HY_OB_DESTS), an empty payload, one over
 * HY_HOOK_PAYLOAD_MAX or holding \t \r \n: HY_E_INVALID. HY_E_IO when the write failed (nothing counted, the seq is
 * not used). */
hy_err_t hy_ob_append(hy_ob_t *ob, int64_t at, uint8_t mask, const char *payload, uint32_t *seq);
/* Oldest pending record for dest. HY_E_NOT_FOUND when none. */
hy_err_t hy_ob_peek(hy_ob_t *ob, uint8_t dest, hy_ob_rec_t *out);
/* seq must be the one hy_ob_peek gave for dest (else ignored). Moves next[dest] past it, counts expired/rejected,
 * marks segments below low deletable, marks the state dirty. Deletes nothing itself. */
void hy_ob_done(hy_ob_t *ob, uint8_t dest, uint32_t seq, hy_ob_outcome_t how);
/* Destination turned off or removed: next[dest] = tail (its pending records are discarded, not counted). */
void hy_ob_drop_dest(hy_ob_t *ob, uint8_t dest);
/* Writes the state when dirty and (force, or a segment is deletable, or HY_OB_FLUSH_S since the last write); then,
 * only when that write succeeded, deletes the deletable segments. Returns true when it wrote. */
bool hy_ob_flush(hy_ob_t *ob, bool force);
void hy_ob_stats(const hy_ob_t *ob, hy_ob_stats_t *out);
/* True while a state change (an ack, a drop, a drop when full) is not yet written: also after a failed state write,
 * false after a successful one. NULL gives false. */
bool hy_ob_dirty(const hy_ob_t *ob);

#endif /* HY_OUTBOX_H */
