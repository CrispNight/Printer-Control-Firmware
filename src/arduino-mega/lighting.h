/*
 * lighting.h — chamber lighting, two relays, three modes.
 *
 * AMBIENT is flat illumination; SHADOW is side-lighting, which is how surface
 * topology — and therefore recoat defects — become visible to the camera.
 *
 * settle_ms is how long a caller should wait after switching before a capture
 * is worth taking: the webcam has a physical lens, so autofocus, white balance
 * and exposure all need time to adapt. It used to be hardcoded on the PC. It
 * is a machine setting here, dialled in once and rarely touched, that the PC
 * can update through MSG_LIGHT_SET.
 */

#ifndef MEGA_LIGHTING_H
#define MEGA_LIGHTING_H

#include <stdint.h>

#include "protocol.h"

namespace lighting {

void begin();

/* Returns an ack_status_t. A settle_ms of 0 keeps the configured default for
 * that mode rather than meaning "no wait". */
uint8_t set(const light_set_t &req);

uint8_t  mode();
uint16_t settleMs(uint8_t mode);

void off();

}  // namespace lighting

#endif /* MEGA_LIGHTING_H */
