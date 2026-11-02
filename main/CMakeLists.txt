idf_component_register(
    SRCS "main.cpp" "panasonic.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp-zigbee-lib esp-zboss-lib nvs_flash esp_timer app_update driver
)
