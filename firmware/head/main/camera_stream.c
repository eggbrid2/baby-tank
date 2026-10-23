#include "camera_stream.h"

#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <net/if.h>
#include <netinet/tcp.h>
#include <sys/time.h>

#include "driver/i2c_types.h"
#include "esp_camera.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_jpeg_enc.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "wifi_portal.h"

#define CAMERA_PIN_PWDN (-1)
#define CAMERA_PIN_RESET (-1)
#define CAMERA_PIN_VSYNC 6
#define CAMERA_PIN_HREF 7
#define CAMERA_PIN_PCLK 13
#define CAMERA_PIN_XCLK 15
#define CAMERA_I2C_PORT I2C_NUM_1
#define CAMERA_PIN_D0 11
#define CAMERA_PIN_D1 9
#define CAMERA_PIN_D2 8
#define CAMERA_PIN_D3 10
#define CAMERA_PIN_D4 12
#define CAMERA_PIN_D5 18
#define CAMERA_PIN_D6 17
#define CAMERA_PIN_D7 16

#define CAMERA_XCLK_HZ 16000000
/* The station connection uses a full-channel, strongest-BSSID scan so the
 * phone and robot can share the best mesh node without taking the phone off
 * its Internet Wi-Fi. Keep the 15 fps target; the one-frame ready queue still
 * replaces stale work if a particular client falls behind. */
#define CAMERA_FRAME_INTERVAL_MS 66
#define CAMERA_JPEG_QUALITY 72
#define CAMERA_FRAME_SIZE FRAMESIZE_HQVGA
#define CAMERA_WIDTH 240
#define CAMERA_HEIGHT 176
#define CAMERA_JPEG_OUTPUT_CAPACITY (128U * 1024U)
/* One buffer may be in TCP send while the producer needs one current output
 * plus one queued slot whose stale frame can be replaced. Two buffers made
 * the queued frame wait behind every slow send (measured 481-495 ms). */
#define CAMERA_JPEG_BUFFER_COUNT 3U
#define STREAM_PREFIX_CAPACITY 128U

static const char *TAG = "baby_tank_camera";
static const char *STREAM_TYPE =
    "multipart/x-mixed-replace;boundary=babytankframe";
static const char *STREAM_BOUNDARY = "\r\n--babytankframe\r\n";
static const char *STREAM_PART =
    "Content-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n";

static SemaphoreHandle_t s_camera_lock;
static SemaphoreHandle_t s_stream_lock;
static bool s_ready;
static bool s_hardware_ready;
static atomic_bool s_stopping;
static httpd_handle_t s_lan_server;
static struct ifreq s_sta_interface;
static uint8_t s_lan_token[16];
static bool s_lan_token_ready;
static const bool s_lan_route = true;
static jpeg_enc_handle_t s_jpeg_encoder;
static uint8_t *s_jpeg_output[CAMERA_JPEG_BUFFER_COUNT];

typedef struct {
    uint8_t index;
    size_t length;
    int64_t capture_ms;
    int64_t encode_ms;
    int64_t ready_us;
} stream_frame_t;

typedef struct {
    QueueHandle_t free_buffers;
    QueueHandle_t ready_frames;
    TaskHandle_t consumer;
    atomic_bool stop;
    atomic_uint dropped;
} stream_pipeline_t;

typedef struct {
    uint8_t *data;
    size_t capacity;
    size_t length;
    bool overflow;
} jpeg_buffer_writer_t;

static esp_err_t start_camera_hardware(void);
static void stop_camera_hardware(void);

static size_t write_camera_jpeg(void *argument, size_t index,
                                const void *data, size_t length)
{
    jpeg_buffer_writer_t *writer = argument;
    if (data == NULL || length == 0U) return 0U;
    if (index > writer->capacity || length > writer->capacity - index) {
        writer->overflow = true;
        return length;
    }
    memcpy(writer->data + index, data, length);
    writer->length = index + length;
    return length;
}

