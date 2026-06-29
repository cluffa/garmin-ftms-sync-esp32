#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef enum { BTN_NONE, BTN_SHORT, BTN_LONG, BTN_VERYLONG } button_event_t;

#define BTN_SHORT_MS    30u
#define BTN_LONG_MS     700u
/* Power off = 3× long: fill (0..700) + hold at 100% (700..1400) + reverse
 * (1400..2100). */
#define BTN_VERYLONG_MS (3u * BTN_LONG_MS)

/* Classify a press by how long it was held. */
button_event_t button_classify(uint32_t press_ms);

/* Loading-bar level 0..100: fills to 100 over the first long-press window,
 * holds at 100 for the second, then reverses to 0 over the third
 * (empty = power-off armed). */
uint8_t button_bar_pct(uint32_t held_ms);

/* True while the button is physically pressed. */
bool button_is_down(void);

/* Current hold duration in ms while the button is pressed; 0 if not held. */
uint32_t button_held_ms(void);

/* Start the GPIO0 (Heltec PRG) polling task; cb is called with BTN_SHORT,
 * BTN_LONG, or BTN_VERYLONG on each release. */
void button_init(void (*cb)(button_event_t));

/* Optional: cb fired on the press-down edge (used to wake the UI so the
 * loading bar appears immediately even from a slow idle refresh). */
void button_set_press_cb(void (*cb)(void));
