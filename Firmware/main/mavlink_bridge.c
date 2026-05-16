#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"

#include "mavlink_bridge.h"

QueueHandle_t fcu_to_udp_queue = NULL;

void mavlink_bridge_rx_task(void* pvParameters)
{
    uint8_t rx_buffer[FC_UART_BUF_SIZE];
    mavlink_message_t msg;
    mavlink_status_t status = {0};

    while (1)
    {
        // Read data from UART
        int len = uart_read_bytes(UART_NUM, rx_buffer, FC_UART_BUF_SIZE, pdMS_TO_TICKS(100));
        if (len > 0)
        {
            for (int i = 0; i < len; i++)
            {
                if (mavlink_parse_char(MAVLINK_COMM_0, rx_buffer[i], &msg, &status))
                {
                    xQueueSend(fcu_to_udp_queue, &msg, portMAX_DELAY);
                }
            }
        }
    }
}

void mavlink_bridge_init()
{
    // Initialize MAVLink communication here
    const uart_config_t uart_config = {
        .baud_rate = FC_UART_BAUDRATE,
        .data_bits = FC_UART_DATA_BITS,
        .parity = FC_UART_PARITY,
        .stop_bits = FC_UART_STOP_BITS,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    // We won't use a buffer for sending data.
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM, FC_UART_BUF_SIZE * 2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM, FC_UART_TX, FC_UART_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

void mavlink_bridge_init_queues()
{
    fcu_to_udp_queue = xQueueCreate(BRIDGE_QUEUE_LEN, sizeof(mavlink_message_t));

    configASSERT(fcu_to_udp_queue);
}