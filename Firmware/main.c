
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
#include <avr/eeprom.h>
#include <util/delay.h>

/* ===================== USER CONFIGURATION ===================== */

/* --- Weld pulse timing (milliseconds) --- */
#define WELD_PULSE_MIN_MS       1   /* Lowest selectable main-pulse duration */
#define WELD_PULSE_MAX_MS      10   /* Highest selectable main-pulse duration */
#define WELD_PULSE_DEFAULT_MS  5   /* Used when EEPROM contains an invalid value */

#define SETTINGS_BUTTON_MS      30   /* TP4 press and release debounce time */
#define SETTING_BLINK_ON_MS     80   /* LED-on time for each indication blink */
#define SETTING_BLINK_OFF_MS   120   /* LED-off time between indication blinks */
#define USE_DOUBLE_PULSE         1   /* 1 = fire a short pre-pulse before the main pulse,
                                         0 = single pulse only */
#define PRE_PULSE_MS              1   /* Pre-pulse duration (seats/cleans the tips on the
                                         workpiece before the main weld). Only used if
                                         USE_DOUBLE_PULSE == 1 */
#define PULSE_GAP_MS              50   /* Gap between pre-pulse and main pulse */

/* --- Safety / re-trigger protection --- */
#define WELD_COOLDOWN_MS        800   /* Minimum time after a weld fires before another can
                                         start, even if the trigger is still active */
#define TRIGGER_DEBOUNCE_MS      15   /* Stability time required for the PC817 weld trigger */
#define REQUIRE_TRIGGER_RELEASE   1   /* 1 = trigger (tip contact) must go inactive
                                         before the welder re-arms; strongly recommended -
                                         prevents continuous arcing/welding while the tips
                                         are left resting on the workpiece. 0 = re-arm
                                         immediately after cooldown. */

/* --- Feedback --- */
#define BEEP_ON_WELD_MS           60   /* Buzzer beep length right after a weld pulse fires */
#define LED_BLINK_POWERUP_MS     150   /* Startup LED blink length */
#define BUZZER_ACTIVE_TYPE         0   /* 1 = self-oscillating "active" buzzer - just needs
                                         DC applied (matches BZ1 being wired directly to the
                                         GPIO with no series resistor/transistor).
                                         0 = passive piezo - GPIO square-wave drive at
                                         BUZZER_TONE_HZ is generated instead. */
#define BUZZER_TONE_HZ           4000   /* Tone frequency used only if BUZZER_ACTIVE_TYPE == 0 */
/*
 * Timer0 uses a divide-by-8 clock.
 *
 * Output frequency:
 * F_CPU / (2 × 8 × (OCR0A + 1))
 *
 * The calculation is done at compile time, so it does not consume
 * runtime flash code for division.
 */
#define TONE_OCR(frequency) \
    ((uint8_t)(((F_CPU + (8UL * (frequency))) / (16UL * (frequency))) - 1UL))

/* Hard ceiling - refuses to compile an accidentally dangerous pulse length.
   Raise only if you really know what you are doing. */
#if USE_DOUBLE_PULSE
    #if (WELD_PULSE_MAX_MS + PRE_PULSE_MS) > 500
        #error "Maximum combined weld pulse exceeds the 500ms safety ceiling"
    #endif
#else
    #if WELD_PULSE_MAX_MS > 500
        #error "Maximum weld pulse exceeds the 500ms safety ceiling"
    #endif
#endif

/* ================================================================= */

/* --- Pin assignments (see header comment for derivation) --- */
#define PIN_BUZZER  PB0
#define PIN_LED     PB1
#define PIN_BTN     PB2   /* TP4 weld-duration settings button, active LOW */
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

static uint8_t EEMEM eeprom_weld_pulse_ms;
static uint8_t weld_pulse_ms;

static inline void weld_on(void)  { PORTB |=  (1 << PIN_WELD); }
static inline void weld_off(void) { PORTB &= ~(1 << PIN_WELD); }
static inline void led_on(void)   { PORTB |=  (1 << PIN_LED); }
static inline void led_off(void)  { PORTB &= ~(1 << PIN_LED); }
static inline void buzzer_on(void)  { PORTB |=  (1 << PIN_BUZZER); }
static inline void buzzer_off(void) { PORTB &= ~(1 << PIN_BUZZER); }

static void load_weld_setting(void)
{
    uint8_t saved_value = eeprom_read_byte(&eeprom_weld_pulse_ms);

    if ((saved_value < WELD_PULSE_MIN_MS) ||
        (saved_value > WELD_PULSE_MAX_MS)) {
        weld_pulse_ms = WELD_PULSE_DEFAULT_MS;
    } else {
        weld_pulse_ms = saved_value;
    }
}

/*
 * Play a tone using Timer0 and the OC0A hardware output on PB0.
 *
 * PB0 is OC0A on the ATtiny13. Timer0 toggles the pin automatically,
 * so the CPU only has to wait for the requested duration.
 */
