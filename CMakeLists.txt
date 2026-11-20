cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(PROJECT_VER "0.1.0")
# LVGL renders straight into the panel's byte order (RGB565 big-endian), so
# nothing is swapped per frame. 9.3's Kconfig has no switch for it.
idf_build_set_property(COMPILE_DEFINITIONS "LV_DRAW_SW_SUPPORT_RGB565_SWAPPED=1" APPEND)
project(desk_knob)
