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

/* Starts the three-stage purge described in config.h, or aborts one when
 * req.enable is 0. While it runs the purge OWNS the chamber blower, so a
 * manual fan set on FAN_CHAMBER_BLOWER is answered ACK_BUSY rather than
 * quietly fighting it. */
uint8_t setPurge(const purge_set_t &req);
bool    purging();
uint8_t purgeStage();   /* purge_stage_t, for state reporting */

/* Fill in a progress snapshot. A purge can run for the better part of an hour,
 * so "busy" is not a useful answer to a host. */
void fillPurgeStatus(purge_status_t &out);

/* Consume-once. A failed purge does NOT stop anything: the old sequence logged
 * it and carried on, and whether to print into a chamber that did not hold is
 * a policy decision for the job sequencer, not for the board that owns the
 * valve. It is reported so that decision can be made. */
uint8_t consumePurgeResult();   /* purge_result_t */

/* True once per change of purge stage, so progress can be published on change
 * as well as on a slow tick. */
bool consumeStageChanged();

/* Seconds the solenoid was open on the last purge. The PC used this to bill
 * argon consumption; there is no protocol field for it yet. */
uint16_t lastPurgeOpenSeconds();

/* Everything off, valve closed. For MSG_ESTOP. */
void allOff();

}  // namespace airflow

#endif /* MEGA_AIRFLOW_H */
