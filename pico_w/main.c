#include "dht.h"
#include "display.h"
#include "ntp.h"
#include "snmp.h"
#include "wifi.h"

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "pico/util/datetime.h"

#include "hardware/rtc.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

int
main ()
{
   stdio_init_all ();
   printf ("\nWiFi-clock\n");

   display_init ();

   rtc_init ();
   {
      datetime_t t =
         {
            .year = 2020, .month = 1, .day = 1, .dotw = 3,
            .hour = 0, .min = 0, .sec = 0
         };
      rtc_set_datetime (&t);
   }

   wifi_init ();

   ntp_t *ntp_state = ntp_init ();
   if (!ntp_state)
      return -1;

   snmp_t *snmp_state = snmp_init ();
   if (!snmp_state)
      return -1;

   dht_t *dht_state = dht_init ();
   if (!dht_state)
      return -1;

   while (1)
   {
      wifi_on_tick ();

      ntp_on_tick (ntp_state);
      dht_on_tick (dht_state);
      display_on_tick ();

      sleep_ms (20);
   }

   free (dht_state);
   free (snmp_state);
   free (ntp_state);
   cyw43_arch_deinit();
}
