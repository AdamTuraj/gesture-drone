#pragma once

#include "esp_err.h"

esp_err_t usb_video_init(void);
void usb_video_stream_task(void* pvParameters);
