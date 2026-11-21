cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# The app version is the git description of this tree (tag or commit, -dirty
# for uncommitted changes); it shows on the settings page and /api/version.
execute_process(COMMAND git describe --always --dirty WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
                OUTPUT_VARIABLE DESK_GIT_VER OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE _git_rc)
if(DESK_VERSION)
    set(PROJECT_VER "${DESK_VERSION}")   # idf.py -DDESK_VERSION=... overrides
elseif(_git_rc EQUAL 0 AND DESK_GIT_VER)
    set(PROJECT_VER "${DESK_GIT_VER}")
else()
    set(PROJECT_VER "unknown")
endif()
# LVGL renders straight into the panel's byte order (RGB565 big-endian), so
# nothing is swapped per frame. 9.3's Kconfig has no switch for it.
idf_build_set_property(COMPILE_DEFINITIONS "LV_DRAW_SW_SUPPORT_RGB565_SWAPPED=1" APPEND)
project(desk_knob)
