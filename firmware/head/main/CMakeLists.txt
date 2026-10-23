idf_component_register(
    SRCS "app_main.c" "display_runtime.c" "face_controller.c" "conversation.c"
         "cloud_client.c" "touch_wake.c" "device_credentials.c"
         "network_runtime.c" "audio_io.c" "voice_runtime.c"
         "control_protocol.c" "camera_stream.c" "chassis_bridge.c"
         "power_runtime.c" "runtime_policy.c" "state_event_queue.c"
         "interaction_memory.c"
         "chassis_debug_console.c" "wifi_portal.c" "diagnostics.c"
    INCLUDE_DIRS "."
    EMBED_FILES "../assets/wake-wozai.pcm" "../assets/wake-lailao.pcm"
                "../assets/remote-mode.pcm"
                "../assets/wake-please-move.pcm"
                "../assets/wake-coming-through.pcm"
                "../assets/car-horn.pcm"
                "../assets/baby-tank-touch-huhu.pcm"
                "../assets/baby-tank-touch-dont-touch.pcm"
                "../assets/baby-tank-touch-getting-angry.pcm"
                "../assets/baby-tank-touch-stop-touching.pcm"
                "../assets/baby-tank-touch-left-ticklish.pcm"
                "../assets/baby-tank-touch-left-again.pcm"
                "../assets/baby-tank-touch-left-found-me.pcm"
                "../assets/baby-tank-touch-left-itchy.pcm"
                "../assets/baby-tank-touch-left-comfy.pcm"
                "../assets/baby-tank-touch-left-like-you.pcm"
                "../assets/baby-tank-touch-right-who.pcm"
                "../assets/baby-tank-touch-right-not-startled.pcm"
                "../assets/baby-tank-touch-right-too.pcm"
                "../assets/baby-tank-touch-right-again.pcm"
                "../assets/baby-tank-touch-right-one-more.pcm"
                "../assets/baby-tank-touch-right-watching.pcm"
                "../assets/baby-tank-touch-rare-sneeze.pcm"
                "../assets/baby-tank-touch-rare-catch-me.pcm"
                "../assets/baby-tank-touch-rare-easter-egg.pcm"
                "../assets/baby-tank-touch-rare-transform.pcm"
                "../assets/game-win.pcm"
                "../assets/game-lose.pcm"
                "../assets/game-draw.pcm"
    REQUIRES
        esp_app_format
        esp_adc
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_i2s
        esp_driver_uart
        esp_driver_usb_serial_jtag
        esp_driver_spi
        esp_driver_touch_sens
        esp_event
        esp_timer
        esp_lcd
        esp_http_server
        esp_http_client
        esp_netif
        esp_psram
        esp_system
        esp_wifi
        heap
        mbedtls
        nvs_flash
        espressif__network_provisioning
        espressif__esp32-camera
        espressif__esp_new_jpeg
        espressif__esp_websocket_client
        spi_flash
)

# ESP-SR 2.6.0 is compatible with IDF 6.1, but two of its C sources are not
# const-clean under this project's stricter -Wwrite-strings setting. Keep the
# strict warning for Baby Tank sources and scope the compatibility exception to
# the managed component only.
idf_component_get_property(esp_sr_lib espressif__esp-sr COMPONENT_LIB)
target_compile_options(${esp_sr_lib} PRIVATE "-Wno-write-strings")
