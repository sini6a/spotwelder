/*
 * Datakonsulten Spot Welder - ATtiny13-20P firmware
 *
 * Pin mapping (derived from KiCad/dk-spot-welder.kicad_sch, U3):
 *   PB0 (pin 5) -> BZ1   buzzer, active HIGH, no series resistor (active/self-oscillating
 *                  piezo buzzer assumed - see BUZZER_ACTIVE_TYPE below)
 *   PB1 (pin 6) -> D5    status LED via R4, active HIGH
 *   PB2 (pin 7) -> TP4   manual trigger button header, external pull-up R7 to +5V,
 *                  active LOW (button pulls to CTRL_GND when pressed)
 *   PB3 (pin 2) -> U4    PC817 phototransistor output, external pull-up R6 to +5V,
 *                  active LOW (opto conducts when welding tips touch the workpiece)
 *   PB4 (pin 3) -> R3 -> U1 MCP1407 INPUT, active HIGH = MOSFET bank ON (welding current
 *                  flowing). R16 hardware-pulls this net low at the driver input as a
 *                  fail-safe, but firmware also forces PB4 low before anything else runs.
 *   PB5 (pin 1) -> RESET, pulled up by R8. Not used as GPIO.
 *
 * All user-tunable parameters are grouped at the top of this file.
 */

#define F_CPU 1200000UL   /* ATtiny13 factory-default fuses = 9.6MHz internal RC / 8 = 1.2MHz.
                              Must match the fuses actually programmed into the chip - see
                              Makefile `fuses` / `fuses-fast` targets. */

#include <avr/io.h>
#include <util/delay.h>

/* ===================== USER CONFIGURATION ===================== */

/* --- Weld pulse timing (milliseconds) --- */
#define WELD_PULSE_MS          18   /* Main weld pulse duration */
#define USE_DOUBLE_PULSE         1   /* 1 = fire a short pre-pulse before the main pulse,
                                         0 = single pulse only */
#define PRE_PULSE_MS              3   /* Pre-pulse duration (seats/cleans the tips on the
                                         workpiece before the main weld). Only used if
                                         USE_DOUBLE_PULSE == 1 */
#define PULSE_GAP_MS              80   /* Gap between pre-pulse and main pulse */

/* --- Safety / re-trigger protection --- */
#define WELD_COOLDOWN_MS        800   /* Minimum time after a weld fires before another can
                                         start, even if the trigger is still active */
#define TRIGGER_DEBOUNCE_MS      15   /* Debounce time applied to both trigger inputs */
#define REQUIRE_TRIGGER_RELEASE   1   /* 1 = trigger (button or tip contact) must go inactive
                                         before the welder re-arms; strongly recommended -
                                         prevents continuous arcing/welding while the tips
                                         are left resting on the workpiece. 0 = re-arm
                                         immediately after cooldown. */

/* --- Feedback --- */
#define BEEP_ON_WELD_MS           60   /* Buzzer beep length right after a weld pulse fires */
#define BEEP_ON_POWERUP_MS        80   /* Startup self-test beep length */
#define LED_BLINK_POWERUP_MS     150   /* Startup LED blink length */
#define BUZZER_ACTIVE_TYPE         1   /* 1 = self-oscillating "active" buzzer - just needs
                                         DC applied (matches BZ1 being wired directly to the
                                         GPIO with no series resistor/transistor).
                                         0 = passive piezo - GPIO square-wave drive at
                                         BUZZER_TONE_HZ is generated instead. */
#define BUZZER_TONE_HZ           2730   /* Tone frequency used only if BUZZER_ACTIVE_TYPE == 0 */

/* Hard ceiling - refuses to compile an accidentally dangerous pulse length.
   Raise only if you really know what you are doing. */
#if (WELD_PULSE_MS + PRE_PULSE_MS) > 500
#error "Combined weld pulse length exceeds the 500ms safety ceiling"
#endif

/* ================================================================= */

