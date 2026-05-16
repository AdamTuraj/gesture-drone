#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "mavlink_bridge.h"
#include "rpi_wifi.h"
#include "camera.h"

void app_main(void)
{
    ESP_LOGI("Main", "Starting MAVLink bridge...");

    // Initialize MAVLink communication
    mavlink_bridge_init();
    mavlink_bridge_init_queues();
    rpi_wifi_init();
    esp_err_t camera_err = camera_init();

    // Create a task to receive MAVLink messages
    xTaskCreate(mavlink_bridge_rx_task, "ReceiveMavlinkTask", 4096, NULL, 5, NULL);
    xTaskCreate(rpi_wifi_udp_server_task, "UDPServerTask", 4096, NULL, 5, NULL);
    if (camera_err == ESP_OK)
    {
        xTaskCreate(camera_capture_task, "CameraCaptureTask", 4096, NULL, 4, NULL);
    }
    else
    {
        ESP_LOGW("Main", "Camera disabled because init failed: %d", camera_err);
    }
}
