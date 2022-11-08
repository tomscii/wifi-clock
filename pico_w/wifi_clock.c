#include <stdio.h>
#include <string.h>
#include <time.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "pico/util/datetime.h"

#include "hardware/gpio.h"
#include "hardware/rtc.h"
#include "hardware/spi.h"

#include "lwip/dns.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"

enum wifi_status_t { WIFI_DOWN, WIFI_CONNECTING, WIFI_UP, WIFI_ERROR };
uint8_t wifi_status = WIFI_DOWN;
alarm_id_t wifi_connect_timeout_alarm = 0;

typedef struct ntp_t_
{
   ip_addr_t ntp_server_address;
   bool dns_request_sent;
   struct udp_pcb *pcb;
   absolute_time_t next_req;
   alarm_id_t timeout_alarm;
   int fail_count;
} ntp_t;

typedef struct snmp_t_
{
   struct udp_pcb *pcb;
} snmp_t;

typedef struct dht_t_
{
   absolute_time_t next_read;
} dht_t;

// WIFI
#define WIFI_CONNECT_TIMEOUT      (10 * 1000)

// NTP
#define NTP_SERVER      "europe.pool.ntp.org"
#define NTP_MSG_LEN                        48
#define NTP_PORT                          123
// seconds between 1 Jan 1900 and 1 Jan 1970:
#define NTP_DELTA                  2208988800
#define NTP_REQ_INTERVAL         (600 * 1000)
#define NTP_REQ_TIMEOUT            (5 * 1000)

// SNMP
#define SNMP_PORT                         161

// SPI
#define SPI_BAUDRATE                 16000000
#define SPI_ID                           spi0

// DHT22 sensor
#define DHT_GPIO                           16
#define DHT_POLL_INTERVAL         (15 * 1000)
#define DHT_RETRY_INTERVAL         (1 * 1000)

// Multiplexed 7-segment display
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

uint8_t digit = 0;
uint8_t display [N_DIGITS] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

uint8_t
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

repeating_timer_t rt_display_update;
bool
update_display (repeating_timer_t *rt)
{
   // Digit select is active high (common anode),
   // the lowest two digits are not connected:
   uint8_t v = 4 << digit;
   // MSB gets shifted out first (driving SEG_G);
   // LSB is the lowermost unconnected digit-driving bit.
   uint16_t w = (display [digit] << 8) | v;

   // This takes 1us, as we run SPI at 16 MHz:
   spi_write16_blocking (SPI_ID, &w, 1);

   if (++digit == N_DIGITS)
      digit = 0;

   return true;
}

static int64_t
wifi_connect_timeout_cb (alarm_id_t id, void *user_data)
{
   printf ("WiFi connection attempt timed out\n");
   wifi_connect_timeout_alarm = 0;
   wifi_status = WIFI_ERROR;
   return 0;
}

time_t rtc_setting = 0;

int64_t
rtc_set_cb (alarm_id_t id, void* data)
{
   // TODO add one more hour within DST interval
   time_t localtime = rtc_setting + 3600;
   struct tm* tm = gmtime (&localtime);

   datetime_t t =
      {
         .year = tm->tm_year + 1900,
         .month = tm->tm_mon + 1,
         .day = tm->tm_mday,
         .dotw = tm->tm_wday,
         .hour = tm->tm_hour,
         .min = tm->tm_min,
         .sec = tm->tm_sec
      };
   rtc_set_datetime (&t);

   printf ("RTC set to: %04d-%02d-%02d %02d:%02d:%02d\n",
           tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
           tm->tm_hour, tm->tm_min, tm->tm_sec);

   return 0;
}

// Called with results of operation
static void
ntp_result (ntp_t* state, int status, time_t* result, double* frac)
{
   if (status == 0 && result && frac)
   {
      struct tm *utc = gmtime (result);
      printf ("Got NTP response: %04d-%02d-%02d %02d:%02d:%2.6f\n",
              utc->tm_year + 1900, utc->tm_mon + 1, utc->tm_mday,
              utc->tm_hour, utc->tm_min, utc->tm_sec + *frac);

      // schedule setting of the RTC at the next whole second:
      uint32_t rtc_set_delay_ms = 1000 * (1.0 - *frac);
      rtc_setting = *result;
      add_alarm_in_ms (rtc_set_delay_ms, rtc_set_cb, NULL, false);
      state->fail_count = 0;
      state->next_req = make_timeout_time_ms (NTP_REQ_INTERVAL);
   }
   else
   {
      if (state->fail_count++ > 3)
         wifi_status = WIFI_ERROR;
      state->next_req = make_timeout_time_ms (NTP_REQ_TIMEOUT);
   }

   if (state->timeout_alarm > 0)
   {
      cancel_alarm (state->timeout_alarm);
      state->timeout_alarm = 0;
   }
   state->dns_request_sent = false;
}

// Make an NTP request
static void
ntp_request (ntp_t *state)
{
   struct pbuf *p = pbuf_alloc (PBUF_TRANSPORT, NTP_MSG_LEN, PBUF_RAM);
   uint8_t *req = (uint8_t *) p->payload;
   memset (req, 0, NTP_MSG_LEN);
   req [0] = 0x1b;
   udp_sendto (state->pcb, p, &state->ntp_server_address, NTP_PORT);
   pbuf_free (p);
}

static int64_t
ntp_timeout_cb (alarm_id_t id, void *user_data)
{
   ntp_t* state = (ntp_t*) user_data;
   printf ("NTP request timed out\n");
   ntp_result (state, -1, NULL, NULL);
   return 0;
}

// Callback with a DNS result
static void
dns_cb (const char *hostname, const ip_addr_t *ipaddr, void *arg)
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
      ntp_result (state, -1, NULL, NULL);
      wifi_status = WIFI_ERROR;
   }
}

