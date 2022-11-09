#include "wifi.h"

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"

#include <stdio.h>

uint8_t wifi_status = WIFI_DOWN;

static int link_status = CYW43_LINK_DOWN;
static alarm_id_t wifi_connect_timeout_alarm = 0;

static void
netif_status_cb (struct netif* netif)
{
   printf ("netif %s is_up=%d is_link_up=%d, addr=%s\n",
           netif_get_hostname (netif), netif_is_up (netif),
           netif_is_link_up (netif), ip4addr_ntoa (netif_ip4_addr (netif)));
}

static int64_t
wifi_connect_timeout_cb (alarm_id_t id, void *user_data)
{
   printf ("WiFi connection attempt timed out\n");
   wifi_connect_timeout_alarm = 0;
   wifi_status = WIFI_ERROR;
   return 0;
}

void
wifi_init ()
{
   if (cyw43_arch_init_with_country (CYW43_COUNTRY_SWEDEN)) {
      printf("CYW43: Failed to initialise\n");
      return;
   }

   cyw43_arch_enable_sta_mode ();

   netif_set_status_callback (netif_default, netif_status_cb);
}

void
wifi_on_tick ()
{
   int new_link_status = cyw43_tcpip_link_status (&cyw43_state, CYW43_ITF_STA);
   if (new_link_status != link_status)
   {
      printf ("link_status -> %d\n", new_link_status);

      if (link_status == CYW43_LINK_UP)
         wifi_status = WIFI_ERROR;
      else if (new_link_status == CYW43_LINK_UP)
      {
         wifi_status = WIFI_UP;
         if (wifi_connect_timeout_alarm > 0)
         {
            cancel_alarm (wifi_connect_timeout_alarm);
            wifi_connect_timeout_alarm = 0;
         }
      }
      else if (new_link_status == CYW43_LINK_JOIN ||
               new_link_status == CYW43_LINK_NOIP)
         wifi_status = WIFI_CONNECTING;
      else if (new_link_status == CYW43_LINK_DOWN)
         wifi_status = WIFI_DOWN;
      else
         wifi_status = WIFI_ERROR;

      link_status = new_link_status;
   }

   if (wifi_status == WIFI_ERROR)
   {
      printf ("Network error, leaving wireless network...\n");
      cyw43_wifi_leave (&cyw43_state, CYW43_ITF_STA);
      wifi_status = WIFI_DOWN;
   }

   if (wifi_status == WIFI_DOWN)
   {
      if (cyw43_arch_wifi_connect_async (
             WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK))
      {
         printf ("cyw43_arch_wifi_connect_async failed\n");
      }
      else
      {
         printf ("Connecting to SSID '%s' with password '%s' ...\n",
                 WIFI_SSID, WIFI_PASSWORD);
      }
      wifi_status = WIFI_CONNECTING;
      wifi_connect_timeout_alarm =
         add_alarm_in_ms (WIFI_CONNECT_TIMEOUT_MS, wifi_connect_timeout_cb,
                          NULL, true);
   }

   cyw43_arch_poll ();
}
