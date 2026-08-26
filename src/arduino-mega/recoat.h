/*
 * recoat.h — one complete powder recoat pass, run entirely on the Mega.
 *
 * The Mega owns the axes and the limit switches, and the cycle has to complete
 * correctly even if the link hiccups, so the whole sequence runs from a single
 * MSG_RECOAT_CYCLE rather than being micro-managed move by move from another
 * node. The parameters travel in the message, so the shape of the cycle stays
 * selectable without reflashing: logic with the hardware, policy in the
 * message.
 *
 * Order matters and differs by park mode; both are laid out in the sequencer
 * and in PROTOCOL.md.
 *
 * feed_um and bed_um are increments, not targets — the piston rise and the
 * platform drop for this one layer. The old machine had the PC compute
 * absolute targets from positions it held itself, which is why it could not
 * run without a host.
 */

#ifndef MEGA_RECOAT_H
#define MEGA_RECOAT_H

#include <stdint.h>

#include "protocol.h"

namespace recoat {

enum result_t {
    RESULT_NONE = 0,  /* nothing has finished since the last call */
    RESULT_OK,
    RESULT_FAILED,    /* an axis refused a move or hit an unexpected limit */
};

void begin();
void service();

/* Validate and start a cycle. Returns an ack_status_t: ACK_OK when it starts,
 * ACK_BUSY, ACK_BAD_PARAM or ACK_BAD_STATE otherwise. Completion is reported
 * through consumeResult(). */
uint8_t start(const recoat_cycle_t &req);

void abort();
bool active();

/* Consume-once: returns RESULT_OK / RESULT_FAILED exactly once per cycle. */
result_t consumeResult();

}  // namespace recoat

#endif /* MEGA_RECOAT_H */
