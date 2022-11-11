#include "snmp.h"

#include "lwip/pbuf.h"
#include "lwip/udp.h"

#include <stdio.h>

// For simplicity, and because it will work just as well in
// comparisons, we use on-the-wire OID encoding:
#define ENTITY_SENSOR_MIB       "\x2b\x06\x01\x02\x01\x63"
#define ENTITY_SENSOR_OBJS      ENTITY_SENSOR_MIB "\x01"
#define ENTITY_SENSOR_TABLE     ENTITY_SENSOR_OBJS "\x01"
#define ENTITY_SENSOR_ENTRY     ENTITY_SENSOR_TABLE "\x01"
#define ENTITY_SENSOR_TYPE      ENTITY_SENSOR_ENTRY "\x01"
#define ENTITY_SENSOR_TYPE_1    ENTITY_SENSOR_TYPE "\x01"
#define ENTITY_SENSOR_TYPE_2    ENTITY_SENSOR_TYPE "\x02"
#define ENTITY_SENSOR_SCALE     ENTITY_SENSOR_ENTRY "\x02"
#define ENTITY_SENSOR_SCALE_1   ENTITY_SENSOR_SCALE "\x01"
#define ENTITY_SENSOR_SCALE_2   ENTITY_SENSOR_SCALE "\x02"
#define ENTITY_SENSOR_PREC      ENTITY_SENSOR_ENTRY "\x03"
#define ENTITY_SENSOR_PREC_1    ENTITY_SENSOR_PREC "\x01"
#define ENTITY_SENSOR_PREC_2    ENTITY_SENSOR_PREC "\x02"
#define ENTITY_SENSOR_VALUE     ENTITY_SENSOR_ENTRY "\x04"
#define ENTITY_SENSOR_VALUE_1   ENTITY_SENSOR_VALUE "\x01"
#define ENTITY_SENSOR_VALUE_2   ENTITY_SENSOR_VALUE "\x02"
#define ENTITY_SENSOR_OPSTAT    ENTITY_SENSOR_ENTRY "\x05"
#define ENTITY_SENSOR_OPSTAT_1  ENTITY_SENSOR_OPSTAT "\x01"
#define ENTITY_SENSOR_OPSTAT_2  ENTITY_SENSOR_OPSTAT "\x02"
#define ENTITY_SENSOR_UNITS     ENTITY_SENSOR_ENTRY "\x06"
#define ENTITY_SENSOR_UNITS_1   ENTITY_SENSOR_UNITS "\x01"
#define ENTITY_SENSOR_UNITS_2   ENTITY_SENSOR_UNITS "\x02"

#define ENTMIB_TYPE_CELSIUS     0x08
#define ENTMIB_TYPE_PERCENT_RH  0x09
#define ENTMIB_SCALE_UNITS      0x09
#define ENTMIB_STATUS_OK        0x01

// Variables bound to MIB OIDs, served over SNMP:
static int32_t sensor_type_1 = ENTMIB_TYPE_CELSIUS;
static int32_t sensor_type_2 = ENTMIB_TYPE_PERCENT_RH;
static int32_t sensor_scale = ENTMIB_SCALE_UNITS;
static int32_t sensor_prec = 1;
int32_t snmp_temperature = 0;
int32_t snmp_percent_rh = 0;
static int32_t sensor_opstat = ENTMIB_STATUS_OK;

#define BER_INTEGER             0x02
#define BER_OCTET_STRING        0x04
#define BER_NULL                0x05
#define BER_OBJECT_ID           0x06
#define BER_SEQUENCE            0x30

#define SNMP_GET                0xa0
#define SNMP_GET_NEXT           0xa1
#define SNMP_RESPONSE           0xa2

typedef void (* entry_value_fn) (uint8_t * buf, uint8_t bufsize, uint8_t * pos,
                                 void * data);
typedef struct snmp_mib_entry_t_
{
   uint8_t * oid;
   uint8_t oid_size;
   entry_value_fn value_fn;
   void * data;
} snmp_mib_entry_t;

void
fn_int32 (uint8_t * buf, uint8_t bufsize, uint8_t * pos, void * data)
{
   if (bufsize - *pos < 6)
      return;

   buf [(*pos)++] = BER_INTEGER;

   int32_t v = *((int32_t *) data);
   if (-128 <= v && v <= 127)
   {
      buf [(*pos)++] = 0x01;
      buf [(*pos)++] = v & 0xff;
   }
   else if (-32768 <= v && v <= 32767)
   {
      buf [(*pos)++] = 0x02;
      buf [(*pos)++] = (v >> 8) & 0xff;
      buf [(*pos)++] = v & 0xff;
   }
   else if (-8388608 <= v && v <= 8388607)
   {
      buf [(*pos)++] = 0x03;
      buf [(*pos)++] = (v >> 16) & 0xff;
      buf [(*pos)++] = (v >> 8) & 0xff;
      buf [(*pos)++] = v & 0xff;
   }
   else
   {
      buf [(*pos)++] = 0x04;
      buf [(*pos)++] = v >> 24;
      buf [(*pos)++] = (v >> 16) & 0xff;
      buf [(*pos)++] = (v >> 8) & 0xff;
      buf [(*pos)++] = v & 0xff;
   }
}

void
fn_octet_string (uint8_t * buf, uint8_t bufsize, uint8_t * pos, void * data)
{
   uint8_t * str = (uint8_t *)data;
   uint8_t len = strlen (str);

   if (bufsize - *pos < len + 2)
      return;

   buf [(*pos)++] = BER_OCTET_STRING;
   buf [(*pos)++] = len;
   memcpy (buf + *pos, str, len);
   *pos += len;
}

