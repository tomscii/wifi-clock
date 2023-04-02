#include "ntp.h"
#include "wifi.h"

#include "dst_table.h"

#include "hardware/rtc.h"

#include "lwip/dns.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"

#include <stdio.h>
#include <time.h>

static time_t rtc_setting = 0;

static int64_t
rtc_set_cb (alarm_id_t id, void* data)
{
   ntp_t* state = (ntp_t*) data;
   struct tm* utc = gmtime (&rtc_setting);
   int year = utc->tm_year + 1900;
   int tz_offset = 3600;
   char* tz = "CET";
   int next_dst_change_in = 99999999;
   if (year >= dst_table_start && year < dst_table_start + dst_table_years)
   {
      int idx = 2 * (year - dst_table_start);
      time_t dst_start = dst_table [idx];
      time_t dst_end = dst_table [idx + 1];
      if (rtc_setting < dst_start)
      {
         next_dst_change_in = dst_start - rtc_setting;
      }
      else if (rtc_setting < dst_end)
      {
         next_dst_change_in = dst_end - rtc_setting;
         tz_offset += 3600;
         tz = "CEST";
      }
      else
      {
         // Next year's DST start is surely more than a few
         // NTP_REQ_INTERVAL_MS away...
      }
   }

   time_t localtime = rtc_setting + tz_offset;
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

   printf ("RTC set to: %04d-%02d-%02d %02d:%02d:%02d %s\n",
           tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
           tm->tm_hour, tm->tm_min, tm->tm_sec, tz);

   if (next_dst_change_in < NTP_REQ_INTERVAL_MS / 1000)
   {
      state->next_req = make_timeout_time_ms (1000 * next_dst_change_in);
      printf ("Next DST change in %d seconds, re-scheduling NTP request\n",
              next_dst_change_in);
   }

   return 0;
}

// Called with results of operation
static void
ntp_result (ntp_t* state, int status, time_t* result, double* frac)
{
   if (status == 0 && result && frac)
   {
      struct tm *utc = gmtime (result);
      printf ("Got NTP response: %04d-%02d-%02d %02d:%02d:%2.6f UTC\n",
              utc->tm_year + 1900, utc->tm_mon + 1, utc->tm_mday,
              utc->tm_hour, utc->tm_min, utc->tm_sec + *frac);

      // schedule setting of the RTC at the next whole second:
      uint32_t rtc_set_delay_ms = 1000 * (1.0 - *frac);
      rtc_setting = *result;
      add_alarm_in_ms (rtc_set_delay_ms, rtc_set_cb, state, true);
      state->fail_count = 0;
      // N.B.: will possibly revise the deadline in rtc_set_cb
      // in case of an upcoming DST transition:
      state->next_req = make_timeout_time_ms (NTP_REQ_INTERVAL_MS);
   }
   else
   {
      if (state->fail_count++ > 3)
         wifi_status = WIFI_ERROR;
      state->next_req = make_timeout_time_ms (NTP_REQ_TIMEOUT_MS);
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

ntp_t*
ntp_init ()
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

void
ntp_on_tick (ntp_t* ntp_state)
{
   if (wifi_status == WIFI_UP &&
       absolute_time_diff_us (get_absolute_time (), ntp_state->next_req) < 0 &&
       !ntp_state->dns_request_sent)
   {
      // Set alarm in case udp requests are lost
      ntp_state->timeout_alarm =
         add_alarm_in_ms (NTP_REQ_TIMEOUT_MS, ntp_timeout_cb, ntp_state, true);

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
}
