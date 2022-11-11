#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"

#define SNMP_PORT 161

#define SNMP_RO_COMMUNITY "public"

// Variables bound to SNMP; set values externally
extern int32_t snmp_temperature;
extern int32_t snmp_percent_rh;

typedef struct snmp_t_
{
   struct udp_pcb *pcb;
} snmp_t;

snmp_t* snmp_init ();
