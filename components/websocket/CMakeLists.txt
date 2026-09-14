idf_component_register(
    SRCS
        "websocket_mgr.cpp"
        "websocket_events.cpp"
        "websocket_rx.cpp"
        "websocket_json.cpp"
        "websocket_audio.cpp"
        "websocket_stack_wrap.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        esp_websocket_client
        esp-tls
        esp_event
        cjson
        mbedtls
        audio_hal
        wifi_manager
)

target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=esp_websocket_client_init"
)
