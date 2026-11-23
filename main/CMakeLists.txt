idf_component_register(SRCS "app.c" "usb_kbd.c" "usb_msc.c" "wifi.c" "channel.c" "settings.c" "keys.c" "armed.c"
                            "proto.c" "seedrules.c" "updrules.c" "lockout.c" "usbserial.c" "update.c" "led.c" "screen.c" "wlsplit.c" "wlfix.c"
                       INCLUDE_DIRS ".")

# The pure parts know the dev build by the define SAT_DEV (as ./test builds them), not by sdkconfig.h
idf_build_get_property(sat_version PROJECT_VER)
if(CONFIG_SAT_DEV)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SAT_DEV)
    if(NOT sat_version MATCHES "-dev$")
        message(FATAL_ERROR "a dev build's version must end in -dev (it is ${sat_version}): build it with ./build --dev")
    endif()
elseif(sat_version MATCHES "-dev$")
    message(FATAL_ERROR "a release build's version must not end in -dev (it is ${sat_version}): build it with ./build")
endif()

# usb_msc.c answers TEST UNIT READY in front of esp_tinyusb (the media change's UNIT ATTENTION): TinyUSB's calls go to
# __wrap_tud_msc_test_unit_ready_cb, which calls esp_tinyusb's own as __real_
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=tud_msc_test_unit_ready_cb")
# wlfix.c cuts every wear-levelled read and write at the flash sectors' boundaries (ESP-IDF v5.5.6 gets a piece
# that crosses one wrong): FatFs's and esp_tinyusb's calls go to __wrap_wl_write and __wrap_wl_read. The -u: nothing
# else names wlfix.c's functions, so without it the linker leaves the file out and the wrapped calls find nothing
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=wl_write" "-Wl,--wrap=wl_read" "-u __wrap_wl_write")
