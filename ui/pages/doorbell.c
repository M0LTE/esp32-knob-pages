// Doorbell page: the door camera, near-live, and a call when someone rings.
// Frames come from Home Assistant as JPEG and are decoded by the device, only
// while this page is showing, a few a second (the camera's own video is H.264,
// which this chip cannot decode). The picture fills the glass at 4:3; a label
// at the top says which door and when it rang. A ring raises a CALL: the
// screen wakes, the knob jumps here with its own chime, and after 90 s or a
// tap it goes back to the page it was on.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../model.h"
#include "../page.h"
#include "../theme.h"

#define RING_SHOW_MS 90000

typedef struct {
    lv_obj_t *canvas;
    uint32_t now, shown_ms;
    bool was_visible;
    lv_draw_buf_t *img;
    uint32_t seen_frame;
    bool have_frame;
    uint16_t w, h;
    // snapshot
    bool up;
    char name[20];
    uint32_t ring_seq, ring_wall;
    float fps;
    // rings
    uint32_t seen_ring;
    uint32_t ring_ms;          // when the latest ring was seen, 0 = none
    bool dismissed;
} door_t;

static bool ringing(const door_t *s) { return s->ring_ms && !s->dismissed && s->now - s->ring_ms < RING_SHOW_MS; }

static void label_box(lv_layer_t *l, int cx, int y, int w, int h, lv_color_t border)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = C_BG;
    d.bg_opa = 200;
    d.radius = h / 2;
    d.border_color = border;
    d.border_width = 1;
    d.border_opa = 200;
    lv_area_t a = {cx - w / 2, y, cx + w / 2, y + h};
    lv_draw_rect(l, &d, &a);
}

static void draw_body(lv_event_t *e)
{
    door_t *s = lv_event_get_user_data(e);
    lv_layer_t *l = lv_event_get_layer(e);
    char b[48];
    if (s->img && s->have_frame) {
        lv_draw_image_dsc_t d;
        lv_draw_image_dsc_init(&d);
        d.src = s->img;
        int x0 = SCR_CX - s->w / 2, y0 = SCR_CY - s->h / 2;
        lv_area_t a = {x0, y0, x0 + s->w - 1, y0 + s->h - 1};
        lv_draw_image(l, &d, &a);
    } else {
        th_text(l, &font_mono_13, SCR_CX, SCR_CY - 8, 240, LV_TEXT_ALIGN_CENTER, C_DIM, LV_OPA_COVER, 2,
                s->up ? "CONNECTING CAMERA" : "SET THE DOORBELL ON THE SETTINGS PAGE");
    }
    bool ring = ringing(s);
    // Top label: which door, and when it rang.
    if (ring && s->ring_wall) {
        time_t t = s->ring_wall;
        struct tm tm;
        localtime_r(&t, &tm);
        uint32_t ago = (s->now - s->ring_ms) / 1000;
        snprintf(b, sizeof b, "%s  %02d:%02d  %us ago", s->name, tm.tm_hour, tm.tm_min, (unsigned)ago);
    } else {
        snprintf(b, sizeof b, "%s  LIVE", s->name);
    }
    int w = (int)strlen(b) * 7 + 24;
    label_box(l, SCR_CX, 48, w, 22, ring ? C_ACCENT : C_FAINT);
    th_text(l, &font_mono_11, SCR_CX, 52, w, LV_TEXT_ALIGN_CENTER, ring ? C_ACCENT : C_INK, LV_OPA_COVER, 1, b);
    if (ring) {
        // A pulse round the rim for the first seconds of a ring.
        uint32_t since = s->now - s->ring_ms;
        if (since < 8000) {
            float k = (since % 1000) / 1000.0f;
            th_ring(l, SCR_CX, SCR_CY, 176, 3, C_ACCENT, (lv_opa_t)(220 * (1 - k)));
        }
        label_box(l, SCR_CX, 312, 140, 22, C_FAINT);
        th_text(l, &font_mono_11, SCR_CX, 316, 140, LV_TEXT_ALIGN_CENTER, C_SUB, LV_OPA_COVER, 2, "TAP TO DISMISS");
    } else if (s->have_frame && s->fps > 0) {
        snprintf(b, sizeof b, "%.1f fps", s->fps);
        th_text(l, &font_mono_11, SCR_CX, 316, 100, LV_TEXT_ALIGN_CENTER, C_DIM, LV_OPA_COVER, 0, b);
    }
}

TH_PAGE_DRAW(draw_cb, draw_body, (lv_obj_t *)lv_event_get_current_target(e))

static void create(page_t *p, lv_obj_t *root)
{
    door_t *s = calloc(1, sizeof *s);
    p->state = s;
    s->canvas = lv_obj_create(root);
    lv_obj_remove_style_all(s->canvas);
    lv_obj_set_size(s->canvas, SCR_W, SCR_H);
    lv_obj_remove_flag(s->canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s->canvas, draw_cb, LV_EVENT_DRAW_MAIN, s);
}

static void destroy(page_t *p)
{
    door_t *s = p->state;
    if (s->img) lv_draw_buf_destroy(s->img);
    free(s);
}

static void tick(page_t *p, uint32_t now, uint32_t dt, bool visible)
{
    door_t *s = p->state;
    s->now = now;
    if (visible && !s->was_visible) s->shown_ms = now;
    s->was_visible = visible;
    model_lock();
    g_model.door_want_video = visible || ringing(s);
    s->up = g_model.door_up;
    snprintf(s->name, sizeof s->name, "%s", g_model.door_name[0] ? g_model.door_name : "DOOR");
    s->ring_seq = g_model.door_ring_seq;
    s->ring_wall = g_model.door_ring_wall;
    s->fps = g_model.door_fps;
    // Copy the newest frame into our own buffer, so the device can carry on decoding.
    if (g_model.door_frame && g_model.door_frame_seq != s->seen_frame && g_model.door_w && g_model.door_h) {
        if (!s->img || s->w != g_model.door_w || s->h != g_model.door_h) {
            if (s->img) lv_draw_buf_destroy(s->img);
            s->w = g_model.door_w;
            s->h = g_model.door_h;
            s->img = lv_draw_buf_create(s->w, s->h, lv_display_get_color_format(lv_display_get_default()), 0);
        }
        if (s->img) {
            for (int y = 0; y < s->h; y++)
                memcpy(s->img->data + (size_t)y * s->img->header.stride, g_model.door_frame + (size_t)y * s->w,
                       (size_t)s->w * 2);
            s->have_frame = true;
        }
        s->seen_frame = g_model.door_frame_seq;
    }
    model_unlock();
    if (s->ring_seq != s->seen_ring) {
        if (s->seen_ring || s->ring_seq == 1) {
            s->ring_ms = now;
            s->dismissed = false;
        }
        s->seen_ring = s->ring_seq;
    }
    if (visible) lv_obj_invalidate(s->canvas);
}

static void tap(page_t *p, int x, int y)
{
    door_t *s = p->state;
    if (ringing(s)) s->dismissed = true;
}

static alert_level_t status(page_t *p, char *why, int len)
{
    door_t *s = p->state;
    if (!ringing(s)) return ALERT_NONE;
    // The ring's sequence number keeps each ring a fresh call.
    snprintf(why, len, "%s ringing (%u)", s->name, (unsigned)s->ring_seq);
    return ALERT_CALL;
}

const page_type_t page_doorbell = {
    .type = "doorbell", .label = "Doorbell", .create = create, .destroy = destroy,
    .tick = tick, .tap = tap, .status = status,
};
