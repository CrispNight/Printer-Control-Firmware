#include "lighting.h"

#include <Arduino.h>

#include "config.h"
#include "pins.h"

namespace lighting {
namespace {

uint8_t  current_mode;
uint16_t settle_ms[3];

void apply(uint8_t m)
{
    /* Relay 0 is ambient, relay 1 is shadow; never both. */
    digitalWrite(PIN_LIGHT_RELAY[0], (m == LIGHT_AMBIENT) ? HIGH : LOW);
    digitalWrite(PIN_LIGHT_RELAY[1], (m == LIGHT_SHADOW) ? HIGH : LOW);
    current_mode = m;
}

}  // namespace

void begin()
{
    for (uint8_t i = 0; i < NUM_LIGHT_RELAYS; i++) {
        pinMode(PIN_LIGHT_RELAY[i], OUTPUT);
        digitalWrite(PIN_LIGHT_RELAY[i], LOW);
    }
    for (uint8_t i = 0; i < 3; i++) settle_ms[i] = LIGHT_SETTLE_DEFAULT_MS[i];
    current_mode = LIGHT_OFF;
}

uint8_t set(const light_set_t &req)
{
    if (req.mode > LIGHT_SHADOW) return ACK_BAD_PARAM;
    if (req.settle_ms != 0) settle_ms[req.mode] = req.settle_ms;
    apply(req.mode);
    return ACK_OK;
}

uint8_t mode() { return current_mode; }

uint16_t settleMs(uint8_t m)
{
    return (m <= LIGHT_SHADOW) ? settle_ms[m] : 0;
}

void off() { apply(LIGHT_OFF); }

}  // namespace lighting
