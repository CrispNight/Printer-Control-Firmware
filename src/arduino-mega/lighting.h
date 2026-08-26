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

/* Returns an ack_status_t. How long to wait before a capture is a stored
 * setting (mega_settings_t.light_settle_ms), not a parameter here. */
uint8_t set(const light_set_t &req);

uint8_t mode();

void off();

}  // namespace lighting

#endif /* MEGA_LIGHTING_H */
