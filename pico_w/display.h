#include "pico/stdlib.h"
#include "pico/util/datetime.h"

#define SPI_BAUDRATE    16000000
#define SPI_ID              spi0

#define N_DIGITS       6

// Segment order defined by shift register output connections:
#define SEG_B   (1 << 0)
#define SEG_DP  (1 << 1)
#define SEG_A   (1 << 2)
#define SEG_C   (1 << 3)
#define SEG_F   (1 << 4)
#define SEG_D   (1 << 5)
#define SEG_E   (1 << 6)
#define SEG_G   (1 << 7)

void display_init ();
void display_set_brightness (uint8_t);
void display_on_tick ();
bool update_display (repeating_timer_t *);
