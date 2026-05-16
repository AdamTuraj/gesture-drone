#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "driver/uart.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "lwip/sockets.h"

#include "rpi_wifi.h"
#include "mavlink_bridge.h"
#include "camera.h"

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;

static const char* TAG = "udp_server";

static void send_fcu_queue_to_udp(int sock, const struct sockaddr_storage* source_addr, socklen_t socklen)
{
    mavlink_message_t queue_buffer;
    uint8_t tx_buffer[MAX_BUFFER_SIZE];

    while (xQueueReceive(fcu_to_udp_queue, &queue_buffer, 0) == pdTRUE)
    {
        uint16_t tx_len = mavlink_msg_to_send_buffer(tx_buffer, &queue_buffer);

        int err = sendto(sock, tx_buffer, tx_len, 0, (const struct sockaddr*)source_addr, socklen);
        if (err < 0)
        {
            ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
        }
    }
}

static void send_camera_frame_to_udp(int sock, const struct sockaddr_storage* source_addr, socklen_t socklen)
{
    if (!camera_frames)
    {
        return;
    }

    camera_frame_t* frame = NULL;
    uint8_t tx_buffer[sizeof(camera_chunk_header_t) + CAMERA_CHUNK_PAYLOAD_SIZE];

    while (xQueueReceive(camera_frames, &frame, 0) == pdTRUE)
    {
        if (!frame)
        {
            continue;
        }

        if (frame->header.payload_len > CAMERA_CHUNK_PAYLOAD_SIZE)
        {
            ESP_LOGE(TAG, "Invalid camera chunk payload length: %u", frame->header.payload_len);
            free(frame->payload);
            free(frame);
            continue;
        }

        size_t tx_len = sizeof(frame->header) + frame->header.payload_len;
        memcpy(tx_buffer, &frame->header, sizeof(frame->header));
        memcpy(tx_buffer + sizeof(frame->header), frame->payload, frame->header.payload_len);

        int err = sendto(sock, tx_buffer, tx_len, 0, (const struct sockaddr*)source_addr, socklen);
        if (err < 0)
        {
            ESP_LOGE(TAG, "Error occurred during sending camera frame: errno %d", errno);
        }

        free(frame->payload);
        free(frame);
    }
}

void rpi_wifi_udp_server_task(void* pvParameters)
{
    char rx_buffer[MAX_BUFFER_SIZE];
    int addr_family = AF_INET;
    int ip_protocol = 0;
    struct sockaddr_in dest_addr;

    // Prepare destination address structure (server's address)
    dest_addr.sin_addr.s_addr = htonl(INADDR_ANY); // Listen on all interfaces
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(UDP_SERVER_PORT);
    ip_protocol = IPPROTO_UDP;

    // Create socket
    int sock = socket(addr_family, SOCK_DGRAM, ip_protocol);
    if (sock < 0)
    {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Socket created");

    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0)
    {
        ESP_LOGE(TAG, "Unable to set non-blocking mode: errno %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    // Bind socket
    int err = bind(sock, (struct sockaddr*)&dest_addr, sizeof(dest_addr));
    if (err < 0)
    {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Socket bound, port %d", UDP_SERVER_PORT);

    struct sockaddr_storage peer_addr = {0};
    socklen_t peer_addr_len = 0;
    bool have_peer = false;

    while (1)
    {
        struct sockaddr_storage source_addr;
        socklen_t source_addr_len = sizeof(source_addr);

        // Receive data
        int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer), 0, (struct sockaddr*)&source_addr, &source_addr_len);

        // Error occurred during receiving
        if (len < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                if (have_peer)
                {
                    send_fcu_queue_to_udp(sock, &peer_addr, peer_addr_len);
                    send_camera_frame_to_udp(sock, &peer_addr, peer_addr_len);
                }
                // No data available, continue waiting
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
            else
            {
                ESP_LOGE(TAG, "recvfrom failed: errno %d", errno);
                break;
            }
        }
        // Data received
        else
        {
            peer_addr = source_addr;
            peer_addr_len = source_addr_len;
            have_peer = true;

            if (len > 0)
            {
                ESP_LOGI(TAG, "Received MAVLink message of length %d", len);
                uart_write_bytes(UART_NUM, rx_buffer, len);
            }
            else
            {
                ESP_LOGW(TAG, "Received non-MAVLink data of length %d", len);
            }
        }

        if (have_peer)
        {
            send_fcu_queue_to_udp(sock, &peer_addr, peer_addr_len);
            send_camera_frame_to_udp(sock, &peer_addr, peer_addr_len);
        }
    }

    ESP_LOGE(TAG, "Shutting down socket and restarting...");
    close(sock);
    vTaskDelete(NULL);
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        if (s_retry_num < WIFI_MAX_RETRY)
        {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGW(TAG, "Retrying WiFi connection...");
        }
        else
        {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;

        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));

        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}


void rpi_wifi_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_wifi_event_group = xEventGroupCreate();

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));

    ESP_ERROR_CHECK(
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta =
            {
                .ssid = WIFI_SSID,
                .password = WIFI_PASS,
            },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits =
        xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT)
    {
        ESP_LOGI(TAG, "Connected to WiFi");
    }
    else if (bits & WIFI_FAIL_BIT)
    {
        ESP_LOGE(TAG, "Failed to connect to WiFi");
    }
}