static bool encode_stream_jpeg(const camera_fb_t *frame, uint8_t *output,
                               size_t *length)
{
    if (output == NULL) return false;
    uint8_t *destination = output + STREAM_PREFIX_CAPACITY;
    size_t capacity = CAMERA_JPEG_OUTPUT_CAPACITY - STREAM_PREFIX_CAPACITY;
    if (frame->format == PIXFORMAT_JPEG && frame->len <= capacity) {
        memcpy(destination, frame->buf, frame->len);
        *length = frame->len;
        return true;
    }
    if (frame->format == PIXFORMAT_RGB565 &&
        frame->len == CAMERA_WIDTH * CAMERA_HEIGHT * 2U) {
        /* Capture corruption was caused by direct camera DMA into PSRAM, not
         * by JPEG conversion. Keep the safe staged-DMA capture configuration
         * and use esp_new_jpeg's much faster RGB565 encoder. */
        if (s_jpeg_encoder != NULL) {
            int encoded = 0;
            jpeg_error_t status = jpeg_enc_process(
                s_jpeg_encoder, frame->buf, (int)frame->len,
                destination, (int)capacity, &encoded);
            if (status == JPEG_ERR_OK && encoded >= 128 &&
                (size_t)encoded <= capacity) {
                *length = (size_t)encoded;
                return true;
            }
            ESP_LOGW(TAG, "optimized JPEG encode failed status=%d bytes=%d",
                     status, encoded);
        }
        /* Retain the component's reference converter as a correctness
         * fallback. It is slower but prevents a stream outage if the fast
         * encoder rejects a frame. */
        jpeg_buffer_writer_t writer = {
            .data = destination,
            .capacity = capacity,
        };
        bool encoded = frame2jpg_cb((camera_fb_t *)frame,
                                    CAMERA_JPEG_QUALITY,
                                    write_camera_jpeg, &writer);
        if (encoded && !writer.overflow && writer.length >= 128U) {
            *length = writer.length;
            return true;
        }
        ESP_LOGW(TAG, "esp32-camera JPEG encode failed overflow=%d bytes=%zu",
                 writer.overflow, writer.length);
    }
    uint8_t *temporary = NULL;
    size_t temporary_length = 0;
    bool converted = frame2jpg((camera_fb_t *)frame, CAMERA_JPEG_QUALITY,
                               &temporary, &temporary_length);
    if (!converted || temporary == NULL || temporary_length > capacity) {
        free(temporary);
        return false;
    }
    memcpy(destination, temporary, temporary_length);
    free(temporary);
    *length = temporary_length;
    return true;
}

static bool authorize_lan(httpd_req_t *request)
{
    if (request->user_ctx != &s_lan_route) return true;
    char supplied[33] = {0};
    if (httpd_req_get_hdr_value_str(request, "X-BabyTank-Camera-Token",
                                     supplied, sizeof(supplied)) != ESP_OK ||
        strlen(supplied) != 32) {
        httpd_resp_send_err(request, HTTPD_403_FORBIDDEN, "camera token required");
        return false;
    }
    unsigned mismatch = 0;
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(s_lan_token); ++i) {
        mismatch |= (unsigned)(supplied[i * 2] ^ hex[s_lan_token[i] >> 4]);
        mismatch |= (unsigned)(supplied[i * 2 + 1] ^ hex[s_lan_token[i] & 15]);
    }
    if (mismatch != 0) {
        httpd_resp_send_err(request, HTTPD_403_FORBIDDEN, "invalid camera token");
        return false;
    }
    return true;
}

static esp_err_t send_jpeg(httpd_req_t *request, camera_fb_t *frame)
{
    uint8_t *converted = NULL;
    size_t length = frame->len;
    const uint8_t *data = frame->buf;
    if (frame->format != PIXFORMAT_JPEG) {
        ESP_RETURN_ON_FALSE(frame2jpg(frame, CAMERA_JPEG_QUALITY,
                                     &converted, &length),
                            ESP_FAIL, TAG, "JPEG conversion failed");
        data = converted;
    }
    httpd_resp_set_type(request, "image/jpeg");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store, max-age=0");
    esp_err_t result = httpd_resp_send(request, (const char *)data, length);
    free(converted);
    return result;
}