#define DECL_MIB_ENTRY(OID, FN, DATA)           \
   {                                            \
      OID, sizeof (OID) - 1, FN, DATA           \
   }

// N.B.: the table MUST be arranged in increasing OID order!
static const snmp_mib_entry_t mib [] =
{
   DECL_MIB_ENTRY (ENTITY_SENSOR_TYPE_1, fn_int32, &sensor_type_1),
   DECL_MIB_ENTRY (ENTITY_SENSOR_TYPE_2, fn_int32, &sensor_type_2),
   DECL_MIB_ENTRY (ENTITY_SENSOR_SCALE_1, fn_int32, &sensor_scale),
   DECL_MIB_ENTRY (ENTITY_SENSOR_SCALE_2, fn_int32, &sensor_scale),
   DECL_MIB_ENTRY (ENTITY_SENSOR_PREC_1, fn_int32, &sensor_prec),
   DECL_MIB_ENTRY (ENTITY_SENSOR_PREC_2, fn_int32, &sensor_prec),
   DECL_MIB_ENTRY (ENTITY_SENSOR_VALUE_1, fn_int32, &snmp_temperature),
   DECL_MIB_ENTRY (ENTITY_SENSOR_VALUE_2, fn_int32, &snmp_percent_rh),
   DECL_MIB_ENTRY (ENTITY_SENSOR_OPSTAT_1, fn_int32, &sensor_opstat),
   DECL_MIB_ENTRY (ENTITY_SENSOR_OPSTAT_2, fn_int32, &sensor_opstat),
   DECL_MIB_ENTRY (ENTITY_SENSOR_UNITS_1, fn_octet_string, "x0.1 *C"),
   DECL_MIB_ENTRY (ENTITY_SENSOR_UNITS_2, fn_octet_string, "x0.1 %RH")
};
#define N_MIB_ENTRIES (sizeof (mib) / sizeof (mib [0]))

static const snmp_mib_entry_t *
find_mib_entry (uint8_t req, uint8_t * oid, uint8_t oid_size)
{
   switch (req)
   {
   case SNMP_GET:
      for (int k = 0; k < N_MIB_ENTRIES; ++k)
      {
         if (oid_size == mib [k].oid_size &&
             strncmp (oid, mib [k].oid, oid_size) == 0)
            return & mib [k];
      }
      break;
   case SNMP_GET_NEXT:
      for (int k = 0; k < N_MIB_ENTRIES; ++k)
      {
         int cmp = strncmp (oid, mib [k].oid, MIN (oid_size, mib [k].oid_size));
         if (cmp < 0 || (cmp == 0 && oid_size < mib [k].oid_size))
            return & mib [k];
      }
      break;
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
   //snmp_t *state = (snmp_t*) arg;

 #if 0
   printf ("SNMP recv: len=%x [", p->len);
   for (int k = 0; k < p->len; ++k)
      printf ("%02x ", pbuf_get_at (p, k));
   printf ("]\n");
 #endif

   // N.B.: we can get away treating length fields as single bytes,
   // as we do not need to support packets longer than 129 bytes
   // (with the packet length byte containing 127, the largest
   // possible single-byte value).

   if (p->len < 23)
      return snmp_recv_done (p);

   uint8_t * data = p->payload;
   if (data [0] != BER_SEQUENCE)
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
   if (req [0] != SNMP_GET && req [0] != SNMP_GET_NEXT)
      return snmp_recv_done (p);

   uint8_t req_len = req [1];
   if (9 + community_len + req_len != p->len)
      return snmp_recv_done (p);
   if (req_len < 14)
      return snmp_recv_done (p);

   if (req [2] != BER_INTEGER)
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
   response [7 + community_len] = SNMP_RESPONSE;
   uint8_t pdu_len_idx = 8 + community_len;
   DECL_LENGTH (bindings_len_idx);

   uint8_t * bind = req2 + 8;
   do
   {
      if (bindings_len < 2)
         return snmp_recv_done (p);

      if (bind [0] != BER_SEQUENCE)
         return snmp_recv_done (p);

      uint8_t bind_len = bind [1];
      if (bindings_len < bind_len + 2)
         return snmp_recv_done (p);

      if (bind [2] != BER_OBJECT_ID)
         return snmp_recv_done (p);
      uint8_t oid_len = bind [3];
      if (oid_len + 4 != bind_len)
         return snmp_recv_done (p);
      if (bind [bind_len] != BER_NULL || bind [bind_len + 1] != 0x00)
         return snmp_recv_done (p);

      uint8_t * oid = bind + 4;
    #if 0
      printf ("oid=%x [", oid_len);
      for (int k = 0; k < oid_len; ++k)
         printf ("%02x ", oid [k]);
      printf ("]\n");
    #endif

      APPEND_BYTE (BER_SEQUENCE);
      DECL_LENGTH (bind_len_idx);
      APPEND_BYTE (BER_OBJECT_ID);

      const snmp_mib_entry_t * e = find_mib_entry (req [0], oid, oid_len);
      if (e)
      {
         APPEND_BYTE (e->oid_size);
         APPEND_STR (e->oid, e->oid_size);
         e->value_fn (response, sizeof (response), &rp, e->data);
      }
      else
      {
         APPEND_BYTE (oid_len);
         APPEND_STR (oid, oid_len);
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
