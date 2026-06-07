#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "mavlink_bridge.h"
#include "camera.h"
#include "usb_video.h"

void app_main(void)
{
    ESP_LOGI("Main", "Starting USB video + MAVLink heartbeat test...");

    // Initialize MAVLink communication
    mavlink_bridge_init();
    mavlink_bridge_init_queues();

    ESP_LOGI("Main", "WiFi features disabled for this test build");
    esp_err_t usb_video_err = usb_video_init();
    esp_err_t camera_err = camera_init();

    // Create a task to receive MAVLink messages
    xTaskCreate(mavlink_bridge_rx_task, "ReceiveMavlinkTask", 4096, NULL, 5, NULL);
    xTaskCreate(mavlink_bridge_heartbeat_probe_task, "HeartbeatProbeTask", 4096, NULL, 5, NULL);

    if (camera_err == ESP_OK)
    {
        xTaskCreate(camera_capture_task, "CameraCaptureTask", 4096, NULL, 4, NULL);

        if (usb_video_err == ESP_OK)
        {
            xTaskCreate(usb_video_stream_task, "UsbVideoTask", 4096, NULL, 4, NULL);
        }
        else
        {
            ESP_LOGW("Main", "USB video disabled because init failed: %d", usb_video_err);
        }
    }
    else
    {
        ESP_LOGW("Main", "Camera disabled because init failed: %d", camera_err);
    }
}