static esp_err_t health_handler(httpd_req_t *request)
{
    if (!authorize_lan(request)) return ESP_OK;
    char body[192];
    int length = snprintf(body, sizeof(body),
                          "{\"ok\":true,\"camera\":true,\"width\":%d,"
                          "\"height\":%d,\"stream\":\"http://192.168.4.1/stream\","
                          "\"heap\":%u}",
                          CAMERA_WIDTH, CAMERA_HEIGHT,
                          (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ESP_RETURN_ON_FALSE(length > 0 && (size_t)length < sizeof(body),
                        ESP_ERR_INVALID_SIZE, TAG, "health response overflow");
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, body, length);
}

static esp_err_t snapshot_handler(httpd_req_t *request)
{
    if (!authorize_lan(request)) return ESP_OK;
    if (xSemaphoreTake(s_camera_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        httpd_resp_set_status(request, "503 Busy");
        return httpd_resp_sendstr(request, "camera busy");
    }
    camera_fb_t *frame = esp_camera_fb_get();
    if (frame == NULL) {
        xSemaphoreGive(s_camera_lock);
        httpd_resp_set_status(request, "503 Unavailable");
        return httpd_resp_sendstr(request, "capture failed");
    }
    esp_err_t result = send_jpeg(request, frame);
    esp_camera_fb_return(frame);
    xSemaphoreGive(s_camera_lock);
    return result;
}

esp_err_t baby_tank_camera_stream_capture_jpeg(uint8_t **jpeg, size_t *size)
{
    ESP_RETURN_ON_FALSE(jpeg != NULL && size != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "invalid snapshot output");
    *jpeg = NULL;
    *size = 0U;
    bool temporary = !s_hardware_ready;
    if (temporary) {
        esp_err_t started = start_camera_hardware();
        if (started != ESP_OK) return started;
    }
    if (s_camera_lock == NULL ||
        xSemaphoreTake(s_camera_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        if (temporary) stop_camera_hardware();
        return ESP_ERR_TIMEOUT;
    }
    camera_fb_t *frame = esp_camera_fb_get();
    if (frame == NULL) {
        xSemaphoreGive(s_camera_lock);
        if (temporary) stop_camera_hardware();
        return ESP_FAIL;
    }
    uint8_t *converted = NULL;
    size_t length = 0U;
    bool valid = frame2jpg(frame, CAMERA_JPEG_QUALITY, &converted, &length);
    esp_camera_fb_return(frame);
    xSemaphoreGive(s_camera_lock);
    if (temporary) stop_camera_hardware();
    if (!valid || converted == NULL || length < 128U || length > 160U * 1024U) {
        free(converted);
        return ESP_ERR_INVALID_SIZE;
    }
    *jpeg = converted;
    *size = length;
    return ESP_OK;
}

static void stream_producer_task(void *argument)
{
    stream_pipeline_t *pipeline = argument;
    while (!atomic_load(&pipeline->stop) && !atomic_load(&s_stopping)) {
        uint8_t index = 0U;
        if (xQueueReceive(pipeline->free_buffers, &index,
                          pdMS_TO_TICKS(100)) != pdTRUE) continue;
        if (atomic_load(&pipeline->stop) || atomic_load(&s_stopping)) {
            (void)xQueueSend(pipeline->free_buffers, &index, 0);
            break;
        }
        int64_t started = esp_timer_get_time();
        if (xSemaphoreTake(s_camera_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
            (void)xQueueSend(pipeline->free_buffers, &index, 0);
            continue;
        }
        camera_fb_t *frame = esp_camera_fb_get();
        if (frame == NULL) {
            xSemaphoreGive(s_camera_lock);
            (void)xQueueSend(pipeline->free_buffers, &index, 0);
            continue;
        }
        int64_t captured = esp_timer_get_time();
        size_t length = 0U;
        bool encoded = encode_stream_jpeg(frame, s_jpeg_output[index], &length);
        esp_camera_fb_return(frame);
        xSemaphoreGive(s_camera_lock);
        int64_t finished = esp_timer_get_time();
        if (!encoded) {
            (void)xQueueSend(pipeline->free_buffers, &index, 0);
            continue;
        }
        stream_frame_t ready = {
            .index = index,
            .length = length,
            .capture_ms = (captured - started) / 1000,
            .encode_ms = (finished - captured) / 1000,
            .ready_us = finished,
        };
        if (xQueueSend(pipeline->ready_frames, &ready, 0) != pdTRUE) {
            /* The network/UI is behind. Replace the queued frame so latency
             * stays bounded instead of replaying stale camera images. */
            stream_frame_t stale = {0};
            if (xQueueReceive(pipeline->ready_frames, &stale, 0) == pdTRUE) {
                atomic_fetch_add(&pipeline->dropped, 1U);
                (void)xQueueSend(pipeline->free_buffers, &stale.index, 0);
            }
            if (xQueueSend(pipeline->ready_frames, &ready, 0) != pdTRUE) {
                (void)xQueueSend(pipeline->free_buffers, &index, 0);
            }
        }
        int64_t elapsed_ms = (esp_timer_get_time() - started) / 1000;
        if (elapsed_ms < CAMERA_FRAME_INTERVAL_MS) {
            vTaskDelay(pdMS_TO_TICKS(CAMERA_FRAME_INTERVAL_MS - elapsed_ms));
        }
    }
    xTaskNotifyGive(pipeline->consumer);
    vTaskDelete(NULL);
}

static esp_err_t stream_handler(httpd_req_t *request)
{
    if (!authorize_lan(request)) return ESP_OK;
    if (xSemaphoreTake(s_stream_lock, 0) != pdTRUE) {
        httpd_resp_set_status(request, "429 Too Many Requests");
        return httpd_resp_sendstr(request, "one video client is already active");
    }
    /* Multipart framing adds a very small chunk header before every JPEG.
     * Do not let that header wait for Nagle/delayed-ACK coalescing: the JPEG
     * already is the useful batching unit and each frame is latency-sensitive. */
    int socket_fd = httpd_req_to_sockfd(request);
    int no_delay = 1;
    const struct timeval send_timeout = {
        .tv_sec = 3,
        .tv_usec = 0,
    };
    if (socket_fd < 0 ||
        setsockopt(socket_fd, IPPROTO_TCP, TCP_NODELAY, &no_delay,
                   sizeof(no_delay)) < 0) {
        ESP_LOGW(TAG, "unable to enable TCP_NODELAY for video stream");
    }
    if (socket_fd >= 0 &&
        setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout,
                   sizeof(send_timeout)) < 0) {
        ESP_LOGW(TAG, "unable to set video send timeout");
    }
    httpd_resp_set_type(request, STREAM_TYPE);
    httpd_resp_set_hdr(request, "Cache-Control", "no-store, max-age=0");
    httpd_resp_set_hdr(request, "Connection", "close");
    esp_err_t result = ESP_OK;
    uint32_t frame_number = 0;
    stream_pipeline_t pipeline = {
        .free_buffers = xQueueCreate(CAMERA_JPEG_BUFFER_COUNT, sizeof(uint8_t)),
        .ready_frames = xQueueCreate(1U, sizeof(stream_frame_t)),
        .consumer = xTaskGetCurrentTaskHandle(),
    };
    if (pipeline.free_buffers == NULL || pipeline.ready_frames == NULL) {
        if (pipeline.free_buffers != NULL) vQueueDelete(pipeline.free_buffers);
        if (pipeline.ready_frames != NULL) vQueueDelete(pipeline.ready_frames);
        xSemaphoreGive(s_stream_lock);
        return ESP_ERR_NO_MEM;
    }
    for (uint8_t index = 0U; index < CAMERA_JPEG_BUFFER_COUNT; ++index) {
        (void)xQueueSend(pipeline.free_buffers, &index, 0);
    }
    TaskHandle_t producer = NULL;
    if (xTaskCreatePinnedToCore(stream_producer_task, "camera_encode", 5120U,
                                &pipeline, 4, &producer, 1) != pdPASS) {
        vQueueDelete(pipeline.free_buffers);
        vQueueDelete(pipeline.ready_frames);
        xSemaphoreGive(s_stream_lock);
        return ESP_ERR_NO_MEM;
    }
    while (result == ESP_OK && !atomic_load(&s_stopping)) {
        stream_frame_t frame = {0};
        if (xQueueReceive(pipeline.ready_frames, &frame,
                          pdMS_TO_TICKS(2000)) != pdTRUE) {
            result = ESP_ERR_TIMEOUT;
            break;
        }
        int64_t sending = esp_timer_get_time();
        uint8_t *output = s_jpeg_output[frame.index];
        size_t length = frame.length;

        char part[64];
        int part_length = snprintf(part, sizeof(part), STREAM_PART, length);
        if (part_length <= 0 || (size_t)part_length >= sizeof(part)) {
            result = ESP_ERR_INVALID_SIZE;
        } else {
            /* One HTTP chunk avoids three small writes and their Wi-Fi/TCP
             * scheduling overhead. Prefix sits immediately before JPEG. */
            size_t boundary_length = strlen(STREAM_BOUNDARY);
            size_t prefix_length = boundary_length + (size_t)part_length;
            if (prefix_length > STREAM_PREFIX_CAPACITY) {
                result = ESP_ERR_INVALID_SIZE;
            } else {
                uint8_t *combined = output + STREAM_PREFIX_CAPACITY - prefix_length;
                memcpy(combined, STREAM_BOUNDARY, boundary_length);
                memcpy(combined + boundary_length, part,
                       (size_t)part_length);
                result = httpd_resp_send_chunk(request,
                                               (const char *)combined,
                                               prefix_length + length);
            }
        }
        int64_t sent = esp_timer_get_time();
        (void)xQueueSend(pipeline.free_buffers, &frame.index,
                         pdMS_TO_TICKS(100));
        if (result == ESP_OK && ++frame_number % 10 == 0) {
            ESP_LOGI(TAG, "stream frame=%lu bytes=%zu capture=%lld ms encode=%lld ms queue=%lld ms send=%lld ms dropped=%lu",
                     (unsigned long)frame_number, length,
                     (long long)frame.capture_ms,
                     (long long)frame.encode_ms,
                     (long long)((sending - frame.ready_us) / 1000),
                     (long long)((sent - sending) / 1000),
                     (unsigned long)atomic_load(&pipeline.dropped));
        }
    }
    atomic_store(&pipeline.stop, true);
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1500)) == 0U && producer != NULL) {
        vTaskDelete(producer);
    }
    vQueueDelete(pipeline.free_buffers);
    vQueueDelete(pipeline.ready_frames);
    xSemaphoreGive(s_stream_lock);
    ESP_LOGI(TAG, "video client disconnected: %s", esp_err_to_name(result));
    return result;
}

