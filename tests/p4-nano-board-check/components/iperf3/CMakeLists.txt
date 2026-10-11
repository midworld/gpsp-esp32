idf_component_register(
    SRCS
        "src/cjson.c"
        "src/dscp.c"
        "src/esp_iperf_io.c"
        "src/iperf_api.c"
        "src/iperf_auth.c"
        "src/iperf_client_api.c"
        "src/iperf_error.c"
        "src/iperf_locale.c"
        "src/iperf_sctp.c"
        "src/iperf_server_api.c"
        "src/iperf_tcp.c"
        "src/iperf_time.c"
        "src/iperf_udp.c"
        "src/iperf_util.c"
        "src/net.c"
        "src/tcp_info.c"
        "src/timer.c"
        "src/units.c"
    INCLUDE_DIRS "src"
    REQUIRES lwip pthread heap esp_hw_support)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error)
