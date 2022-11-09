#include "pico/stdlib.h"

#define WIFI_CONNECT_TIMEOUT_MS (10 * 1000)

enum wifi_status_t { WIFI_DOWN, WIFI_CONNECTING, WIFI_UP, WIFI_ERROR };
extern uint8_t wifi_status;

void wifi_init ();
void wifi_on_tick ();