static esp_err_t register_camera_routes(httpd_handle_t server, bool lan)
{
    void *route_context = lan ? (void *)&s_lan_route : NULL;
    const httpd_uri_t health = {
        .uri = "/health", .method = HTTP_GET, .handler = health_handler,
        .user_ctx = route_context,
    };
    const httpd_uri_t snapshot = {
        .uri = "/snapshot.jpg", .method = HTTP_GET,
        .handler = snapshot_handler, .user_ctx = route_context,
    };
    const httpd_uri_t stream = {
        .uri = "/stream", .method = HTTP_GET, .handler = stream_handler,
        .user_ctx = route_context,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &health), TAG,
                        "health route failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &snapshot), TAG,
                        "snapshot route failed");
    return httpd_register_uri_handler(server, &stream);
}

esp_err_t baby_tank_camera_stream_register_ap_routes(void)
{
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG,
                        "camera is not initialized");
    httpd_handle_t server = baby_tank_wifi_portal_server();
    ESP_RETURN_ON_FALSE(server != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "AP HTTP listener is unavailable");
    return register_camera_routes(server, false);
}

static esp_err_t start_http_servers(void)
{
    /* The setup portal owns port 80 and binds it to the robot AP. Camera
     * routes share that listener, so they cannot leak onto campus Wi-Fi. */
    httpd_handle_t server = baby_tank_wifi_portal_server();
    ESP_RETURN_ON_FALSE(server != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "robot hotspot HTTP server is unavailable");

    ESP_RETURN_ON_ERROR(register_camera_routes(server, false), TAG,
                        "AP camera routes failed");
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    ESP_RETURN_ON_FALSE(sta != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "station network is unavailable");
    memset(&s_sta_interface, 0, sizeof(s_sta_interface));
    ESP_RETURN_ON_ERROR(esp_netif_get_netif_impl_name(sta, s_sta_interface.ifr_name),
                        TAG, "unable to bind camera to station");
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 8081;
    config.ctrl_port = 32769;
    config.max_uri_handlers = 3;
    config.max_open_sockets = 3;
    config.lru_purge_enable = true;
    config.stack_size = 6144;
    config.task_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    config.if_name = &s_sta_interface;
    ESP_RETURN_ON_ERROR(httpd_start(&s_lan_server, &config), TAG,
                        "LAN camera listener failed");
    return register_camera_routes(s_lan_server, true);
}

