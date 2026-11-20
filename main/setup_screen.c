// Plain LVGL screens drawn outside the page system, for start-up and set-up.
#include "setup_screen.h"

#include <stdio.h>
#include <string.h>

#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "theme.h"

static lv_obj_t *new_screen(void)
{
    lv_obj_t *old = lv_screen_active();
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, C_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_screen_load(scr);
    if (old) lv_obj_delete(old);
    return scr;
}

static lv_obj_t *text(lv_obj_t *parent, const lv_font_t *f, lv_color_t c, int y, const char *s)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(l, 300);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_label_set_text(l, s);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
    return l;
}

static void ring(lv_obj_t *scr, lv_color_t c)
{
    lv_obj_t *a = lv_arc_create(scr);
    lv_obj_remove_style_all(a);
    lv_obj_set_size(a, 352, 352);
    lv_obj_center(a);
    lv_arc_set_bg_angles(a, 0, 360);
    lv_obj_set_style_arc_width(a, 2, LV_PART_MAIN);
    lv_obj_set_style_arc_color(a, c, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(a, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
}

void setup_screen_joining(const char *ssid)
{
    lvgl_port_lock(0);
    lv_obj_t *s = new_screen();
    ring(s, C_FAINT);
    text(s, &font_sans_28, C_ACCENT, 120, "Desk knob");
    char b[64];
    snprintf(b, sizeof b, "Joining %s", ssid);
    text(s, &font_mono_16, C_SUB, 170, b);
    lv_obj_t *sp = lv_spinner_create(s);
    lv_obj_set_size(sp, 34, 34);
    lv_obj_align(sp, LV_ALIGN_TOP_MID, 0, 215);
    lv_obj_set_style_arc_width(sp, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_width(sp, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(sp, C_FAINT, LV_PART_MAIN);
    lv_obj_set_style_arc_color(sp, C_ACCENT, LV_PART_INDICATOR);
    lvgl_port_unlock();
}

void setup_screen_joined(const char *ip, const char *hostname)
{
    lvgl_port_lock(0);
    lv_obj_t *s = new_screen();
    ring(s, C_FAINT);
    text(s, &font_sans_28, C_OK, 120, "Connected");
    char b[80];
    snprintf(b, sizeof b, "%s\n%s", ip, hostname);
    text(s, &font_mono_16, C_SUB, 170, b);
    lvgl_port_unlock();
}

void setup_screen_ap(const char *ssid, const char *pass, const char *ip, const char *why)
{
    lvgl_port_lock(0);
    lv_obj_t *s = new_screen();
    ring(s, C_ACCENT);
    text(s, &font_sans_20, C_ACCENT, 26, "Set-up");
    text(s, &font_mono_11, C_DIM, 52, why);

    // Scanning this joins the phone to the set-up network.
    char q[96];
    snprintf(q, sizeof q, "WIFI:T:WPA;S:%s;P:%s;;", ssid, pass);
    lv_obj_t *qr = lv_qrcode_create(s);
    lv_qrcode_set_size(qr, 116);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_qrcode_update(qr, q, strlen(q));
    lv_obj_set_style_border_color(qr, lv_color_white(), 0);
    lv_obj_set_style_border_width(qr, 6, 0);
    lv_obj_align(qr, LV_ALIGN_TOP_MID, 0, 74);

    text(s, &font_mono_11, C_DIM, 208, "WI-FI");
    text(s, &font_mono_16, C_INK, 222, ssid);
    text(s, &font_mono_11, C_DIM, 248, "PASSWORD");
    text(s, &font_mono_22, C_ACCENT, 262, pass);
    char b[64];
    snprintf(b, sizeof b, "then open http://%s", ip);
    text(s, &font_mono_13, C_SUB, 296, b);
    lvgl_port_unlock();
}

void setup_screen_clear(void)
{
    lvgl_port_lock(0);
    new_screen();
    lvgl_port_unlock();
}
