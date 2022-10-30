#include <stdio.h>
#include <string.h>
#include <time.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"

#include "hardware/gpio.h"
#include "hardware/spi.h"

#include "lwip/dns.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"

typedef struct ntp_t_
{
   ip_addr_t ntp_server_address;
   bool dns_request_sent;
   struct udp_pcb *ntp_pcb;
   absolute_time_t next_req;
   alarm_id_t ntp_resend_alarm;
} ntp_t;

typedef struct dht_t_
{
   absolute_time_t next_read;
} dht_t;

// NTP
#define NTP_SERVER     "pool.ntp.org"
#define NTP_MSG_LEN                48
#define NTP_PORT                  123
// seconds between 1 Jan 1900 and 1 Jan 1970:
#define NTP_DELTA          2208988800
#define NTP_REQ_INTERVAL  (60 * 1000)
#define NTP_REQ_TIMEOUT   (15 * 1000)

// SPI
#define SPI_BAUDRATE         16000000
#define SPI_ID                   spi0

// DHT22 sensor
#define DHT_GPIO                   16
#define DHT_POLL_INTERVAL  (5 * 1000)
#define DHT_RETRY_INTERVAL (1 * 1000)

// Multiplexed 7-segment display
#define N_DIGITS         6

uint8_t digit = 0;
uint8_t display [N_DIGITS] = { 0x55, 0xAA, 0x12, 0x34, 0x56, 0x78 };

repeating_timer_t rt_display_update;
bool
update_display (repeating_timer_t *rt)
{
   uint8_t v = 1 << digit;
   uint16_t w = (display [digit] << 8) | v;

   // This takes 1us, as we run SPI at 16 MHz:
   spi_write16_blocking (SPI_ID, &w, 1);

   if (++digit == N_DIGITS)
      digit = 0;

   return true;
}

// Called with results of operation
static void
ntp_result (ntp_t* state, int status, time_t *result)
{
   if (status == 0 && result)
   {
      struct tm *utc = gmtime(result);
      printf ("Got NTP response: %04d-%02d-%02d %02d:%02d:%02d\n",
              utc->tm_year + 1900, utc->tm_mon + 1, utc->tm_mday,
              utc->tm_hour, utc->tm_min, utc->tm_sec);
   }

   if (state->ntp_resend_alarm > 0)
   {
      cancel_alarm (state->ntp_resend_alarm);
      state->ntp_resend_alarm = 0;
   }
   state->next_req = make_timeout_time_ms (NTP_REQ_INTERVAL);
   state->dns_request_sent = false;
}

// Make an NTP request
static void
ntp_request (ntp_t *state)
{
   // cyw43_arch_lwip_begin/end should be used around calls into lwIP to ensure correct locking.
   // You can omit them if you are in a callback from lwIP. Note that when using pico_cyw_arch_poll
   // these calls are a no-op and can be omitted, but it is a good practice to use them in
   // case you switch the cyw43_arch type later.
   cyw43_arch_lwip_begin ();
   struct pbuf *p = pbuf_alloc (PBUF_TRANSPORT, NTP_MSG_LEN, PBUF_RAM);
   uint8_t *req = (uint8_t *) p->payload;
   memset (req, 0, NTP_MSG_LEN);
   req [0] = 0x1b;
   udp_sendto (state->ntp_pcb, p, &state->ntp_server_address, NTP_PORT);
   pbuf_free (p);
   cyw43_arch_lwip_end ();
}

static int64_t
ntp_failed_handler (alarm_id_t id, void *user_data)
{
   ntp_t* state = (ntp_t*) user_data;
   printf ("NTP request timed out\n");
   ntp_result (state, -1, NULL);
   return 0;
}

// Callback with a DNS result
static void
ntp_dns_found (const char *hostname, const ip_addr_t *ipaddr, void *arg)
{
   ntp_t *state = (ntp_t*) arg;
   if (ipaddr)
   {
      state->ntp_server_address = *ipaddr;
      printf ("NTP server address: %s\n", ip4addr_ntoa (ipaddr));
      ntp_request (state);
   }
   else
   {
      printf ("NTP DNS request failed\n");
      ntp_result (state, -1, NULL);
   }
}

// NTP data received
static void
ntp_recv (void *arg, struct udp_pcb *pcb, struct pbuf *p,
          const ip_addr_t *addr, u16_t port)
{
   ntp_t *state = (ntp_t*) arg;
   uint8_t mode = pbuf_get_at (p, 0) & 0x7;
   uint8_t stratum = pbuf_get_at (p, 1);

   // Check the result
   if (ip_addr_cmp (addr, &state->ntp_server_address) && port == NTP_PORT &&
       p->tot_len == NTP_MSG_LEN && mode == 0x4 && stratum != 0)
   {
      uint8_t seconds_buf [4] = {0};
      pbuf_copy_partial (p, seconds_buf, sizeof (seconds_buf), 40);
      uint32_t seconds_since_1900 =
         seconds_buf [0] << 24 | seconds_buf [1] << 16 |
         seconds_buf [2] << 8 | seconds_buf [3];
      uint32_t seconds_since_1970 = seconds_since_1900 - NTP_DELTA;
      time_t epoch = seconds_since_1970;
      ntp_result (state, 0, &epoch);
   }
   else
   {
      printf ("Invalid NTP response\n");
      ntp_result (state, -1, NULL);
   }
   pbuf_free (p);
}

