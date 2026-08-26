#include "settings.h"

#include <Arduino.h>
#include <string.h>

#include "config.h"
#include "persist.h"

namespace settings {
namespace {

mega_settings_t current;

void loadDefaults()
{
    current.purge_target_o2_ppm = PURGE_TARGET_DEFAULT_PPM;
    current.purge_timeout_s     = PURGE_STAGE2_TIMEOUT_S;
    current.purge_min_mix_s     = PURGE_STAGE2_MIN_S;
    for (uint8_t i = 0; i < 3; i++)
        current.light_settle_ms[i] = LIGHT_SETTLE_DEFAULT_MS[i];
    current.recoat_settle_ms    = RECOAT_SETTLE_DEFAULT_MS;
    current.argon_flow_ml_min   = ARGON_FLOW_ML_MIN_DEFAULT;
}

/* Refused rather than clamped: a settings page that sends nonsense should be
 * told, not quietly given something else. */
bool plausible(const mega_settings_t &s)
{
    if (s.purge_target_o2_ppm == 0 || s.purge_target_o2_ppm > 50000) return false;
    if (s.purge_timeout_s == 0) return false;
    /* A mixing floor of zero is reachable only through
     * PURGE_FLAG_SKIP_MIN_MIX, which is a deliberate testing action. It must
     * not be reachable by storing it, or it would apply to every purge
     * silently from then on. */
    if (s.purge_min_mix_s == 0) return false;
    if (s.recoat_settle_ms > 60000) return false;
    if (s.argon_flow_ml_min == 0 || s.argon_flow_ml_min > 60000) return false;  /* 60 L/min */
    return true;
}

uint16_t orStored(uint16_t requested, uint16_t stored)
{
    return requested ? requested : stored;
}

}  // namespace

void begin()
{
    loadDefaults();
    persist::loadSettings(current);   /* leaves the defaults if nothing valid */
}

const mega_settings_t &get() { return current; }

uint8_t set(const mega_settings_t &in)
{
    if (!plausible(in)) return ACK_BAD_PARAM;
    current = in;
    persist::saveSettings(current);
    return ACK_OK;
}

uint16_t purgeTarget(uint16_t requested)  { return orStored(requested, current.purge_target_o2_ppm); }
uint16_t purgeTimeout(uint16_t requested) { return orStored(requested, current.purge_timeout_s); }
uint16_t purgeMinMix(uint16_t requested)  { return orStored(requested, current.purge_min_mix_s); }
uint16_t recoatSettle(uint16_t requested) { return orStored(requested, current.recoat_settle_ms); }

uint16_t lightSettle(uint8_t mode, uint16_t requested)
{
    if (mode > LIGHT_SHADOW) return 0;
    return orStored(requested, current.light_settle_ms[mode]);
}

uint32_t argonMl(uint16_t open_seconds)
{
    return ((uint32_t)open_seconds * (uint32_t)current.argon_flow_ml_min) / 60UL;
}

}  // namespace settings