// NTP data received
static void
ntp_recv_cb (void *arg, struct udp_pcb *pcb, struct pbuf *p,
             const ip_addr_t *addr, u16_t port)
{
   ntp_t *state = (ntp_t*) arg;
   uint8_t mode = pbuf_get_at (p, 0) & 0x7;
   uint8_t stratum = pbuf_get_at (p, 1);

   // Check the result
   if (ip_addr_cmp (addr, &state->ntp_server_address) && port == NTP_PORT &&
       p->tot_len == NTP_MSG_LEN && mode == 0x4 && stratum != 0)
   {
      uint8_t buf [8] = {0};
      pbuf_copy_partial (p, buf, sizeof (buf), 0x28);
      uint32_t seconds_since_1900 =
         buf [0] << 24 | buf [1] << 16 | buf [2] << 8 | buf [3];
      uint32_t seconds_since_1970 = seconds_since_1900 - NTP_DELTA;
      time_t epoch = seconds_since_1970;
      uint32_t frac_seconds =
         buf [4] << 24 | buf [5] << 16 | buf [6] << 8 | buf [7];
      double frac = (double)frac_seconds / (1ULL << 32);
      ntp_result (state, 0, &epoch, &frac);
   }
   else
   {
      printf ("Invalid NTP response\n");
      ntp_result (state, -1, NULL, NULL);
   }
   pbuf_free (p);
}

static void
snmp_recv_cb (void *arg, struct udp_pcb *pcb, struct pbuf *p,
              const ip_addr_t *addr, u16_t port)
{
   snmp_t *state = (snmp_t*) arg;
   printf ("SNMP recv: len=%x [", p->len);
   for (int k = 0; k < p->len; ++k)
      printf ("%02x ", pbuf_get_at (p, k));
   printf ("]\n");
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
   state->pcb = udp_new_ip_type (IPADDR_TYPE_ANY);
   if (!state->pcb)
   {
      printf ("Failed to create ntp pcb\n");
      free (state);
      return NULL;
   }
   udp_recv (state->pcb, ntp_recv_cb, state);
   return state;
}

static snmp_t*
snmp_init (void)
{
   snmp_t *state = calloc (1, sizeof (snmp_t));
   if (!state)
   {
      printf ("Failed to allocate snmp_t\n");
      return NULL;
   }
   state->pcb = udp_new_ip_type (IPADDR_TYPE_ANY);
   if (udp_bind (state->pcb, IP4_ADDR_ANY, SNMP_PORT) != ERR_OK)
   {
      printf ("udp_bind error\n");
      free (state);
      return NULL;
   }
   udp_recv (state->pcb, snmp_recv_cb, state);
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

void
netif_status_cb (struct netif* netif)
{
   printf ("netif %s is_up=%d is_link_up=%d, addr=%s\n",
           netif_get_hostname (netif), netif_is_up (netif),
           netif_is_link_up (netif), ip4addr_ntoa (netif_ip4_addr (netif)));
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

   rtc_init ();
   datetime_t t =
   {
      .year = 2020, .month = 1, .day = 1, .dotw = 3,
      .hour = 0, .min = 0, .sec = 0
   };
   rtc_set_datetime (&t);

   add_repeating_timer_ms (-2, update_display, NULL, &rt_display_update);

   if (cyw43_arch_init_with_country (CYW43_COUNTRY_SWEDEN)) {
      printf("CYW43: Failed to initialise\n");
      return 1;
   }

   cyw43_arch_enable_sta_mode ();

   netif_set_status_callback (netif_default, netif_status_cb);

   ntp_t *ntp_state = ntp_init ();
   if (!ntp_state)
      return -1;

   snmp_t *snmp_state = snmp_init ();
   if (!snmp_state)
      return -1;

   dht_t *dht_state = dht_init ();
   if (!dht_state)
      return -1;

   int link_status = CYW43_LINK_DOWN;
   while (1)
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
            add_alarm_in_ms (WIFI_CONNECT_TIMEOUT, wifi_connect_timeout_cb,
                             NULL, true);
      }

      if (wifi_status == WIFI_UP &&
          absolute_time_diff_us (get_absolute_time (), ntp_state->next_req) < 0 &&
          !ntp_state->dns_request_sent)
      {
         // Set alarm in case udp requests are lost
         ntp_state->timeout_alarm =
            add_alarm_in_ms (NTP_REQ_TIMEOUT, ntp_timeout_cb, ntp_state, true);

         int err = dns_gethostbyname (NTP_SERVER,
                                      &ntp_state->ntp_server_address,
                                      dns_cb, ntp_state);

         ntp_state->dns_request_sent = true;

         if (err == ERR_OK)
         {
            ntp_request (ntp_state); // Cached result
         }
         else if (err != ERR_INPROGRESS)
         { // ERR_INPROGRESS means expect a callback
            printf ("DNS request failed\n");
            ntp_result (ntp_state, -1, NULL, NULL);
         }
      }

      if (absolute_time_diff_us (get_absolute_time(), dht_state->next_read) < 0)
      {
         dht_reading reading;
         if (read_from_dht (&reading) == 0)
         {
            printf ("%.1f*C %.1f%%RH  ",
                    reading.temp_celsius, reading.humidity);

            dht_state->next_read = make_timeout_time_ms (DHT_POLL_INTERVAL);
         }
         else
         {
            printf ("DHT: Bad data\n");
            dht_state->next_read = make_timeout_time_ms (DHT_RETRY_INTERVAL);
         }
      }

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

      cyw43_arch_poll ();
      sleep_ms (20);
   }

   free (dht_state);
   free (snmp_state);
   free (ntp_state);
   cyw43_arch_deinit();
}
