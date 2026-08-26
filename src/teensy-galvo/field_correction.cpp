#include "field_correction.h"

#include <Arduino.h>
#include <stdio.h>
#include <string.h>

#include "config.h"

namespace field {
namespace {

/* Two full tables: the one in use and the one arriving. The swap at the end of
 * a verified upload is what makes it atomic.
 *
 * Both live in RAM2 rather than the fast tightly-coupled RAM1. They are 16.9 KB
 * each and, thanks to the cell cache below, actually get touched about once
 * every few hundred samples -- so the slower memory costs nothing and RAM1
 * stays free for code and stack. */
DMAMEM int16_t g_active_dx[kGridPoints];
DMAMEM int16_t g_active_dy[kGridPoints];
DMAMEM int16_t g_stage_dx[kGridPoints];
DMAMEM int16_t g_stage_dy[kGridPoints];

bool     g_loaded = false;
uint32_t g_scale_mcpmm = (uint32_t)(kCountsPerMm * 1000.0f);

/* Upload in progress. */
bool     g_uploading = false;
uint16_t g_expect_chunk = 0;
uint16_t g_points_in = 0;
uint16_t g_want_points = 0;
uint16_t g_want_crc = 0;
uint32_t g_want_scale = 0;

/* Cell cache. Consecutive galvo samples are microns apart and a cell is 1024
 * counts wide, so a scan line spends hundreds of samples inside one cell.
 * Caching the four corners turns the common case into arithmetic with no table
 * access at all. */
int32_t g_cx = -1, g_cy = -1;
int16_t g_dx00, g_dx10, g_dx01, g_dx11;
int16_t g_dy00, g_dy10, g_dy01, g_dy11;

void invalidateCache()
{
    g_cx = -1;
    g_cy = -1;
}

int32_t clampCount(int32_t v)
{
    if (v < 0) return 0;
    if (v > 65535) return 65535;
    return v;
}

/* Bilinear across one cell, entirely in integers. fx and fy are 0..1023, so the
 * weighting divides by 1024 -- a shift, which is the whole reason the grid is
 * 65 points and not a rounder-looking number. */
inline int32_t lerpCell(int32_t c00, int32_t c10, int32_t c01, int32_t c11,
                        int32_t fx, int32_t fy)
{
    const int32_t top = c00 + (((c10 - c00) * fx) >> kCellShift);
    const int32_t bot = c01 + (((c11 - c01) * fx) >> kCellShift);
    return top + (((bot - top) * fy) >> kCellShift);
}

void loadCell(int32_t ix, int32_t iy)
{
    const uint16_t i0 = (uint16_t)(iy * kGridSize + ix);
    const uint16_t i1 = (uint16_t)(i0 + 1);
    const uint16_t i2 = (uint16_t)(i0 + kGridSize);
    const uint16_t i3 = (uint16_t)(i2 + 1);

    g_dx00 = g_active_dx[i0]; g_dx10 = g_active_dx[i1];
    g_dx01 = g_active_dx[i2]; g_dx11 = g_active_dx[i3];
    g_dy00 = g_active_dy[i0]; g_dy10 = g_active_dy[i1];
    g_dy01 = g_active_dy[i2]; g_dy11 = g_active_dy[i3];

    g_cx = ix;
    g_cy = iy;
}

}  // namespace

void begin()
{
    memset(g_active_dx, 0, sizeof(g_active_dx));
    memset(g_active_dy, 0, sizeof(g_active_dy));
    g_loaded = false;
    g_uploading = false;
    g_scale_mcpmm = (uint32_t)(kCountsPerMm * 1000.0f);
    invalidateCache();
}

bool loaded() { return g_loaded; }

uint32_t scale_mcpmm() { return g_scale_mcpmm; }

uint8_t upload_begin(const field_corr_begin_t& hdr)
{
    if (hdr.grid_size != kGridSize) return ACK_BAD_PARAM;
    if (hdr.point_total != kGridPoints) return ACK_BAD_PARAM;

    /* A scale outside the band implies a field smaller than 33 mm or larger
     * than 300 mm, which is not this machine. Exactly 1000 -- 1.0 counts per
     * millimetre -- is the placeholder the old tooling wrote into every .cor
     * file, and taken literally it describes a 65-metre field. Refuse it
     * rather than quietly substituting something sensible: silently fixing it
     * would hide the fact that the file was never calibrated. */
    if (hdr.scale_mcpmm < FIELD_SCALE_MIN_MCPMM ||
        hdr.scale_mcpmm > FIELD_SCALE_MAX_MCPMM) {
        return ACK_BAD_PARAM;
    }

    g_uploading    = true;
    g_expect_chunk = 0;
    g_points_in    = 0;
    g_want_points  = hdr.point_total;
    g_want_crc     = hdr.table_crc;
    g_want_scale   = hdr.scale_mcpmm;
    memset(g_stage_dx, 0, sizeof(g_stage_dx));
    memset(g_stage_dy, 0, sizeof(g_stage_dy));
    return ACK_OK;
}

uint8_t upload_data(const field_corr_data_t& hdr, const uint8_t* points, uint8_t len)
{
    if (!g_uploading) return ACK_BAD_STATE;

    /* In-order and accounted for. A gap that went unnoticed would shift every
     * point after it, and the table would look plausible while being wrong
     * everywhere -- which is far worse than a refused upload. */
    if (hdr.chunk_index != g_expect_chunk) return ACK_BAD_PARAM;
    if (hdr.point_count == 0 || hdr.point_count > FIELD_CORR_MAX_POINTS) return ACK_BAD_PARAM;
    if (len < (uint16_t)hdr.point_count * sizeof(field_corr_point_t)) return ACK_BAD_LENGTH;
    if ((uint32_t)g_points_in + hdr.point_count > g_want_points) return ACK_BAD_PARAM;

    for (uint16_t n = 0; n < hdr.point_count; n++) {
        field_corr_point_t pt;
        memcpy(&pt, points + n * sizeof(field_corr_point_t), sizeof(pt));
        g_stage_dx[g_points_in] = pt.dx;
        g_stage_dy[g_points_in] = pt.dy;
        g_points_in++;
    }

    g_expect_chunk++;
    return ACK_OK;
}

uint8_t upload_end()
{
    if (!g_uploading) return ACK_BAD_STATE;
    g_uploading = false;

    if (g_points_in != g_want_points) return ACK_BAD_LENGTH;

    /* CRC over the entries as they went on the wire: dx then dy, little-endian,
     * in order. Checked before anything is applied. */
    uint16_t crc = 0xFFFF;
    for (uint16_t n = 0; n < g_want_points; n++) {
        const uint8_t bytes[4] = {
            (uint8_t)(g_stage_dx[n] & 0xFF), (uint8_t)((g_stage_dx[n] >> 8) & 0xFF),
            (uint8_t)(g_stage_dy[n] & 0xFF), (uint8_t)((g_stage_dy[n] >> 8) & 0xFF),
        };
        for (uint8_t b = 0; b < 4; b++) {
            crc ^= (uint16_t)bytes[b] << 8;
            for (uint8_t bit = 0; bit < 8; bit++) {
                crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u)
                                      : (uint16_t)(crc << 1);
            }
        }
    }
    if (crc != g_want_crc) return ACK_BAD_CRC;

