#include "display.h"

#include "hardware/rtc.h"
#include "hardware/spi.h"

static uint8_t digit = 0;
static uint8_t display [N_DIGITS] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
static repeating_timer_t rt_display_update;

static uint8_t
seg7_encode (char c)
{
   // Drive is active low, so list dark segments for each character:
   switch (c)
   {
   case ' ': return SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F | SEG_G |
                    SEG_DP;
   case '0': return SEG_G | SEG_DP;
   case '1': return SEG_A | SEG_D | SEG_E | SEG_F | SEG_G | SEG_DP;
   case '2': return SEG_C | SEG_F | SEG_DP;
   case '3': return SEG_E | SEG_F | SEG_DP;
   case '4': return SEG_A | SEG_D | SEG_E | SEG_DP;
   case '5': return SEG_B | SEG_E | SEG_DP;
   case '6': return SEG_B | SEG_DP;
   case '7': return SEG_D | SEG_E | SEG_F | SEG_G | SEG_DP;
   case '8': return SEG_DP;
   case '9': return SEG_E | SEG_DP;
   default: return 0;
   }
}

void
display_init ()
{
   spi_init (SPI_ID, SPI_BAUDRATE);
   spi_set_format (SPI_ID, 16, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
   gpio_set_function (PICO_DEFAULT_SPI_SCK_PIN, GPIO_FUNC_SPI);
   gpio_set_function (PICO_DEFAULT_SPI_TX_PIN, GPIO_FUNC_SPI);
   gpio_set_function (PICO_DEFAULT_SPI_CSN_PIN, GPIO_FUNC_SPI);

   add_repeating_timer_ms (-2, update_display, NULL, &rt_display_update);
}

void
display_on_tick ()
{
   datetime_t t;

   rtc_get_datetime (&t);

   if (t.hour < 10)
      display [0] = seg7_encode (' ');
   else
      display [0] = seg7_encode ((t.hour / 10) + '0');
   display [1] = seg7_encode ((t.hour % 10) + '0');
   display [2] = seg7_encode ((t.min / 10) + '0');
   display [3] = seg7_encode ((t.min % 10) + '0');
   display [4] = seg7_encode ((t.sec / 10) + '0');
   display [5] = seg7_encode ((t.sec % 10) + '0');

   display [1] &= ~SEG_DP;
   display [3] &= ~SEG_DP;
}

bool
update_display (repeating_timer_t *rt)
{
   // Digit select is active high (common anode),
   // the lowest two digits are not connected.
   // N.B.: digit order is reversed (d.[0] is leftmost,
   // i.e., highest value) for the sake of PCB wiring!
   uint8_t v = 128 >> digit;
   // MSB gets shifted out first (driving SEG_G);
   // LSB is the lowermost unconnected digit-driving bit.
   uint16_t w = (display [digit] << 8) | v;

   // This takes 1us, as we run SPI at 16 MHz:
   spi_write16_blocking (SPI_ID, &w, 1);

   if (++digit == N_DIGITS)
      digit = 0;

   return true;
}
