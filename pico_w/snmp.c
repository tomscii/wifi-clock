#include "snmp.h"

#include "lwip/pbuf.h"
#include "lwip/udp.h"

#include <stdio.h>

typedef void (* entry_value_fn) (uint8_t * buf, uint8_t * pos, void * data);
typedef struct snmp_mib_entry_t_
{
   uint8_t * oid;
   uint8_t oid_size;
   entry_value_fn value_fn;
   void * data;
} snmp_mib_entry_t;

static uint8_t counter = 0;

void
fn_octet_string (uint8_t * buf, uint8_t * pos, void * data)
{
   uint8_t * str = (uint8_t *)data;
   uint8_t len = strlen (str);
   buf [(*pos)++] = 0x04;
   buf [(*pos)++] = len;
   memcpy (buf + *pos, str, len);
   *pos += len;
}

void
fn_counter (uint8_t * buf, uint8_t * pos, void * data)
{
   buf [(*pos)++] = 0x02;
   buf [(*pos)++] = 0x01;
   buf [(*pos)++] = counter++;
}

// For simplicity, and because it will work just as well in
// comparisons, we use on-the-wire OID encoding:
#define ENTITY_SENSOR_MIB     "\x2b\x06\x01\x02\x01\x63"
#define ENTITY_SENSOR_OBJECTS (ENTITY_SENSOR_MIB "\x01")
#define ENTITY_SENSOR_BLAH    (ENTITY_SENSOR_MIB "\x02")

static const snmp_mib_entry_t mib [] =
{
   {
      ENTITY_SENSOR_OBJECTS, sizeof (ENTITY_SENSOR_OBJECTS) - 1,
      fn_counter, NULL
   },
   {
      ENTITY_SENSOR_BLAH, sizeof (ENTITY_SENSOR_BLAH) - 1,
      fn_octet_string, "Blah blah blah."
   }
};

#define N_MIB_ENTRIES (sizeof (mib) / sizeof (mib [0]))

static const snmp_mib_entry_t *
find_mib_entry (uint8_t * oid, uint8_t oid_size)
{
   for (int k = 0; k < N_MIB_ENTRIES; ++k)
   {
      if (oid_size == mib [k].oid_size &&
          strncmp (oid, mib [k].oid, oid_size) == 0)
         return & mib [k];
   }

   return NULL;
}

static void
snmp_recv_done (struct pbuf *p)
{
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

   // N.B.: we can get away treating length fields as single bytes,
   // as we do not need to support packets longer than 129 bytes
   // (with the packet length byte containing 127, the largest
   // possible single-byte value).

   if (p->len < 23)
      return snmp_recv_done (p);

   uint8_t * data = p->payload;
   if (data [0] != 0x30)
      return snmp_recv_done (p);

   uint8_t pktlen = data [1];
   if (pktlen + 2 != p->len)
      return snmp_recv_done (p);

   if (pbuf_memcmp (p, 2, "\x02\x01\x01\x04", 4))
      return snmp_recv_done (p);

   uint8_t community_len = data [6];
   if (p->len < 7 + community_len + 16)
      return snmp_recv_done (p);
   if (community_len != strlen (SNMP_RO_COMMUNITY))
      return snmp_recv_done (p);
   if (pbuf_memcmp (p, 7, SNMP_RO_COMMUNITY, community_len))
      return snmp_recv_done (p);

   uint8_t * req = data + 7 + community_len;
   uint8_t req_len = req [1];
   if (9 + community_len + req_len != p->len)
      return snmp_recv_done (p);
   if (req_len < 14)
      return snmp_recv_done (p);

   if (req [2] != 0x02)
      return snmp_recv_done (p);
   uint8_t req_id_len = req [3];
   if (req_id_len > 4)
      return snmp_recv_done (p);
   uint8_t * req2 = req + 4 + req_id_len;
   if (memcmp (req2, "\x02\x01\x00\x02\x01\x00\x30", 7))
      return snmp_recv_done (p);

   uint8_t bindings_len = req2 [7];
   if (19 + community_len + req_id_len + bindings_len != p->len)
      return snmp_recv_done (p);

   switch (req [0])
   {
   case 0xa0:
      printf ("get\n");
      break;
   case 0xa1:
      printf ("get-next\n");
      break;
   default:
      return snmp_recv_done (p);
   }

#define APPEND_STR(STR, LEN)                    \
   if (129 < rp + LEN)                          \
      return snmp_recv_done (p);                \
   memcpy (response + rp, STR, LEN);            \
   rp += LEN
#define APPEND_BYTE(VAL)                        \
   if (129 < rp + 1)                            \
      return snmp_recv_done (p);                \
   response [rp++] = VAL
#define DECL_LENGTH(VAR)                        \
   uint8_t VAR = rp++;
#define UPDATE_LENGTH(VAR)                      \
   response [VAR] = rp - VAR - 1;

   uint8_t response [129];
   uint8_t rp = 18 + community_len + req_id_len;
   memcpy (response, data, rp);
   response [7 + community_len] = 0xa2;
   uint8_t pdu_len_idx = 8 + community_len;
   DECL_LENGTH (bindings_len_idx);

   uint8_t * bind = req2 + 8;
   do
   {
      if (bindings_len < 2)
         return snmp_recv_done (p);

      if (bind [0] != 0x30)
         return snmp_recv_done (p);

      uint8_t bind_len = bind [1];
      if (bindings_len < bind_len + 2)
         return snmp_recv_done (p);

      if (bind [2] != 0x06)
         return snmp_recv_done (p);
      uint8_t oid_len = bind [3];
      if (oid_len + 4 != bind_len)
         return snmp_recv_done (p);
      if (bind [bind_len] != 0x05 || bind [bind_len + 1] != 0x00)
         return snmp_recv_done (p);

      uint8_t * oid = bind + 4;
      printf ("oid=%x [", oid_len);
      for (int k = 0; k < oid_len; ++k)
         printf ("%02x ", oid [k]);
      printf ("]\n");

      APPEND_BYTE (0x30);
      DECL_LENGTH (bind_len_idx);
      APPEND_BYTE (0x06);
      APPEND_BYTE (oid_len);
      APPEND_STR (oid, oid_len);

      const snmp_mib_entry_t * e = find_mib_entry (oid, oid_len);
      if (e)
      {
         e->value_fn (response, &rp, e->data);
      }
      else
      {
         APPEND_BYTE (0x80);
         APPEND_BYTE (0x00);
      }

      UPDATE_LENGTH (bind_len_idx);
      bind += bind_len + 2;
      bindings_len -= bind_len + 2;
   } while (bindings_len);

   UPDATE_LENGTH (1);
   UPDATE_LENGTH (pdu_len_idx);
   UPDATE_LENGTH (bindings_len_idx);

   struct pbuf * r = pbuf_alloc (PBUF_TRANSPORT, rp, PBUF_RAM);
   uint8_t * rb = (uint8_t *) r->payload;
   memcpy (r->payload, response, rp);
   udp_sendto (pcb, r, addr, port);
   pbuf_free (r);

   return snmp_recv_done (p);
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
