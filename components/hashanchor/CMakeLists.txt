idf_component_register(
    SRCS
        "hashanchor_task.c"
        "hashanchor_crypto.c"
        "hashanchor_http.c"
        "tweetnacl.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        "json"
        "mbedtls"
        "esp_http_client"
        "esp_timer"
        "nvs_flash"
        "asic"
        "stratum"
        "connect"
)

target_include_directories(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../main"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../main/tasks"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../main/thermal"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../main/power"
)
