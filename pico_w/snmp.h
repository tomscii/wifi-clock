#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"

#define SNMP_PORT 161

#define SNMP_RO_COMMUNITY "public"

typedef struct snmp_t_
{
   struct udp_pcb *pcb;
} snmp_t;

snmp_t* snmp_init ();
