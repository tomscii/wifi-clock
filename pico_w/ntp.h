#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "pico/util/datetime.h"

#define NTP_SERVER      "europe.pool.ntp.org"
#define NTP_MSG_LEN                        48
#define NTP_PORT                          123
// seconds between 1 Jan 1900 and 1 Jan 1970:
#define NTP_DELTA                  2208988800
#define NTP_REQ_INTERVAL_MS      (900 * 1000)
#define NTP_REQ_TIMEOUT_MS         (5 * 1000)

typedef struct ntp_t_
{
   struct udp_pcb *pcb;
   bool dns_request_sent;
   ip_addr_t ntp_server_address;
   absolute_time_t next_req;
   alarm_id_t timeout_alarm;
   int fail_count;
} ntp_t;

ntp_t* ntp_init ();
void ntp_on_tick (ntp_t*);