static esp_err_t start_camera_hardware(void)
{
    if (s_hardware_ready) return ESP_OK;
    if (s_camera_lock == NULL) s_camera_lock = xSemaphoreCreateMutex();
    if (s_stream_lock == NULL) s_stream_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_camera_lock != NULL && s_stream_lock != NULL,
                        ESP_ERR_NO_MEM, TAG, "camera locks unavailable");
    const camera_config_t config = {
        .pin_pwdn = CAMERA_PIN_PWDN,
        .pin_reset = CAMERA_PIN_RESET,
        .pin_xclk = CAMERA_PIN_XCLK,
        /* GC2145 and ES8311 share GPIO4/5. Audio initializes I2C1 first;
         * esp32-camera must attach to that bus instead of acquiring it again. */
        .pin_sccb_sda = -1,
        .pin_sccb_scl = -1,
        .sccb_i2c_port = CAMERA_I2C_PORT,
        .pin_d7 = CAMERA_PIN_D7,
        .pin_d6 = CAMERA_PIN_D6,
        .pin_d5 = CAMERA_PIN_D5,
        .pin_d4 = CAMERA_PIN_D4,
        .pin_d3 = CAMERA_PIN_D3,
        .pin_d2 = CAMERA_PIN_D2,
        .pin_d1 = CAMERA_PIN_D1,
        .pin_d0 = CAMERA_PIN_D0,
        .pin_vsync = CAMERA_PIN_VSYNC,
        .pin_href = CAMERA_PIN_HREF,
        .pin_pclk = CAMERA_PIN_PCLK,
        .xclk_freq_hz = CAMERA_XCLK_HZ,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        /* GC2145 has no hardware JPEG output. Capture RGB565 in PSRAM,
         * then encode for the network with esp_new_jpeg when available. */
        .pixel_format = PIXFORMAT_RGB565,
        .frame_size = CAMERA_FRAME_SIZE,
        .jpeg_quality = 0,
        /* Two PSRAM frame buffers keep capture and encoding independent. The
         * camera DMA itself is deliberately staged through internal SRAM;
         * direct PSRAM DMA corrupts RGB565 rows on this ESP32-S3 setup. */
        .fb_count = 2,
        .fb_location = CAMERA_FB_IN_PSRAM,
        /* A preview should show the newest available frame. If networking
         * briefly stalls, discard the older sensor buffer instead of showing
         * it later and accumulating visible control latency. */
        .grab_mode = CAMERA_GRAB_LATEST,
    };
    ESP_RETURN_ON_ERROR(esp_camera_init(&config), TAG,
                        "GC2145 initialization failed");
    s_hardware_ready = true;
    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor == NULL || sensor->set_framesize(sensor, CAMERA_FRAME_SIZE) != 0) {
        stop_camera_hardware();
        return ESP_FAIL;
    }
    bool output_ready = true;
    for (size_t index = 0U; index < CAMERA_JPEG_BUFFER_COUNT; ++index) {
        s_jpeg_output[index] = heap_caps_malloc(
            CAMERA_JPEG_OUTPUT_CAPACITY,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_jpeg_output[index] == NULL) output_ready = false;
    }
    if (!output_ready) {
        for (size_t index = 0U; index < CAMERA_JPEG_BUFFER_COUNT; ++index) {
            free(s_jpeg_output[index]);
            s_jpeg_output[index] = NULL;
        }
        stop_camera_hardware();
        return ESP_ERR_NO_MEM;
    }
    jpeg_enc_config_t encoder_config = DEFAULT_JPEG_ENC_CONFIG();
    encoder_config.width = CAMERA_WIDTH;
    encoder_config.height = CAMERA_HEIGHT;
    encoder_config.src_type = JPEG_PIXEL_FORMAT_RGB565_BE;
    encoder_config.subsampling = JPEG_SUBSAMPLE_420;
    encoder_config.quality = CAMERA_JPEG_QUALITY;
    jpeg_error_t encoder_status = jpeg_enc_open(&encoder_config,
                                                &s_jpeg_encoder);
    if (encoder_status != JPEG_ERR_OK) {
        ESP_LOGW(TAG, "optimized JPEG encoder unavailable: %d",
                 encoder_status);
        s_jpeg_encoder = NULL;
    }
    ESP_LOGI(TAG, "JPEG encoder=%s internal_free=%u largest=%u",
             s_jpeg_encoder != NULL ? "esp_new_jpeg" : "reference fallback",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    return ESP_OK;
}

