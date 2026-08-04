cmake_minimum_required(VERSION 3.15)

find_package(bouffalo_sdk REQUIRED HINTS $ENV{BL_SDK_BASE})

sdk_add_include_directories(
    .
    bluetooth
    config
    debug
    ds5
    platform
    usb
    ${CMAKE_CURRENT_LIST_DIR}/../../lib/WDL/WDL
)

set(DS5_WDL_RESAMPLE_SOURCE
    ${CMAKE_CURRENT_LIST_DIR}/../../lib/WDL/WDL/resample.cpp
)

target_sources(app PRIVATE
    bluetooth/ds5_bt.c
    bluetooth/ds5_bt_policy.c
    bluetooth/ds5_l2cap.c
    debug/ds5_log.c
    ds5/ds5_feature_cache.c
    ds5/ds5_protocol.c
    platform/ds5_cpp_runtime.cpp
    platform/ds5_haptics_mailbox.c
    platform/ds5_input_mailbox.c
    platform/ds5_output_mailbox.c
    usb/ds5_usb_audio.cpp
    usb/ds5_usb_hid.c
    usb/ds5_usb_log.c
    ${DS5_WDL_RESAMPLE_SOURCE}
)

set_source_files_properties(
    usb/ds5_usb_audio.cpp
    ${DS5_WDL_RESAMPLE_SOURCE}
    PROPERTIES COMPILE_OPTIONS "-fsigned-char"
)

sdk_set_main_file(main.c)

project(ds5dongle_bl616)

# The WDL resampler is C++ but does not use the C++ standard library.  Keep the
# firmware link on the SDK's bare-metal C runtime instead of pulling in
# libstdc++ exception/TLS support through the g++ driver.
set_target_properties(ds5dongle_bl616_bl616.elf PROPERTIES LINKER_LANGUAGE C)
set(CMAKE_CXX_IMPLICIT_LINK_LIBRARIES "")
