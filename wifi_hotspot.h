#ifndef WIFI_HOTSPOT_H
#define WIFI_HOTSPOT_H

#include <stdbool.h>
#include "lwip/ip_addr.h"
#include "lwip/tcp.h"

/* --------------------------------------------------------------------------
 * Device state machine — shared between main.c and wifi_hotspot.c
 * -------------------------------------------------------------------------- */
typedef enum {
    DEVICE_STATE_OFF,
    DEVICE_STATE_RAMPING_UP,
    DEVICE_STATE_RUNNING,
    DEVICE_STATE_RAMPING_DOWN
} DeviceState;

/* Current device state (defined in main.c, read by wifi_hotspot.c) */
extern volatile DeviceState g_device_state;

/**
 * Request a ramp-up: OFF -> RAMPING_UP -> RUNNING.
 * Safe to call from the lwIP TCP callback context.
 */
void device_request_start(void);

/**
 * Request a ramp-down: RUNNING/RAMPING_UP -> RAMPING_DOWN -> OFF.
 * Safe to call from the lwIP TCP callback context.
 */
void device_request_stop(void);

/* Opaque server state shared between wifi_hotspot.c and main.c */
typedef struct TCP_SERVER_T_ {
    struct tcp_pcb *server_pcb;
    bool complete;      /* Set to true when the AP should shut down */
    ip_addr_t gw;
} TCP_SERVER_T;

/**
 * Start the Wi-Fi access point, DHCP server, DNS server, and HTTP server.
 * Call cyw43_arch_init() BEFORE calling this function.
 *
 * @param state  Caller-allocated TCP_SERVER_T. Must remain valid for the
 *               lifetime of the hotspot.
 * @return true on success, false on any initialisation failure.
 */
bool wifi_hotspot_start(TCP_SERVER_T *state);

/**
 * Stop the HTTP server, DNS server, and DHCP server.
 * Does NOT call cyw43_arch_deinit() — the caller is responsible for that.
 *
 * @param state  The same TCP_SERVER_T passed to wifi_hotspot_start().
 */
void wifi_hotspot_stop(TCP_SERVER_T *state);

#endif /* WIFI_HOTSPOT_H */
