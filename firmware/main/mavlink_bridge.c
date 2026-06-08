#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "mavlink_bridge.h"

static const char* TAG = "mavlink_bridge";

QueueHandle_t fcu_to_udp_queue = NULL;

static void mavlink_bridge_write_message(const mavlink_message_t* msg)
{
    uint8_t tx_buffer[MAVLINK_PACKET_MAX_LEN];
    uint16_t tx_len = mavlink_msg_to_send_buffer(tx_buffer, msg);

    uart_write_bytes(UART_NUM, tx_buffer, tx_len);
}

static void mavlink_bridge_send_heartbeat_probe(void)
{
    mavlink_message_t heartbeat;
    mavlink_msg_heartbeat_pack(MAVLINK_TEST_SYSTEM_ID,
                               MAVLINK_TEST_COMPONENT_ID,
                               &heartbeat,
                               MAV_TYPE_GCS,
                               MAV_AUTOPILOT_INVALID,
                               0,
                               0,
                               MAV_STATE_ACTIVE);
    mavlink_bridge_write_message(&heartbeat);

    mavlink_message_t request;
    mavlink_msg_command_long_pack(MAVLINK_TEST_SYSTEM_ID,
                                  MAVLINK_TEST_COMPONENT_ID,
                                  &request,
                                  1,
                                  MAV_COMP_ID_AUTOPILOT1,
                                  MAV_CMD_REQUEST_MESSAGE,
                                  0,
                                  (float)MAVLINK_MSG_ID_HEARTBEAT,
                                  0,
                                  0,
                                  0,
                                  0,
                                  0,
                                  0);
    mavlink_bridge_write_message(&request);
}

static void mavlink_bridge_pulse_heartbeat_led(void)
{
    gpio_set_level(MAVLINK_HEARTBEAT_LED_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(MAVLINK_HEARTBEAT_LED_PULSE_MS));
    gpio_set_level(MAVLINK_HEARTBEAT_LED_GPIO, 0);
}

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

void mavlink_bridge_heartbeat_probe_task(void* pvParameters)
{
    gpio_config_t led_config = {
        .pin_bit_mask = 1ULL << MAVLINK_HEARTBEAT_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&led_config));
    gpio_set_level(MAVLINK_HEARTBEAT_LED_GPIO, 0);

    TickType_t last_probe = xTaskGetTickCount() - pdMS_TO_TICKS(MAVLINK_HEARTBEAT_PERIOD_MS);

    while (1)
    {
        TickType_t now = xTaskGetTickCount();
        if ((now - last_probe) >= pdMS_TO_TICKS(MAVLINK_HEARTBEAT_PERIOD_MS))
        {
            mavlink_bridge_send_heartbeat_probe();
            last_probe = now;
        }

        mavlink_message_t msg;
        if (xQueueReceive(fcu_to_udp_queue, &msg, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            if (msg.msgid == MAVLINK_MSG_ID_HEARTBEAT)
            {
                ESP_LOGI(TAG, "FC heartbeat from system %u component %u", msg.sysid, msg.compid);
                mavlink_bridge_pulse_heartbeat_led();
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
