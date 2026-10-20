idf_component_register(
    SRCS "hci_check.c" "usb_model.c"
        "../../../components/gpsp_devices/usb_bt.c"
        "../../../components/gpsp_devices/usb_bt_packet.c"
    INCLUDE_DIRS "../../../components/gpsp_devices"
    PRIV_REQUIRES bt espressif__usb esp_timer esp_psram nvs_flash
)