    /* Commit. Only now does anything change. */
    memcpy(g_active_dx, g_stage_dx, sizeof(g_active_dx));
    memcpy(g_active_dy, g_stage_dy, sizeof(g_active_dy));
    g_scale_mcpmm = g_want_scale;
    g_loaded = true;
    invalidateCache();
    return ACK_OK;
}

void apply(int32_t x_um, int32_t y_um, uint16_t* out_x, uint16_t* out_y)
{
    /* Micrometres to counts, centred on mid-scale. The multiply is done in
     * 64-bit because a 150 mm offset at 374.5 counts/mm is about 5.6e10 before
     * the divide. */
    const int64_t sx = ((int64_t)x_um * (int64_t)g_scale_mcpmm) / 1000000;
    const int64_t sy = ((int64_t)y_um * (int64_t)g_scale_mcpmm) / 1000000;

    int32_t cx = clampCount((int32_t)(32768 + sx));
    int32_t cy = clampCount((int32_t)(32768 + sy));

    if (g_loaded) {
        /* The last grid line has no cell beyond it, so a point sitting exactly
         * on the far edge belongs to the cell below it. */
        int32_t ix = cx >> kCellShift;
        int32_t iy = cy >> kCellShift;
        if (ix >= kGridSize - 1) ix = kGridSize - 2;
        if (iy >= kGridSize - 1) iy = kGridSize - 2;

        if (ix != g_cx || iy != g_cy) loadCell(ix, iy);

        const int32_t fx = cx - (ix << kCellShift);
        const int32_t fy = cy - (iy << kCellShift);

        cx += lerpCell(g_dx00, g_dx10, g_dx01, g_dx11, fx, fy);
        cy += lerpCell(g_dy00, g_dy10, g_dy01, g_dy11, fx, fy);
        cx = clampCount(cx);
        cy = clampCount(cy);
    }

    *out_x = (uint16_t)cx;
    *out_y = (uint16_t)cy;
}

void cmd_map(const char* args)
{
    long x_um = 0, y_um = 0;
    if (sscanf(args, "%ld %ld", &x_um, &y_um) != 2) {
        Serial.println(F("usage: field map <x_um> <y_um>"));
        return;
    }
    uint16_t cx = 0, cy = 0;
    apply((int32_t)x_um, (int32_t)y_um, &cx, &cy);
    Serial.print(F("map "));
    Serial.print(x_um); Serial.print(' '); Serial.print(y_um);
    Serial.print(F(" -> "));
    Serial.print(cx); Serial.print(' '); Serial.println(cy);
}

void cmd_status()
{
    Serial.print(F("field correction: "));
    Serial.println(g_loaded ? F("loaded") : F("none (scale only, no offsets)"));

    Serial.print(F("  scale       : "));
    Serial.print(g_scale_mcpmm / 1000);
    Serial.print('.');
    Serial.print((g_scale_mcpmm % 1000) / 100);
    Serial.print(F(" counts/mm  -> "));
    /* Rounded, not truncated: 374.5 counts/mm is a 174.99 mm field, and
     * printing "174" invites someone to think the scale is wrong. */
    Serial.print((uint32_t)((65536ULL * 1000ULL + g_scale_mcpmm / 2) / g_scale_mcpmm));
    Serial.println(F(" mm field"));

    Serial.print(F("  grid        : "));
    Serial.print(kGridSize); Serial.print('x'); Serial.print(kGridSize);
    Serial.print(F("  cell ")); Serial.print(kCellCounts);
    Serial.println(F(" counts"));

    if (g_uploading) {
        Serial.print(F("  UPLOAD in progress: "));
        Serial.print(g_points_in); Serial.print('/'); Serial.println(g_want_points);
    }

    if (g_loaded) {
        int16_t lo = 0, hi = 0;
        for (uint16_t n = 0; n < kGridPoints; n++) {
            if (g_active_dx[n] < lo) lo = g_active_dx[n];
            if (g_active_dx[n] > hi) hi = g_active_dx[n];
        }
        Serial.print(F("  dx range    : "));
        Serial.print(lo); Serial.print(F(" .. ")); Serial.println(hi);
    }
}

}  // namespace field
