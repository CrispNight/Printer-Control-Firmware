#pragma once

#include <stdint.h>

#include "protocol.h"

// The job file on the microSD card.
//
// WHY THE CARD AND NOT RAM OR THE HOST
//
// A real job is 19 to 43 MB across fifteen hundred layers, so it does not fit
// in memory and never will. Streaming it from a host would put that host back
// in the real-time path, and the whole point of this machine is that the PC is
// optional -- it adds a UI, a file upload and a camera, and nothing a print
// depends on. So a job is uploaded once and then read from the card, layer by
// layer, for however many hours the print takes.
//
// ATOMIC COMMIT
//
// Bytes land in a temporary file. Only when the last chunk has arrived, the
// byte count matches and the whole-file CRC verifies does that file become the
// job. A transfer that fails part-way leaves the previous job untouched and
// never becomes printable.
//
// That is the lesson from the old correction-table upload: 4,225
// fire-and-forget writes whose only check confirmed the card was still
// responding, not that every line had landed. It cost a week.
//
// THE PER-LAYER CRC IS THE ONE THAT MATTERS
//
// The whole-file CRC proves the upload arrived. Each layer also carries its
// own, and that one is checked when the layer is READ, minutes or hours later.
// A card can develop bad sectors long after a correct write, so this is the
// check that actually stands between a bad read and a ruined print.

namespace job {

void begin();

// Is the card present and usable? Everything below fails without it.
bool card_ready();

// Is there a verified, printable job on the card?
bool have_job();
uint32_t job_id();
uint16_t layer_count();
uint32_t job_bytes();

// MSG_JOB_UPLOAD_*. Each returns an ack_status_t.
uint8_t upload_begin(const job_upload_begin_t& hdr);
uint8_t upload_data(const job_upload_data_t& hdr, const uint8_t* bytes, uint8_t len);
uint8_t upload_end(const job_upload_end_t& hdr);

// Console: "job"
void cmd_status();

}  // namespace job
