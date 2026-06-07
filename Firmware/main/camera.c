#include "camera.h"

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static const char* TAG = "camera";

QueueHandle_t camera_frames = NULL;

#define CAMERA_FRAME_QUEUE_LEN 32
#define CAMERA_QUEUE_SEND_TIMEOUT_MS 2000
#define CAMERA_WARMUP_FRAMES 5

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

    // 10 MHz is easier on the parallel camera bus than the default 20 MHz.
    .xclk_freq_hz = CAMERA_XCLK_FREQ_HZ,
    .ledc_timer = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,

    .pixel_format = PIXFORMAT_JPEG, // YUV422,GRAYSCALE,RGB565,JPEG
    .frame_size =
        CAMERA_FRAME_SIZE, // QQVGA-UXGA, For ESP32, do not use sizes above QVGA when not JPEG. The performance of the
                           // ESP32-S series has improved a lot, but JPEG mode always gives better frame rates.

    .jpeg_quality = CAMERA_JPEG_QUALITY, // 0-63, for OV series camera sensors, lower number means higher quality
    .fb_count = 1, // When jpeg mode is used, if fb_count more than one, the driver will work in continuous mode.
    .grab_mode = CAMERA_GRAB_WHEN_EMPTY, // CAMERA_GRAB_LATEST. Sets when buffers should be filled
    .fb_location = CAMERA_FB_IN_PSRAM,
};

static bool camera_frame_is_complete_jpeg(const camera_fb_t* fb)
{
    return fb && fb->format == PIXFORMAT_JPEG && fb->len >= 4 && fb->buf[0] == 0xff && fb->buf[1] == 0xd8 &&
        fb->buf[fb->len - 2] == 0xff && fb->buf[fb->len - 1] == 0xd9;
}

static void camera_log_capture_config(void)
{
    sensor_t* sensor = esp_camera_sensor_get();
    if (sensor)
    {
        ESP_LOGI(
            TAG,
            "Sensor PID=0x%04x, frame_size=%d, jpeg_quality=%d, xclk=%d Hz, psram_dma=%s",
            (unsigned)sensor->id.PID,
            camera_config.frame_size,
            camera_config.jpeg_quality,
            camera_config.xclk_freq_hz,
            esp_camera_get_psram_mode() ? "on" : "off");
    }
}

static void camera_discard_warmup_frames(void)
{
    for (int i = 0; i < CAMERA_WARMUP_FRAMES; i++)
    {
        camera_fb_t* fb = esp_camera_fb_get();
        if (fb)
        {
            esp_camera_fb_return(fb);
        }
        else
        {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

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

        if (xQueueSend(camera_frames, &frame, pdMS_TO_TICKS(CAMERA_QUEUE_SEND_TIMEOUT_MS)) != pdPASS)
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
    camera_frames = xQueueCreate(CAMERA_FRAME_QUEUE_LEN, sizeof(camera_frame_t*));
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

    camera_log_capture_config();
    camera_discard_warmup_frames();

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

        if (!camera_frame_is_complete_jpeg(fb))
        {
            static uint32_t bad_jpeg_count = 0;
            bad_jpeg_count++;
            if ((bad_jpeg_count % 32) == 1)
            {
                ESP_LOGW(
                    TAG,
                    "Dropping invalid JPEG frame: len=%u format=%d head=%02x %02x tail=%02x %02x",
                    (unsigned)fb->len,
                    fb->format,
                    fb->len > 0 ? (unsigned)fb->buf[0] : 0,
                    fb->len > 1 ? (unsigned)fb->buf[1] : 0,
                    fb->len > 1 ? (unsigned)fb->buf[fb->len - 2] : 0,
                    fb->len > 0 ? (unsigned)fb->buf[fb->len - 1] : 0);
            }
            esp_camera_fb_return(fb);
            vTaskDelay(pdMS_TO_TICKS(CAMERA_CAPTURE_INTERVAL_MS));
            continue;
        }

        // Send the frame buffer to the queue for processing
        split_frame_and_send_to_queue(fb, frame_id);
        esp_camera_fb_return(fb);
        frame_id++;

        vTaskDelay(pdMS_TO_TICKS(CAMERA_CAPTURE_INTERVAL_MS));
    }
}
