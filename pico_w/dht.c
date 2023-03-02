#include "dht.h"
#include "snmp.h"

#include "hardware/gpio.h"

#include <stdio.h>
#include <stdlib.h>

dht_t*
dht_init ()
{
   gpio_init (DHT_GPIO);
   gpio_pull_up (DHT_GPIO);

   dht_t *state = calloc (1, sizeof (dht_t));
   if (!state)
   {
      printf ("Failed to allocate dht_t\n");
      return NULL;
   }
   return state;
}

static int
read_dht_bit ()
{
   uint8_t count = 0;
   while (gpio_get (DHT_GPIO) == 0)
   {
      if (++count > 100)
         return 0;
      sleep_us (1);
   }

   count = 0;
   while (gpio_get (DHT_GPIO) == 1)
   {
      if (++count > 100)
         return 0;
      sleep_us (1);
   }

   return count > 30 ? 1 : 0;
}

static int
read_from_dht (dht_reading *result)
{
   uint8_t* data = result->data;

   gpio_set_dir (DHT_GPIO, GPIO_OUT);
   gpio_put (DHT_GPIO, 0);
   sleep_ms (2);
   gpio_put (DHT_GPIO, 1);
   gpio_set_dir (DHT_GPIO, GPIO_IN);

   read_dht_bit (); // MCU pull-up
   read_dht_bit (); // sensor start bit

   for (int byte = 0; byte < 5; ++byte)
   {
      for (int bit = 0; bit < 8; ++bit)
      {
         data [byte] <<= 1;
         data [byte] |= read_dht_bit ();
      }
   }

 #if 0
   printf ("%02x %02x %02x %02x %02x\n",
           data [0], data [1], data [2], data [3], data [4]);
 #endif

   uint sum = data [0] + data [1] + data [2] + data [3];

   if (sum > 0 && data [4] == (sum & 0xff))
   {
      uint16_t rh = (data [0] << 8) | data [1];
      result->humidity = (float) rh / 10;

 /* ACHTUNG!
  *
  * I have observed TWO kinds of nominally DHT22-compatible sensors in the wild:
  * ONE that has a sign bit in front of the (unsigned / positive) temperature
  * value, and ANOTHER that emits a proper two's complement temperature value.
  *
  * For positive temperatures, there is no difference.
  *
  * As illustration, the temperature -7.2*C would be encoded like this:
  *
  *    RAW MEASUREMENT DATA    TEMPERATURE
  *    data [2]   data [3]      IN CELSIUS
  *    -----------------------------------
  *     0x80       0x48            -7.2    (sign bit + positive: 0x48 -> 72)
  *     0xff       0xb8            -7.2    (2's complement encoding: -72)
  *
  * Choose your poison.
  */
 #if 0
      /* Positive 15-bit value preceded by sign bit */
      uint16_t traw = (data [2] << 8) | data [3];
      int16_t temp = traw & 0x7fff;
      if (traw & 0x8000)
         temp *= -1;
 #else
      /* Two's complement value on 16 bits */
      uint16_t traw = (data [2] << 8) | data [3];
      int16_t temp = traw;
 #endif
      result->temp_celsius = (float) temp / 10;

      return 0;
   }

   return -1;
}

void
dht_on_tick (dht_t* dht_state)
{
   if (absolute_time_diff_us (get_absolute_time(), dht_state->next_read) < 0)
   {
      dht_reading reading;
      if (read_from_dht (&reading) == 0)
      {
         snmp_temperature = 10 * reading.temp_celsius;
         snmp_percent_rh = 10 * reading.humidity;

         printf ("%.1f*C %.1f%%RH  ",
                 reading.temp_celsius, reading.humidity);

         dht_state->next_read = make_timeout_time_ms (DHT_POLL_INTERVAL_MS);
      }
      else
      {
         uint8_t* data = reading.data;
         printf ("DHT: Bad data: %02x %02x %02x %02x %02x\n",
                 data [0], data [1], data [2], data [3], data [4]);
         dht_state->next_read = make_timeout_time_ms (DHT_RETRY_INTERVAL_MS);
      }
   }
}
