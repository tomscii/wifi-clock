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

   return count > 26 ? 1 : 0;
}

static int
read_from_dht (dht_reading *result)
{
   uint8_t data [5] = {0, 0, 0, 0, 0};

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
      result->humidity = (float) ((data [0] << 8) + data [1]) / 10;
      if (result->humidity > 100)
         result->humidity = data[0];

      result->temp_celsius = (float) (((data [2] & 0x7F) << 8) + data [3]) / 10;
      if (result->temp_celsius > 125)
         result->temp_celsius = data[2];

      if (data [2] & 0x80)
         result->temp_celsius = -result->temp_celsius;

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
         printf ("DHT: Bad data\n");
         dht_state->next_read = make_timeout_time_ms (DHT_RETRY_INTERVAL_MS);
      }
   }
}
