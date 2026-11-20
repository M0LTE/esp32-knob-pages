// LVGL settings for the desktop simulator; kept in step with the device's sdkconfig.
#ifndef LV_CONF_H
#define LV_CONF_H
#define LV_COLOR_DEPTH 16
#define LV_USE_STDLIB_MALLOC LV_STDLIB_BUILTIN
#define LV_MEM_SIZE (4 * 1024 * 1024)
#define LV_USE_OS LV_OS_NONE
#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_USE_FLOAT 1
#define LV_USE_DRAW_SW 1
#define LV_DRAW_SW_COMPLEX 1
// Break lines only at spaces and commas, never inside a number like 22.4.
#define LV_TXT_BREAK_CHARS " ,;:"
#endif