static void passive_tone(uint8_t timer_value, uint16_t duration_ms)
{
    /* Make sure the buzzer begins LOW. */
    buzzer_off();

    /* Stop and reset Timer0 before configuring it. */
    TCCR0A = 0;
    TCCR0B = 0;
    TCNT0 = 0;

    /* Set the requested frequency. */
    OCR0A = timer_value;

    /*
     * CTC mode:
     * WGM01 = 1
     *
     * Toggle OC0A/PB0 each time the timer reaches OCR0A:
     * COM0A0 = 1
     */
    TCCR0A = (1 << COM0A0) | (1 << WGM01);

    /*
     * Start Timer0 with clock divided by 8:
     * CS01 = 1
     */
    TCCR0B = (1 << CS01);

    delay_ms(duration_ms);

    /* Stop Timer0. */
    TCCR0B = 0;

    /* Disconnect Timer0 from PB0. */
    TCCR0A = 0;

    /* Guarantee that the buzzer finishes LOW. */
    buzzer_off();
}

static void beep(uint16_t ms)
{
#if BUZZER_ACTIVE_TYPE
    buzzer_on();
    delay_ms(ms);
    buzzer_off();
#else
    passive_tone(TONE_OCR(BUZZER_TONE_HZ), ms);
#endif
}

static void welcome_chirp(void)
{
#if BUZZER_ACTIVE_TYPE
    /*
     * An active buzzer cannot change pitch, so use two short beeps.
     */
    beep(60);
    delay_ms(40);
    beep(100);
#else
    /*
     * Rising three-note startup chirp:
     *
     * Approximately 2 kHz → 3 kHz → 4 kHz
     */

    passive_tone(TONE_OCR(2000UL), 45);
    delay_ms(20);

    passive_tone(TONE_OCR(3000UL), 45);
    delay_ms(20);

    passive_tone(TONE_OCR(4000UL), 100);
#endif
}

static inline uint8_t weld_trigger_active(void)
{
    return ((PINB & (1 << PIN_OPTO)) == 0); /* PC817 triggers welding */
}

static inline uint8_t settings_button_active(void)
{
    return ((PINB & (1 << PIN_BTN)) == 0);  /* TP4 changes weld duration */
}

static void show_weld_setting(void)
{
    uint8_t blinks = weld_pulse_ms;

    while (blinks--) {
        led_on();
        beep(SETTING_BLINK_ON_MS);

        led_off();
        delay_ms(SETTING_BLINK_OFF_MS);
    }
}

static void handle_settings_button(void)
{
    delay_ms(SETTINGS_BUTTON_MS);

    if (!settings_button_active()) {
        return;                             /* Ignore a short noise pulse */
    }

    while (settings_button_active()) {
        /* Wait for TP4 to be released */
    }

    delay_ms(SETTINGS_BUTTON_MS);           /* Debounce the release */

    weld_pulse_ms++;

    if (weld_pulse_ms > WELD_PULSE_MAX_MS) {
        weld_pulse_ms = WELD_PULSE_MIN_MS;
    }

    eeprom_update_byte(&eeprom_weld_pulse_ms, weld_pulse_ms);

    show_weld_setting();
}

static void fire_weld(void)
{
#if USE_DOUBLE_PULSE
    if (weld_pulse_ms >= 3) {
        weld_on();
        delay_ms(PRE_PULSE_MS);
        weld_off();

        delay_ms(PULSE_GAP_MS);
    }
#endif

    weld_on();
    delay_ms(weld_pulse_ms);
    weld_off();

    beep(BEEP_ON_WELD_MS);

    led_on();
    delay_ms(60);
    led_off();
}

static void power_up_selftest(void)
{
    led_on();
    welcome_chirp();
    delay_ms(LED_BLINK_POWERUP_MS);
    led_off();
}

int main(void)
{
    /* Force the weld output low before touching anything else - this is the first thing
       that runs after reset, ahead of any other setup. R16 on the board does the same
       thing in hardware as a backstop. */
    PORTB &= ~(1 << PIN_WELD);
    DDRB |= (1 << PIN_WELD);
    
    /* Remaining outputs. */
    PORTB &= ~((1 << PIN_LED) | (1 << PIN_BUZZER));
    DDRB |= (1 << PIN_LED) | (1 << PIN_BUZZER);

    /* Trigger inputs, with internal pull-ups enabled alongside the external R6/R7
       pull-ups for extra noise immunity. */
    PORTB |= (1 << PIN_BTN) | (1 << PIN_OPTO);
    DDRB &= ~((1 << PIN_BTN) | (1 << PIN_OPTO));

    ADCSRA &= ~(1 << ADEN); /* ADC unused - save power */

    load_weld_setting();
    power_up_selftest();

for (;;) {
    if (settings_button_active()) {
        handle_settings_button();
        continue;
    }

    if (weld_trigger_active()) {
        delay_ms(TRIGGER_DEBOUNCE_MS);

        if (weld_trigger_active()) {
            fire_weld();
            delay_ms(WELD_COOLDOWN_MS);

#if REQUIRE_TRIGGER_RELEASE
            while (weld_trigger_active()) {
                /* Wait until the welding probes are lifted */
            }

            delay_ms(TRIGGER_DEBOUNCE_MS);
#endif
              }
        }
    }
}