// Perform initialisation
static ntp_t*
ntp_init (void)
{
   ntp_t *state = calloc (1, sizeof (ntp_t));
   if (!state)
   {
      printf ("Failed to allocate ntp_t\n");
      return NULL;
   }
   state->ntp_pcb = udp_new_ip_type (IPADDR_TYPE_ANY);
   if (!state->ntp_pcb)
   {
      printf ("Failed to create ntp_pcb\n");
      free(state);
      return NULL;
   }
   udp_recv (state->ntp_pcb, ntp_recv, state);
   return state;
}

static dht_t*
dht_init (void)
{
   dht_t *state = calloc (1, sizeof (dht_t));
   if (!state)
   {
      printf ("Failed to allocate dht_t\n");
      return NULL;
   }
   return state;
}


typedef struct
{
   float humidity;
   float temp_celsius;
} dht_reading;

int
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

int
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

int
main ()
{
   stdio_init_all ();
   printf ("\nWiFi-clock\n");

   spi_init (SPI_ID, SPI_BAUDRATE);
   spi_set_format (SPI_ID, 16, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
   gpio_set_function (PICO_DEFAULT_SPI_SCK_PIN, GPIO_FUNC_SPI);
   gpio_set_function (PICO_DEFAULT_SPI_TX_PIN, GPIO_FUNC_SPI);
   gpio_set_function(PICO_DEFAULT_SPI_CSN_PIN, GPIO_FUNC_SPI);

   gpio_init (DHT_GPIO);
   gpio_pull_up (DHT_GPIO);

   add_repeating_timer_ms (-2, update_display, NULL, &rt_display_update);

   if (cyw43_arch_init_with_country (CYW43_COUNTRY_SWEDEN)) {
      printf("CYW43: Failed to initialise\n");
      return 1;
   }

   cyw43_arch_enable_sta_mode ();

   printf ("Connecting to wireless network '%s' with password '%s' ...\n",
           WIFI_SSID, WIFI_PASSWORD);
   if (cyw43_arch_wifi_connect_timeout_ms (
          WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, 10000))
   {
      printf("Connection timed out.\n");
      return 1;
   }
   printf ("Connected.\n");

   ntp_t *ntp_state = ntp_init ();
   if (!ntp_state)
      return -1;

   dht_t *dht_state = dht_init ();
   if (!dht_state)
      return -1;

   while (1)
   {
      if (absolute_time_diff_us (get_absolute_time(), ntp_state->next_req) < 0 &&
          !ntp_state->dns_request_sent)
      {
         // Set alarm in case udp requests are lost
         ntp_state->ntp_resend_alarm =
            add_alarm_in_ms (NTP_REQ_TIMEOUT, ntp_failed_handler, ntp_state,
                             true);

         // cyw43_arch_lwip_begin/end should be used around calls into lwIP to ensure correct locking.
         // You can omit them if you are in a callback from lwIP. Note that when using pico_cyw_arch_poll
         // these calls are a no-op and can be omitted, but it is a good practice to use them in
         // case you switch the cyw43_arch type later.
         cyw43_arch_lwip_begin ();
         int err = dns_gethostbyname (NTP_SERVER,
                                      &ntp_state->ntp_server_address,
                                      ntp_dns_found, ntp_state);
         cyw43_arch_lwip_end ();

         ntp_state->dns_request_sent = true;

         if (err == ERR_OK)
         {
            ntp_request (ntp_state); // Cached result
         }
         else if (err != ERR_INPROGRESS)
         { // ERR_INPROGRESS means expect a callback
            printf ("DNS request failed\n");
            ntp_result (ntp_state, -1, NULL);
         }
      }
#if PICO_CYW43_ARCH_POLL
      // if you are using pico_cyw43_arch_poll, then you must poll periodically from your
      // main loop (not from a timer) to check for WiFi driver or lwIP work that needs to be done.
      cyw43_arch_poll ();
      //sleep_ms(1);
#else
      // if you are not using pico_cyw43_arch_poll, then WiFI driver and lwIP work
      // is done via interrupt in the background. This sleep is just an example of some (blocking)
      // work you might be doing.
      //sleep_ms(1000);
#endif

      if (absolute_time_diff_us (get_absolute_time(), dht_state->next_read) < 0)
      {
         dht_reading reading;
         if (read_from_dht (&reading) == 0)
         {
            printf("Humidity = %.1f%%, Temperature = %.1f*C\n",
                   reading.humidity, reading.temp_celsius);

            dht_state->next_read = make_timeout_time_ms (DHT_POLL_INTERVAL);
         }
         else
         {
            printf("DHT: Bad data\n");
            dht_state->next_read = make_timeout_time_ms (DHT_RETRY_INTERVAL);
         }
      }

      sleep_ms (1);
   }

   free (dht_state);
   free (ntp_state);
   cyw43_arch_deinit();
}
