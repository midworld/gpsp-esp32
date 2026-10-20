# Copyright (C) 2026 Midworld Kim
# SPDX-License-Identifier: GPL-3.0-or-later

idf_component_register(
    SRCS "dsi_check.c" "settings_check.c"
        "../../../components/gpsp_video/lcd.c"
        "../../../components/gpsp_devices/i2c_bus.c"
        "../../../components/gpsp_video/spi.c"
        "../../../components/gpsp_video/dsi_hud.c"
        "../../../components/gpsp_video/panels/waveshare_dsi.c"
        "../../../components/gpsp_video/panels/osptek_ydp430bt009.c"
        "../../../boards/${GPSP_BOARD}/board.c"
        "../../../main/gpsp_settings.c"
        "../../../main/gpsp_ui.c"
    INCLUDE_DIRS "../../../components/gpsp_devices/include"
        "../../../components/gpsp_video/include" "../../../components/gpsp_video"
        "../../../components/gpsp/core/include"
        "../../../components/gpsp/core/libretro/libretro-common/include"
        "../../../components/gpsp_platform/include" "../../../main/include"
    PRIV_REQUIRES esp_driver_gpio esp_driver_i2c esp_driver_spi
        esp_driver_ppa esp_driver_dma esp_lcd esp_mm esp_timer esp_psram gpsp_font gpsp_display
)

if(CONFIG_DSI_CHECK_MODEL)
    target_sources(${COMPONENT_LIB} PRIVATE "display_model.c")
    foreach(symbol IN ITEMS
            esp_ldo_acquire_channel esp_ldo_release_channel
            i2c_new_master_bus i2c_del_master_bus
            i2c_master_bus_add_device i2c_master_bus_rm_device i2c_master_transmit
            esp_lcd_new_dsi_bus esp_lcd_del_dsi_bus esp_lcd_new_panel_io_dbi
            esp_lcd_panel_io_tx_param esp_lcd_panel_io_del esp_lcd_new_panel_dpi
            esp_lcd_dpi_panel_get_frame_buffer esp_lcd_dpi_panel_register_event_callbacks
            esp_lcd_panel_init esp_lcd_panel_del esp_lcd_panel_draw_bitmap
            ppa_register_client ppa_unregister_client
            ppa_client_register_event_callbacks ppa_do_scale_rotate_mirror ppa_do_blend)
        target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=${symbol}")
    endforeach()
    if(CONFIG_GPSP_LCD_DSI_DMA_EXPAND)
        foreach(symbol IN ITEMS esp_async_color_convert_install_dma2d
                esp_async_color_convert_uninstall esp_async_color_convert)
            target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=${symbol}")
        endforeach()
    endif()
endif()
