#include "pico/stdlib.h"

#define DHT_GPIO                        16
#define DHT_POLL_INTERVAL_MS   (30 * 1000)
#define DHT_RETRY_INTERVAL_MS   (3 * 1000)

typedef struct dht_t_
{
   absolute_time_t next_read;
} dht_t;

typedef struct
{
   float humidity;
   float temp_celsius;
   uint8_t data [5];
} dht_reading;

dht_t* dht_init ();
void dht_on_tick (dht_t*);
