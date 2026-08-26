#pragma once

#include <stdint.h>

#include "protocol.h"

// Field correction: the 65x65 grid of position offsets that straightens out
// f-theta lens distortion, plus the field scale that turns millimetres into
// galvo counts.
//
// SPATIAL, and not to be confused with the timing offset. That one is a single
// signed number for laser lead/lag; this one is a map.
//
// WHAT ARRIVES HERE
//
// Ordinary signed int16 offsets. The .cor file on disk stores sign-magnitude
// with bit 15 as a sign FLAG rather than two's complement, and that is decoded
// once by whoever reads the file -- never in here, and never in the
// interpolation path. Getting that wrong mirrors the field, which is a
// spectacular and confusing failure.
//
// The upload is atomic. Nothing is applied until the whole table has arrived
// and its CRC checks out, so a dropped chunk can never leave a half-written
// table in place. That is exactly how the old tooling failed: 4,225
// fire-and-forget writes whose only check confirmed the card was still
// responding, not that every line had landed.
//
// WITHOUT A TABLE
//
// apply() still works: it scales and centres, and adds no offsets. That is the
// machine's behaviour today, so nothing has to wait for a table to exist.

namespace field {

// Grid is fixed at 65x65 because the spacing that gives -- 65536/64 = 1024
// counts -- is a power of two, so a lookup is a shift rather than a division.
// A different grid size would need a different interpolation path, so one is
// refused rather than half-supported.
constexpr uint8_t  kGridSize    = 65;
constexpr uint16_t kGridPoints  = kGridSize * kGridSize;
constexpr uint16_t kCellShift   = 10;            // 1024 counts per cell
constexpr uint16_t kCellCounts  = 1u << kCellShift;

void begin();

// Is a verified correction table in use?
bool loaded();

// Field scale actually in force, milli-counts per millimetre. Falls back to
// the compiled-in value until a table sets it.
uint32_t scale_mcpmm();

// MSG_FIELD_CORRECTION_*. Each returns an ack_status_t.
uint8_t upload_begin(const field_corr_begin_t& hdr);
uint8_t upload_data(const field_corr_data_t& hdr, const uint8_t* points, uint8_t len);
uint8_t upload_end();

// Bed coordinates in micrometres to galvo DAC counts, corrected and clamped.
// Cheap to call in a tight loop: consecutive points nearly always land in the
// same grid cell, so the four corners are cached and the common case is a
// couple of multiplies.
void apply(int32_t x_um, int32_t y_um, uint16_t* out_x, uint16_t* out_y);

// Console: "field"
void cmd_status();

// Console: "field map <x_um> <y_um>" -- where does this bed coordinate land on
// the galvo? Prints the DAC counts apply() produces. This is the calibration
// question ("is that point where I think it is?") and it is also how the
// interpolation gets checked against a reference off-board, on the real chip
// rather than on a model of it.
void cmd_map(const char* args);

}  // namespace field
