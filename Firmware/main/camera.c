#include "camera.h"

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdlib.h>
#include <string.h>

static const char* TAG = "camera";

QueueHandle_t camera_frames = NULL;

static camera_config_t camera_config = {
    .pin_pwdn = CAM_PIN_PWDN,
    .pin_reset = CAM_PIN_RESET,
    .pin_xclk = CAM_PIN_XCLK,
    .pin_sccb_sda = CAM_PIN_SIOD,
    .pin_sccb_scl = CAM_PIN_SIOC,

    .pin_d7 = CAM_PIN_D7,
    .pin_d6 = CAM_PIN_D6,
    .pin_d5 = CAM_PIN_D5,
    .pin_d4 = CAM_PIN_D4,
    .pin_d3 = CAM_PIN_D3,
    .pin_d2 = CAM_PIN_D2,
    .pin_d1 = CAM_PIN_D1,
    .pin_d0 = CAM_PIN_D0,
    .pin_vsync = CAM_PIN_VSYNC,
    .pin_href = CAM_PIN_HREF,
    .pin_pclk = CAM_PIN_PCLK,

    .xclk_freq_hz = 20000000,
    .ledc_timer = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,

    .pixel_format = PIXFORMAT_JPEG, // YUV422,GRAYSCALE,RGB565,JPEG
    .frame_size =
        FRAMESIZE_QVGA, // QQVGA-UXGA, For ESP32, do not use sizes above QVGA when not JPEG. The performance of the
                        // ESP32-S series has improved a lot, but JPEG mode always gives better frame rates.

    .jpeg_quality = 12, // 0-63, for OV series camera sensors, lower number means higher quality
    .fb_count = 1, // When jpeg mode is used, if fb_count more than one, the driver will work in continuous mode.
    .grab_mode = CAMERA_GRAB_WHEN_EMPTY, // CAMERA_GRAB_LATEST. Sets when buffers should be filled
    .fb_location = CAMERA_FB_IN_PSRAM};

static void split_frame_and_send_to_queue(camera_fb_t* fb, uint32_t frame_id)
{
    size_t offset = 0;
    uint16_t chunk_id = 0;

    uint16_t chunk_count = (fb->len + CAMERA_CHUNK_PAYLOAD_SIZE - 1) / CAMERA_CHUNK_PAYLOAD_SIZE;

    while (offset < fb->len)
    {
        size_t packet_size =
            (fb->len - offset) > CAMERA_CHUNK_PAYLOAD_SIZE ? CAMERA_CHUNK_PAYLOAD_SIZE : (fb->len - offset);
        uint8_t* packet_data = fb->buf + offset;

        camera_frame_t* frame = (camera_frame_t*)malloc(sizeof(camera_frame_t));

        if (!frame)
        {
            ESP_LOGE(TAG, "Failed to allocate memory for camera frame");
            break;
        }

        frame->header.magic = CAMERA_CHUNK_MAGIC;
        frame->header.frame_id = frame_id;
        frame->header.chunk_id = chunk_id;
        frame->header.chunk_count = chunk_count;
        frame->header.payload_len = (uint16_t)packet_size;
        frame->payload = (uint8_t*)malloc(packet_size);

        if (!frame->payload)
        {
            ESP_LOGE(TAG, "Failed to allocate memory for camera frame payload");
            free(frame);
            break;
        }

        memcpy(frame->payload, packet_data, packet_size);

        if (xQueueSend(camera_frames, &frame, 0) != pdPASS)
        {
            ESP_LOGE(TAG, "Failed to send camera frame packet to queue");
            free(frame->payload);
            free(frame);
            break;
        }

        offset += packet_size;
        chunk_id++;
    }
}

esp_err_t camera_init(void)
{
    camera_frames = xQueueCreate(10, sizeof(camera_frame_t*));
    if (!camera_frames)
    {
        ESP_LOGE(TAG, "Failed to create camera frame queue");
        return ESP_ERR_NO_MEM;
    }

    // initialize the camera
    esp_err_t err = esp_camera_init(&camera_config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Camera Init Failed: %d", err);
        vQueueDelete(camera_frames);
        camera_frames = NULL;
        return err;
    }

    return ESP_OK;
}

void camera_capture_task(void* pvParameters)
{
    uint32_t frame_id = 0;

    while (1)
    {
        camera_fb_t* fb = esp_camera_fb_get();
        if (!fb)
        {
            ESP_LOGE(TAG, "Camera capture failed");
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // Send the frame buffer to the queue for processing
        split_frame_and_send_to_queue(fb, frame_id);
        esp_camera_fb_return(fb);
        frame_id++;

        vTaskDelay(pdMS_TO_TICKS(100)); // Capture at ~10 FPS
    }
}
