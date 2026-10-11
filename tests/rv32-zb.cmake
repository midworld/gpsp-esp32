# Copyright (C) 2026 Midworld Kim
# SPDX-License-Identifier: GPL-3.0-or-later

# Generated-code extensions for ESP-IDF tests; defaults match the P4
# firmware (Zb, RVC and Zcb on). Override with idf.py
# -D GPSP_RV32_USE_ZBB=0, -D GPSP_RV32_USE_ZCB=0 or -D GPSP_RV32_USE_RVC=0,
# for example.
function(gpsp_rv32_zb_flags target)
    foreach(ext ZBA ZBB ZBS RVC ZCB)
        set(value 1)
        if(DEFINED GPSP_RV32_USE_${ext})
            string(TOUPPER "${GPSP_RV32_USE_${ext}}" value)
            if(value MATCHES "^(1|ON|TRUE|YES)$")
                set(value 1)
            elseif(value MATCHES "^(0|OFF|FALSE|NO)$")
                set(value 0)
            else()
                message(FATAL_ERROR "GPSP_RV32_USE_${ext} must be 0/1, "
                    "ON/OFF, TRUE/FALSE or YES/NO: ${GPSP_RV32_USE_${ext}}")
            endif()
        endif()
        target_compile_definitions(${target} PRIVATE
            GPSP_RV32_USE_${ext}=${value})
    endforeach()
    # -D GPSP_TEST_GOLDEN=ON prints generated code for rv32-golden.py.
    if(GPSP_TEST_GOLDEN)
        target_compile_definitions(${target} PRIVATE GPSP_TEST_GOLDEN=1)
    endif()
endfunction()
