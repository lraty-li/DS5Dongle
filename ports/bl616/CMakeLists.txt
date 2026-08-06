cmake_minimum_required(VERSION 3.15)

find_package(bouffalo_sdk REQUIRED HINTS $ENV{BL_SDK_BASE})

set(DS5_OPUS_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../lib/opus)
include(${DS5_OPUS_ROOT}/cmake/OpusFunctions.cmake)

get_opus_sources(OPUS_SOURCES ${DS5_OPUS_ROOT}/opus_sources.mk
                 DS5_OPUS_CORE_SOURCES)
get_opus_sources(OPUS_SOURCES_FLOAT ${DS5_OPUS_ROOT}/opus_sources.mk
                 DS5_OPUS_FLOAT_SOURCES)
get_opus_sources(CELT_SOURCES ${DS5_OPUS_ROOT}/celt_sources.mk
                 DS5_OPUS_CELT_SOURCES)
get_opus_sources(SILK_SOURCES ${DS5_OPUS_ROOT}/silk_sources.mk
                 DS5_OPUS_SILK_SOURCES)
get_opus_sources(SILK_SOURCES_FLOAT ${DS5_OPUS_ROOT}/silk_sources.mk
                 DS5_OPUS_SILK_FLOAT_SOURCES)

set(DS5_OPUS_SOURCES)
foreach(DS5_OPUS_SOURCE IN LISTS
        DS5_OPUS_CORE_SOURCES
        DS5_OPUS_FLOAT_SOURCES
        DS5_OPUS_CELT_SOURCES
        DS5_OPUS_SILK_SOURCES
        DS5_OPUS_SILK_FLOAT_SOURCES)
    list(APPEND DS5_OPUS_SOURCES ${DS5_OPUS_ROOT}/${DS5_OPUS_SOURCE})
endforeach()

# libopus' top-level CMake project cannot be nested below BouffaloSDK: its
# project() hook recreates SDK-wide custom targets.  Its source lists are
# instead compiled by the SDK-owned app target, retaining the exact BL616 ABI
# and compiler flags selected by BouffaloSDK.

sdk_add_include_directories(
    .
    bluetooth
    config
    debug
    ds5
    platform
    usb
    ${CMAKE_CURRENT_LIST_DIR}/../../lib/WDL/WDL
    ${DS5_OPUS_ROOT}
    ${DS5_OPUS_ROOT}/dnn
    ${DS5_OPUS_ROOT}/celt
    ${DS5_OPUS_ROOT}/include
    ${DS5_OPUS_ROOT}/silk
    ${DS5_OPUS_ROOT}/silk/float
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
    platform/ds5_audio_mailbox.c
    platform/ds5_cpp_runtime.cpp
    platform/ds5_feature_set_mailbox.c
    platform/ds5_haptics_mailbox.c
    platform/ds5_input_mailbox.c
    platform/ds5_output_mailbox.c
    usb/ds5_usb_audio.cpp
    usb/ds5_usb_audio_adapter.c
    usb/ds5_usb_hid.c
    usb/ds5_usb_log.c
    ${DS5_WDL_RESAMPLE_SOURCE}
    ${DS5_OPUS_SOURCES}
)

set_source_files_properties(
    ${DS5_OPUS_SOURCES}
    PROPERTIES COMPILE_DEFINITIONS
    "OPUS_BUILD;VAR_ARRAYS;HAVE_LRINT;HAVE_LRINTF"
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
