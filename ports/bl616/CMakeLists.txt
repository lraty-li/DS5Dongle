cmake_minimum_required(VERSION 3.15)

find_package(bouffalo_sdk REQUIRED HINTS $ENV{BL_SDK_BASE})

sdk_add_include_directories(. bluetooth config debug ds5 usb)

target_sources(app PRIVATE
    bluetooth/ds5_bt.c
    bluetooth/ds5_bt_policy.c
    bluetooth/ds5_l2cap.c
    debug/ds5_log.c
    ds5/ds5_protocol.c
    usb/ds5_usb_log.c
)

sdk_set_main_file(main.c)

project(ds5dongle_bl616)