/* --- Pin assignments (see header comment for derivation) --- */
#define PIN_BUZZER  PB0
#define PIN_LED     PB1
#define PIN_BTN     PB2   /* manual trigger, TP4 */
#define PIN_OPTO    PB3   /* automatic tip-touch trigger, via U4/PC817 */
#define PIN_WELD    PB4   /* MOSFET bank enable, via U1/MCP1407 */

/* Millisecond delay built from a compile-time-constant _delay_us() call per iteration,
   rather than a single runtime _delay_ms() call, to keep code size small on the
   ATtiny13's 1KB flash. */
static void delay_ms(uint16_t ms)
{
    while (ms--) {
        _delay_us(1000);
    }
}

static inline void weld_on(void)  { PORTB |=  (1 << PIN_WELD); }
static inline void weld_off(void) { PORTB &= ~(1 << PIN_WELD); }
static inline void led_on(void)   { PORTB |=  (1 << PIN_LED); }
static inline void led_off(void)  { PORTB &= ~(1 << PIN_LED); }
static inline void buzzer_on(void)  { PORTB |=  (1 << PIN_BUZZER); }
static inline void buzzer_off(void) { PORTB &= ~(1 << PIN_BUZZER); }

static void beep(uint16_t ms)
{
#if BUZZER_ACTIVE_TYPE
    buzzer_on();
    delay_ms(ms);
    buzzer_off();
#else
    /* Passive piezo: generate a square wave at BUZZER_TONE_HZ for `ms` milliseconds. */
    uint16_t half_period_us = 500000UL / BUZZER_TONE_HZ;
    uint32_t toggles = ((uint32_t)ms * 1000UL) / half_period_us;
    for (uint32_t i = 0; i < toggles; i++) {
        PORTB ^= (1 << PIN_BUZZER);
        _delay_us(half_period_us);
    }
    buzzer_off();
#endif
}

/* Active-LOW: true if either the manual button or the opto auto-trigger is asserted. */
static inline uint8_t trigger_active(void)
{
    return ((PINB & (1 << PIN_BTN)) == 0) || ((PINB & (1 << PIN_OPTO)) == 0);
}

static void fire_weld(void)
{
    weld_on();
#if USE_DOUBLE_PULSE
    delay_ms(PRE_PULSE_MS);
    weld_off();
    delay_ms(PULSE_GAP_MS);
    weld_on();
#endif
    delay_ms(WELD_PULSE_MS);
    weld_off();

    beep(BEEP_ON_WELD_MS);
    led_on();
    delay_ms(60);
    led_off();
}

static void power_up_selftest(void)
{
    led_on();
    beep(BEEP_ON_POWERUP_MS);
    delay_ms(LED_BLINK_POWERUP_MS);
    led_off();
}

int main(void)
{
    /* Force the weld output low before touching anything else - this is the first thing
       that runs after reset, ahead of any other setup. R16 on the board does the same
       thing in hardware as a backstop. */
    DDRB |= (1 << PIN_WELD);
    PORTB &= ~(1 << PIN_WELD);

    /* Remaining outputs. */
    DDRB |= (1 << PIN_LED) | (1 << PIN_BUZZER);
    PORTB &= ~((1 << PIN_LED) | (1 << PIN_BUZZER));

    /* Trigger inputs, with internal pull-ups enabled alongside the external R6/R7
       pull-ups for extra noise immunity. */
    DDRB &= ~((1 << PIN_BTN) | (1 << PIN_OPTO));
    PORTB |= (1 << PIN_BTN) | (1 << PIN_OPTO);

    ADCSRA &= ~(1 << ADEN); /* ADC unused - save power */

    power_up_selftest();

    for (;;) {
        if (trigger_active()) {
            delay_ms(TRIGGER_DEBOUNCE_MS);
            if (trigger_active()) {
                fire_weld();
                delay_ms(WELD_COOLDOWN_MS);
#if REQUIRE_TRIGGER_RELEASE
                while (trigger_active()) {
                    /* Hold here until the button is released / tips are lifted off
                       the workpiece before re-arming. */
                }
                delay_ms(TRIGGER_DEBOUNCE_MS);
#endif
            }
        }
    }
}