static void stop_camera_hardware(void)
{
    if (s_jpeg_encoder != NULL) {
        jpeg_enc_close(s_jpeg_encoder);
        s_jpeg_encoder = NULL;
    }
    for (size_t index = 0U; index < CAMERA_JPEG_BUFFER_COUNT; ++index) {
        free(s_jpeg_output[index]);
        s_jpeg_output[index] = NULL;
    }
    if (s_hardware_ready) {
        esp_err_t result = esp_camera_deinit();
        if (result != ESP_OK) {
            ESP_LOGW(TAG, "camera deinit failed: %s", esp_err_to_name(result));
        }
        s_hardware_ready = false;
    }
}

esp_err_t baby_tank_camera_stream_start(void)
{
    ESP_RETURN_ON_FALSE(!s_ready, ESP_ERR_INVALID_STATE, TAG,
                        "camera stream already started");
    atomic_store(&s_stopping, false);
    ESP_RETURN_ON_ERROR(start_camera_hardware(), TAG,
                        "camera hardware failed");
    /* A remote-mode heartbeat or a short BLE reconnect can restart the camera.
     * Keep the authenticated LAN token stable for this boot so the existing
     * phone stream does not immediately become unauthorized. */
    if (!s_lan_token_ready) {
        esp_fill_random(s_lan_token, sizeof(s_lan_token));
        s_lan_token_ready = true;
    }
    esp_err_t result = start_http_servers();
    if (result != ESP_OK) {
        (void)baby_tank_wifi_portal_restart();
        stop_camera_hardware();
        return result;
    }
    s_ready = true;
    ESP_LOGI(TAG,
             "GC2145 RGB565/JPEG camera ready at "
             "http://192.168.4.1/stream");
    return ESP_OK;
}

