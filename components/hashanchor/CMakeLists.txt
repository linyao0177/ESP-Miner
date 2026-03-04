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
        "esp_crt_bundle"
)

target_include_directories(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../main"
)
