/*
 * airflow.h — chamber blower, radiator fan and the argon solenoid.
 *
 * This lives on the Mega today and moves to the ESP32 node later; the protocol
 * addresses fan messages to NODE_AIRFLOW so that migration is a one-line
 * change in protocol.h rather than an edit to every caller.
 *
 * Purging is a closed loop the board runs by itself: open the solenoid, watch
 * the oxygen channels, close at the target and reopen if it drifts back up.
 * Nothing in a print sequence may need a host round-trip, and "is the chamber
 * inert enough to melt in" is exactly the kind of decision that belongs on a
 * board.
 */

#ifndef MEGA_AIRFLOW_H
#define MEGA_AIRFLOW_H

#include <stdint.h>

#include "protocol.h"

namespace airflow {

void begin();
void service();

/* Returns an ack_status_t. FAN_MODE_MAPPED and FAN_MODE_CLOSEDLOOP are
 * refused: the Mega no longer sees scan speed (that is the Teensy's), and no
 * flow sensor is fitted. Refused rather than accepted-and-ignored, because on
 * this machine an ACK_OK reads as "it worked". */
uint8_t setFan(const fan_set_t &req);
bool    fillStatus(uint8_t fan, fan_status_t &out);

uint8_t setPurge(const purge_set_t &req);
bool    purging();

/* Consume-once: the purge did not reach its target within timeout_s. */
bool consumePurgeTimeout();

/* Everything off, valve closed. For MSG_ESTOP. */
void allOff();

}  // namespace airflow

#endif /* MEGA_AIRFLOW_H */
