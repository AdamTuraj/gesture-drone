#pragma once

#include <stdint.h>

#include "esp_camera.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#define CAM_PIN_PWDN -1 // power down is not used
#define CAM_PIN_RESET 40
#define CAM_PIN_XCLK 14
#define CAM_PIN_SIOD 42
#define CAM_PIN_SIOC 41

#define CAM_PIN_D7 7
#define CAM_PIN_D6 6
#define CAM_PIN_D5 21
#define CAM_PIN_D4 48
#define CAM_PIN_D3 12
#define CAM_PIN_D2 10
#define CAM_PIN_D1 11
#define CAM_PIN_D0 13

#define CAM_PIN_VSYNC 39
#define CAM_PIN_HREF 38
#define CAM_PIN_PCLK 47

#define CAMERA_XCLK_FREQ_HZ 10000000

#define CAMERA_FRAME_SIZE FRAMESIZE_VGA
#define CAMERA_JPEG_QUALITY 10

#define CAMERA_CAPTURE_INTERVAL_MS 100

#define CAMERA_CHUNK_MAGIC 0x304D4143u
#define CAMERA_CHUNK_PAYLOAD_SIZE 1200

extern QueueHandle_t camera_frames;

typedef struct __attribute__((packed))
{
    uint32_t magic;
    uint32_t frame_id; // increments every captured frame
    uint16_t chunk_id; // 0, 1, 2...
    uint16_t chunk_count; // total chunks in this frame
    uint16_t payload_len; // bytes in this chunk
} camera_chunk_header_t;

typedef struct
{
    camera_chunk_header_t header;
    uint8_t* payload;
} camera_frame_t;

esp_err_t camera_init(void);
void camera_capture_task(void* pvParameters);
