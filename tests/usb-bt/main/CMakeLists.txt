idf_component_register(
    SRCS "bt_check.c" "../../usb-bt-hci/main/usb_model.c" "menu_check.c" "audio_mute_check.c"
        "../../../main/gpsp_settings.c" "../../../main/gpsp_ui.c"
        "../../../components/gpsp_devices/usb_bt_packet.c"
        "../../../components/gpsp_devices/usb_bt.c"
        "../../../components/gpsp_devices/playstation_pad.c"
        "../../../components/gpsp_devices/hid_gamepad.cc"
        "../../../components/gpsp_devices/third_party/hid_report_parser/hid_report_parser.cpp"
    INCLUDE_DIRS "../../usb-bt-hci/main"
        "../../../components/gpsp_devices"
        "../../../components/gpsp_devices/include"
        "../../../main/include" "../../../components/gpsp_platform/include"
        "../../../components/gpsp/core/libretro/libretro-common/include"
    PRIV_REQUIRES bt gpsp_font gpsp_display espressif__usb nvs_flash esp_timer esp_psram
        esp_driver_i2c esp_driver_i2s esp_driver_gpio
)
target_include_directories(${COMPONENT_LIB} SYSTEM PRIVATE
    "../../../components/gpsp_devices/third_party/hid_report_parser")
set_source_files_properties(
    "../../../components/gpsp_devices/third_party/hid_report_parser/hid_report_parser.cpp"
    PROPERTIES COMPILE_OPTIONS "-w")
target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=i2c_master_transmit" "-Wl,--wrap=i2s_channel_write"
    "-Wl,--wrap=esp_bt_hid_host_connect" "-Wl,--wrap=esp_bt_hid_host_disconnect"
    "-Wl,--wrap=esp_bt_gap_get_bond_device_list" "-Wl,--wrap=esp_bt_gap_remove_bond_device"
    "-Wl,--wrap=esp_bt_gap_set_scan_mode")
