#pragma once

#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "driver/gpio.h"
#include "driver/uart.h"

#include "mavlink/ardupilotmega/mavlink.h"

// MAVLink UART config
#define FC_UART_TX (GPIO_NUM_5)
#define FC_UART_RX (GPIO_NUM_4)

// 115200 baud is the default for Ardupilot
#define FC_UART_BAUDRATE 115200

// 8N1 is the default for Ardupilot
#define FC_UART_DATA_BITS UART_DATA_8_BITS
#define FC_UART_PARITY UART_PARITY_DISABLE
#define FC_UART_STOP_BITS UART_STOP_BITS_1

// Buffer size for MAVLink messages
#define FC_UART_BUF_SIZE 1024

#define UART_NUM UART_NUM_2

#define BRIDGE_QUEUE_LEN 16
#define MAVLINK_PACKET_MAX_LEN 300

extern QueueHandle_t fcu_to_udp_queue;

void mavlink_bridge_rx_task(void* pvParameters);
void mavlink_bridge_init(void);
void mavlink_bridge_init_queues(void);