esp_err_t baby_tank_camera_stream_stop(void)
{
    if (!s_ready && !s_hardware_ready) return ESP_OK;
    atomic_store(&s_stopping, true);
    s_ready = false;

    esp_err_t result = baby_tank_wifi_portal_stop();
    if (s_lan_server != NULL) {
        esp_err_t stopped = httpd_stop(s_lan_server);
        s_lan_server = NULL;
        if (result == ESP_OK) result = stopped;
    }
    stop_camera_hardware();
    if (s_camera_lock != NULL) {
        vSemaphoreDelete(s_camera_lock);
        s_camera_lock = NULL;
    }
    if (s_stream_lock != NULL) {
        vSemaphoreDelete(s_stream_lock);
        s_stream_lock = NULL;
    }
    memset(&s_sta_interface, 0, sizeof(s_sta_interface));

    esp_err_t restarted = baby_tank_wifi_portal_start();
    if (result == ESP_OK) result = restarted;
    atomic_store(&s_stopping, false);
    ESP_LOGI(TAG, "camera stream stopped; dynamic buffers released");
    return result;
}

bool baby_tank_camera_stream_is_ready(void)
{
    return s_ready;
}

esp_err_t baby_tank_camera_stream_lan_info(uint8_t ip[4], uint8_t token[16])
{
    ESP_RETURN_ON_FALSE(ip != NULL && token != NULL && s_ready,
                        ESP_ERR_INVALID_STATE, TAG, "LAN camera unavailable");
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info = {0};
    ESP_RETURN_ON_FALSE(sta != NULL &&
                            esp_netif_get_ip_info(sta, &info) == ESP_OK &&
                            info.ip.addr != 0,
                        ESP_ERR_INVALID_STATE, TAG, "station has no address");
    memcpy(ip, &info.ip.addr, 4);
    memcpy(token, s_lan_token, sizeof(s_lan_token));
    return ESP_OK;
}
