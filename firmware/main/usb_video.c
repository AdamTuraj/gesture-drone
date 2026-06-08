#include "usb_video.h"

#include <stdbool.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "camera.h"

#define USB_VIDEO_TX_BUFFER_SIZE 8192
#define USB_VIDEO_RX_BUFFER_SIZE 256
#define USB_VIDEO_WRITE_TIMEOUT_MS 100
#define USB_VIDEO_IDLE_DELAY_MS 5
#define USB_VIDEO_NO_HOST_DELAY_MS 100

static const char* TAG = "usb_video";

static bool s_usb_video_ready = false;

static void free_camera_frame(camera_frame_t* frame)
{
    if (!frame)
    {
        return;
    }

    free(frame->payload);
    free(frame);
}

static bool usb_video_write_all(const void* data, size_t len)
{
    const uint8_t* cursor = (const uint8_t*)data;
    size_t remaining = len;

    while (remaining > 0)
    {
        int written = usb_serial_jtag_write_bytes(cursor, remaining, pdMS_TO_TICKS(USB_VIDEO_WRITE_TIMEOUT_MS));
        if (written <= 0)
        {
            return false;
        }

        cursor += written;
        remaining -= written;
    }

    return true;
}

esp_err_t usb_video_init(void)
{
    if (!usb_serial_jtag_is_driver_installed())
    {
        usb_serial_jtag_driver_config_t usb_config = {
            .tx_buffer_size = USB_VIDEO_TX_BUFFER_SIZE,
            .rx_buffer_size = USB_VIDEO_RX_BUFFER_SIZE,
        };

        esp_err_t err = usb_serial_jtag_driver_install(&usb_config);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "USB Serial/JTAG driver install failed: %d", err);
            return err;
        }
    }

    s_usb_video_ready = true;
    ESP_LOGI(TAG, "USB video stream ready; chunk magic is 0x%08" PRIx32, (uint32_t)CAMERA_CHUNK_MAGIC);
    return ESP_OK;
}

void usb_video_stream_task(void* pvParameters)
{
    uint32_t dropped_chunks = 0;

    while (1)
    {
        if (!s_usb_video_ready || !camera_frames)
        {
            vTaskDelay(pdMS_TO_TICKS(USB_VIDEO_NO_HOST_DELAY_MS));
            continue;
        }

        camera_frame_t* frame = NULL;
        if (xQueueReceive(camera_frames, &frame, pdMS_TO_TICKS(USB_VIDEO_IDLE_DELAY_MS)) != pdTRUE)
        {
            continue;
        }

        if (!frame)
        {
            continue;
        }

        if (!usb_serial_jtag_is_connected())
        {
            free_camera_frame(frame);
            vTaskDelay(pdMS_TO_TICKS(USB_VIDEO_NO_HOST_DELAY_MS));
            continue;
        }

        if (!frame->payload || frame->header.payload_len > CAMERA_CHUNK_PAYLOAD_SIZE)
        {
            ESP_LOGW(TAG, "Dropping invalid camera chunk, payload_len=%u", frame->header.payload_len);
            free_camera_frame(frame);
            continue;
        }

        bool sent = usb_video_write_all(&frame->header, sizeof(frame->header))
                    && usb_video_write_all(frame->payload, frame->header.payload_len);

        if (!sent)
        {
            dropped_chunks++;
            if ((dropped_chunks % 32) == 1)
            {
                ESP_LOGW(TAG, "USB video write timed out, dropped_chunks=%" PRIu32, dropped_chunks);
            }
        }

        free_camera_frame(frame);
    }
}
