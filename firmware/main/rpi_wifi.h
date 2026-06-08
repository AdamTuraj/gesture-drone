#pragma once

#define WIFI_SSID "your-ssid"
#define WIFI_PASS "your-password"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define WIFI_MAX_RETRY 5

#define MAX_BUFFER_SIZE MAVLINK_MAX_PACKET_LEN
#define UDP_SERVER_PORT 3333

void rpi_wifi_udp_server_task(void* pvParameters);
void rpi_wifi_init(void);