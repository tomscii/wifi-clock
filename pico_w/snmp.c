#include "snmp.h"

#include "lwip/pbuf.h"
#include "lwip/udp.h"

#include <stdio.h>

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

snmp_t*
snmp_init ()
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
