#include "sensors.h"

#include <Arduino.h>
#include <math.h>

#include "config.h"
#include "pins.h"

namespace sensors {
namespace {

/* valid_mask / override_mask bit layout, from protocol.h: bit0..1 oxygen,
 * bit8..13 temperature. */
uint16_t bitOxygen(uint8_t i) { return (uint16_t)(1u << i); }
uint16_t bitTemp(uint8_t i)   { return (uint16_t)(1u << (8 + i)); }

uint16_t oxy_ppm[NUM_OXYGEN_SENSORS];        /* as reported */
uint16_t oxy_true_ppm[NUM_OXYGEN_SENSORS];   /* what the hardware actually reads */
int16_t  temp_c_x10[NUM_TEMP_SENSORS];
int16_t  temp_true_c_x10[NUM_TEMP_SENSORS];
uint16_t valid_mask;
uint16_t warn_mask;
uint16_t fitted_mask;        /* the channels that are supposed to work */
uint16_t invalid_since_ms[NUM_TEMP_SENSORS + NUM_OXYGEN_SENSORS];
uint16_t reported_broken;    /* one fault per channel, not one per sample */
uint8_t  broken_pending = SENSOR_NONE;
bool     override_changed = true;   /* publish once on connect */

uint8_t  next_channel;              /* 0..1 oxygen, 2..7 temperature */
uint32_t last_sample_ms;

/* Slot in invalid_since_ms[] for a valid_mask bit index. */
uint8_t slotOf(uint8_t bit_index)
{
    return (bit_index < 8) ? bit_index : (uint8_t)(NUM_OXYGEN_SENSORS + (bit_index - 8));
}

void noteValid(uint8_t bit_index, uint16_t bit)
{
    invalid_since_ms[slotOf(bit_index)] = 0;
    reported_broken &= (uint16_t)~bit;
}

void noteInvalid(uint8_t bit_index, uint16_t bit)
{
    if (!(fitted_mask & bit)) return;      /* never fitted; not a fault */
    if (reported_broken & bit) return;     /* already reported once */

    const uint8_t slot = slotOf(bit_index);
    const uint16_t now = (uint16_t)millis();
    if (invalid_since_ms[slot] == 0) {
        invalid_since_ms[slot] = now ? now : 1;
        return;
    }
    if ((uint16_t)(now - invalid_since_ms[slot]) < SENSOR_INVALID_FAULT_MS) return;

    reported_broken |= bit;
    if (broken_pending == SENSOR_NONE) broken_pending = bit_index;
}

uint16_t readOxygenPpm(uint8_t i)
{
    const uint32_t adc = (uint32_t)analogRead(PIN_OXYGEN_ANA[i]);
    return (uint16_t)((adc * OXYGEN_PPM_FULL_SCALE) / ADC_MAX);
}

void sampleOxygen(uint8_t i)
{
    const uint16_t ppm = readOxygenPpm(i);
    oxy_true_ppm[i] = ppm;

    if (SENSOR_TEST_MODE)      oxy_ppm[i] = TEST_OXYGEN_PPM;
    else if (OXYGEN_OVERRIDE)  oxy_ppm[i] = OXYGEN_OVERRIDE_PPM;
    else                       oxy_ppm[i] = ppm;

    valid_mask |= bitOxygen(i);

    /* Into the interlock chain: LOW asserts "oxygen is above the threshold".
     * Note this uses the REPORTED value, so an override feeds the chain too —
     * which is the whole point of being able to work with a dead sensor, and
     * exactly why MSG_SENSOR_OVERRIDE exists to make it visible. */
    digitalWrite(PIN_OXYGEN_DIG[i], (oxy_ppm[i] >= OXYGEN_THRESHOLD_PPM) ? LOW : HIGH);
}

void sampleTemperature(uint8_t i)
{
    analogRead(PIN_TEMP_ANA[i]);          // dummy read — let mux + sample-hold settle after channel switch
    const float adc = (float)analogRead(PIN_TEMP_ANA[i]);

    const float *fit = (i == 2) ? TEMP_FIT_S4 : TEMP_FIT_S2;
    const float t = fit[0] * adc * adc + fit[1] * adc + fit[2];

    int16_t raw_x10 = (int16_t)lroundf(t * 10.0f);

    /* Reject implausible readings on unused/floating channels — prevents
     * heater-switching noise spikes. A rejected channel holds its last value
     * and drops out of valid_mask so the reading is not mistaken for real. */
    const bool plausible = (raw_x10 >= TEMP_MIN_PLAUSIBLE_C_X10 &&
                            raw_x10 <= TEMP_MAX_PLAUSIBLE_C_X10);
    if (!plausible) {
        valid_mask &= (uint16_t)~bitTemp(i);
        noteInvalid((uint8_t)(8 + i), bitTemp(i));
        return;
    }
    valid_mask |= bitTemp(i);
    noteValid((uint8_t)(8 + i), bitTemp(i));

    temp_true_c_x10[i] = raw_x10;
    if (SENSOR_TEST_MODE) {
        temp_c_x10[i] = TEST_TEMP_C_X10;
    } else {
        temp_c_x10[i] = (int16_t)(raw_x10 + (int16_t)lroundf(TEMP_TRIM_C[i] * 10.0f));
    }

    /* The chain and the warning band both compare the untrimmed reading, as
     * the old firmware did — the trim corrects the reported value, not the
     * thresholds it was set against. */
    const int16_t limit = TEMP_LIMIT_C_X10[i];
    const bool over = (raw_x10 >= limit);
    const uint16_t safety_bit = (uint16_t)(1u << (4 + i));  /* PIN_SAFETY 4..9 are temperature */
    if (!over && raw_x10 >= (int16_t)(limit + TEMP_WARN_OFFSET_C_X10))
        warn_mask |= safety_bit;
    else
        warn_mask &= (uint16_t)~safety_bit;

    digitalWrite(PIN_TEMP_DIG[i], over ? LOW : HIGH);
}

}  // namespace

void begin()
{
    for (uint8_t i = 0; i < NUM_OXYGEN_SENSORS; i++) {
        pinMode(PIN_OXYGEN_DIG[i], OUTPUT);
        digitalWrite(PIN_OXYGEN_DIG[i], HIGH);
        oxy_ppm[i] = 0;
        oxy_true_ppm[i] = 0;
    }
    for (uint8_t i = 0; i < NUM_TEMP_SENSORS; i++) {
        pinMode(PIN_TEMP_DIG[i], OUTPUT);
        digitalWrite(PIN_TEMP_DIG[i], HIGH);
        temp_c_x10[i] = 0;
        temp_true_c_x10[i] = 0;
    }
    valid_mask = 0;
    warn_mask = 0;
    fitted_mask = (uint16_t)(OXYGEN_FITTED_MASK | ((uint16_t)TEMP_FITTED_MASK << 8));
    reported_broken = 0;
    broken_pending = SENSOR_NONE;
    for (uint8_t i = 0; i < NUM_TEMP_SENSORS + NUM_OXYGEN_SENSORS; i++)
        invalid_since_ms[i] = 0;
    next_channel = 0;
    last_sample_ms = millis();
}

void service()
{
    const uint32_t now = millis();
    if (now - last_sample_ms < SENSOR_SAMPLE_INTERVAL_MS) return;
    last_sample_ms = now;

    if (next_channel < NUM_OXYGEN_SENSORS)
        sampleOxygen(next_channel);
    else
        sampleTemperature((uint8_t)(next_channel - NUM_OXYGEN_SENSORS));

    next_channel++;
    if (next_channel >= NUM_OXYGEN_SENSORS + NUM_TEMP_SENSORS) next_channel = 0;
}

void fill(sensor_report_t &out)
{
    for (uint8_t i = 0; i < NUM_OXYGEN_SENSORS; i++) out.oxygen_ppm[i] = oxy_ppm[i];
    for (uint8_t i = 0; i < NUM_TEMP_SENSORS; i++)   out.temp_c_x10[i] = temp_c_x10[i];
    out.valid_mask = valid_mask;
}

bool overrideActive()
{
    return SENSOR_TEST_MODE || OXYGEN_OVERRIDE;
}

void fillOverride(sensor_override_t &out)
{
    uint16_t mask = 0;
    if (SENSOR_TEST_MODE) {
        for (uint8_t i = 0; i < NUM_OXYGEN_SENSORS; i++) mask |= bitOxygen(i);
        for (uint8_t i = 0; i < NUM_TEMP_SENSORS; i++)   mask |= bitTemp(i);
    } else if (OXYGEN_OVERRIDE) {
        for (uint8_t i = 0; i < NUM_OXYGEN_SENSORS; i++) mask |= bitOxygen(i);
    }
    out.override_mask = mask;
    for (uint8_t i = 0; i < NUM_OXYGEN_SENSORS; i++) out.oxygen_true_ppm[i] = oxy_true_ppm[i];
    for (uint8_t i = 0; i < NUM_TEMP_SENSORS; i++)   out.temp_true_c_x10[i] = temp_true_c_x10[i];
}

bool consumeOverrideChanged()
{
    const bool changed = override_changed;
    override_changed = false;
    return changed;
}

uint16_t oxygenWorstPpm()
{
    uint16_t worst = 0;
    for (uint8_t i = 0; i < NUM_OXYGEN_SENSORS; i++)
        if ((valid_mask & bitOxygen(i)) && oxy_ppm[i] > worst) worst = oxy_ppm[i];
    return worst;
}

uint16_t warnMask() { return warn_mask; }

uint8_t consumeInvalidSensor()
{
    const uint8_t s = broken_pending;
    broken_pending = SENSOR_NONE;
    return s;
}

}  // namespace sensors
