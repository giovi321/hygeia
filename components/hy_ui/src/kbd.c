/* Shared on-screen QWERTY keyboard: the note view's keyboard (maps, controls, 220 px height, Inter 26 bold with the
 * Montserrat fallback, no auto-repeat on enter and OK, the waking-stroke text restore and stroke consumption) plus a
 * one-shot shift key. All state is per keyboard: the text before the press lives in the keyboard's user data and
 * "shift armed" is the keyboard's own mode (upper map = armed), so two text areas can each own a keyboard.
 *
 * Shift sits where the left spacer of the z row was (3 units wide), so no letter key moves. Pressing it swaps to the
 * upper-case map, where the key is drawn in the text colour (#EDEDF0 fill, black arrow) and every letter shows its
 * capital. The next letter key inserts the capital and puts the lower-case map back. Space, backspace and OK leave
 * shift armed; the symbol page switch drops it, as the stock keyboard would. A second press of shift while armed
 * disarms it. */

#include <string.h>

#include "esp_log.h"
#include "ui_internal.h"

static const char *TAG = "ui_kbd";

LV_FONT_DECLARE(lv_font_montserrat_14);

/* Simplified QWERTY keyboard: ten keys in the top row so every key is about 44 px wide on the 480 px panel (the stock
 * map has 10 to 12 per row at about 35 px). A lower-case map and an upper-case map for the one-shot shift, and a
 * bottom row with the mode key, space, backspace and OK. The shorter letter rows are centred with hidden spacer
 * buttons: a key is 2 width units, a row is 20 units. The bottom row starts and ends with a 2 unit hidden spacer, so
 * its outer keys (mode and OK) stay clear of the rounded glass corners. The mode texts are the ones the stock keyboard
 * handler switches on. */
#define KB_MODE_SYMBOLS "1#"
#define KB_MODE_LETTERS "abc"
#define KB_SHIFT        LV_SYMBOL_UP
#define KB_KEY(w)       (LV_BUTTONMATRIX_CTRL_POPOVER | (w))
#define KB_MODE(w)      (LV_KEYBOARD_CTRL_BUTTON_FLAGS | (w))
#define KB_GAP(w)       (LV_BUTTONMATRIX_CTRL_HIDDEN | LV_BUTTONMATRIX_CTRL_DISABLED | (w))
#define KB_NINE         KB_KEY(1), KB_KEY(1), KB_KEY(1), KB_KEY(1), KB_KEY(1), KB_KEY(1), KB_KEY(1), KB_KEY(1), KB_KEY(1)
#define KB_W2           KB_KEY(2)
#define KB_SHIFT_UP     (LV_BUTTONMATRIX_CTRL_NO_REPEAT | LV_BUTTONMATRIX_CTRL_CLICK_TRIG | 3)
#define KB_SHIFT_ARMED  (KB_SHIFT_UP | LV_BUTTONMATRIX_CTRL_CHECKED)
#define KB_BOTTOM       KB_GAP(2), KB_MODE(3), 7, LV_BUTTONMATRIX_CTRL_CHECKED | 3, KB_MODE(3), KB_GAP(2)

/* Geometry: four rows of 56 px keys with 4 px gaps and 4 px padding, so HY_KBD_H is 244. The keyboard sits at the
 * bottom of its view, so its bottom edge is the screen's. */
#define KB_PAD    4
#define KB_GAP_PX 4
#define KB_KEY_R  12
#define KB_KEY_H  ((HY_KBD_H - 2 * KB_PAD - 3 * KB_GAP_PX) / 4)
/* Left edge of the bottom row's mode key: the padding, the 2 unit spacer (6 buttons, 5 gaps, 20 units) and a gap */
#define KB_OUTER_X (KB_PAD + 2 * (HY_SCREEN - 2 * KB_PAD - 5 * KB_GAP_PX) / 20 + KB_GAP_PX)
/* Centre of that key's rounded bottom-left corner, and of the glass corner circle */
#define KB_ARC_X  (KB_OUTER_X + KB_KEY_R)
#define KB_ARC_Y  (HY_SCREEN - KB_PAD - KB_KEY_R)
#define KB_GLASS  (HY_SCREEN - HY_CORNER_R)
_Static_assert(KB_KEY_H >= 56, "keyboard keys under 56 px");
_Static_assert(KB_ARC_X >= HY_CORNER_R || KB_ARC_Y <= KB_GLASS ||
                   (HY_CORNER_R - KB_ARC_X) * (HY_CORNER_R - KB_ARC_X) + (KB_ARC_Y - KB_GLASS) * (KB_ARC_Y - KB_GLASS) <=
                       (HY_CORNER_R - KB_KEY_R) * (HY_CORNER_R - KB_KEY_R),
               "the outer bottom keys reach into the rounded corner");

/* LVGL keeps the maps per mode in process-wide tables: lv_keyboard_set_map replaces them for every keyboard, existing
 * and future. All keyboards here are created with the same three maps, so they agree; a keyboard that needs a
 * different map would change the others. */
static const char *const kb_map_letters[] = {
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    " ", "a", "s", "d", "f", "g", "h", "j", "k", "l", " ", "\n",
    KB_SHIFT, "z", "x", "c", "v", "b", "n", "m", " ", "\n",
    " ", KB_MODE_SYMBOLS, " ", LV_SYMBOL_BACKSPACE, LV_SYMBOL_OK, " ", ""};
static const lv_buttonmatrix_ctrl_t kb_ctrl_letters[] = {
    KB_W2, KB_W2, KB_W2, KB_W2, KB_W2, KB_W2, KB_W2, KB_W2, KB_W2, KB_W2,
    KB_GAP(1), KB_W2, KB_W2, KB_W2, KB_W2, KB_W2, KB_W2, KB_W2, KB_W2, KB_W2, KB_GAP(1),
    KB_SHIFT_UP, KB_W2, KB_W2, KB_W2, KB_W2, KB_W2, KB_W2, KB_W2, KB_GAP(3),
    KB_BOTTOM};
static const char *const kb_map_upper[] = {
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    " ", "A", "S", "D", "F", "G", "H", "J", "K", "L", " ", "\n",
    KB_SHIFT, "Z", "X", "C", "V", "B", "N", "M", " ", "\n",
    " ", KB_MODE_SYMBOLS, " ", LV_SYMBOL_BACKSPACE, LV_SYMBOL_OK, " ", ""};
static const lv_buttonmatrix_ctrl_t kb_ctrl_upper[] = {
    KB_W2, KB_W2, KB_W2, KB_W2, KB_W2, KB_W2, KB_W2, KB_W2, KB_W2, KB_W2,
    KB_GAP(1), KB_W2, KB_W2, KB_W2, KB_W2, KB_W2, KB_W2, KB_W2, KB_W2, KB_W2, KB_GAP(1),
    KB_SHIFT_ARMED, KB_W2, KB_W2, KB_W2, KB_W2, KB_W2, KB_W2, KB_W2, KB_GAP(3),
    KB_BOTTOM};
static const char *const kb_map_symbols[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "\n",
    "0", ".", ",", "-", ":", ";", "/", "(", ")", "\n",
    "!", "?", "+", "=", "%", "@", "&", "'", "\n",
    " ", KB_MODE_LETTERS, " ", LV_SYMBOL_BACKSPACE, LV_SYMBOL_OK, " ", ""};
static const lv_buttonmatrix_ctrl_t kb_ctrl_symbols[] = {
    KB_NINE,
    KB_NINE,
    KB_KEY(1), KB_KEY(1), KB_KEY(1), KB_KEY(1), KB_KEY(1), KB_KEY(1), KB_KEY(1), KB_KEY(1),
    KB_BOTTOM};

static lv_font_t s_kb_font;   /* Inter 26 bold with Montserrat as the fallback for the keyboard's symbol keys */
static bool s_font_ready;

/* Per keyboard: the text area it types into and its text before the current press */
typedef struct {
    lv_obj_t *ta;
    char *prev;
    uint32_t shift_idx;   /* button index of the shift key, found by its text at create; same in both letter maps */
} kbd_t;

static kbd_t *kbd_of(lv_obj_t *kb)
{
    return (kbd_t *)lv_obj_get_user_data(kb);
}

/* The keyboard's enter and OK keys must not auto-repeat: a held key would send READY again and the repeat
 * counts as the confirming second request of the R29 too-soon warning. Every mode switch reloads the stock control
 * map, so the flag is set again on each press. One press, one READY. */
static void kb_no_repeat(lv_obj_t *kb)
{
    for (uint32_t i = 0; i < 64; i++) {
        const char *t = lv_buttonmatrix_get_button_text(kb, i);
        if (!t) {
            break;
        }
        if (strcmp(t, LV_SYMBOL_NEW_LINE) == 0 || strcmp(t, LV_SYMBOL_OK) == 0) {
            lv_buttonmatrix_set_button_ctrl(kb, i, LV_BUTTONMATRIX_CTRL_NO_REPEAT);
        }
    }
}

static void kb_set_mode(lv_obj_t *kb, lv_keyboard_mode_t mode)
{
    lv_keyboard_set_mode(kb, mode);
    kb_no_repeat(kb);
}

static void consume_cb(lv_event_t *e)
{
    (void)e;
    ui_input_consume();   /* a key press is the widget's stroke */
}

/* The text before the press, so a stroke that only woke the screen can be taken back after the keyboard typed it */
static void kb_press_cb(lv_event_t *e)
{
    lv_obj_t *kb = lv_event_get_current_target_obj(e);
    kbd_t *k = kbd_of(kb);
    kb_no_repeat(kb);
    if (k && k->ta) {
        lv_free(k->prev);
        k->prev = lv_strdup(lv_textarea_get_text(k->ta));
    }
    consume_cb(e);
}

/* Replaces the stock handler: the shift key is handled here, every other key goes to the stock handler and a typed
 * letter then drops shift. OK and close keys send READY or CANCEL, whose handlers may delete the keyboard and its
 * view: nothing touches the keyboard or its user data after the stock handler for those keys. */
static void kb_value_cb(lv_event_t *e)
{
    lv_obj_t *kb = lv_event_get_current_target_obj(e);
    kbd_t *k = kbd_of(kb);
    uint32_t id = lv_buttonmatrix_get_selected_button(kb);
    const char *txt = id == LV_BUTTONMATRIX_BUTTON_NONE ? NULL : lv_buttonmatrix_get_button_text(kb, id);
    bool waking = ui_input_waking();
    bool armed = lv_keyboard_get_mode(kb) == LV_KEYBOARD_MODE_TEXT_UPPER;

    if (txt && strcmp(txt, KB_SHIFT) == 0) {
        if (!waking) {   /* a stroke that only woke the screen does nothing else */
            kb_set_mode(kb, armed ? LV_KEYBOARD_MODE_TEXT_LOWER : LV_KEYBOARD_MODE_TEXT_UPPER);
        }
        return;
    }
    bool letter = txt && txt[0] && !txt[1] && ((txt[0] >= 'A' && txt[0] <= 'Z') || (txt[0] >= 'a' && txt[0] <= 'z'));
    bool leaves = txt && (strcmp(txt, LV_SYMBOL_OK) == 0 || strcmp(txt, LV_SYMBOL_NEW_LINE) == 0 ||
                          strcmp(txt, LV_SYMBOL_CLOSE) == 0 || strcmp(txt, LV_SYMBOL_KEYBOARD) == 0);
    lv_keyboard_def_event_cb(e);
    if (leaves) {
        return;   /* these keys type nothing and need no follow-up; the object may be gone */
    }
    if (waking) {
        if (k && k->ta && k->prev) {
            lv_textarea_set_text(k->ta, k->prev);
        }
        return;
    }
    if (armed && letter) {
        kb_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    } else {
        kb_no_repeat(kb);   /* a mode key reloaded the control map */
    }
}

/* Armed shift: text-colour fill and a black arrow, so it reads differently from the always-filled mode and space keys */
static void kb_draw_cb(lv_event_t *e)
{
    lv_obj_t *kb = lv_event_get_current_target_obj(e);
    if (lv_keyboard_get_mode(kb) != LV_KEYBOARD_MODE_TEXT_UPPER) {
        return;
    }
    lv_draw_task_t *t = lv_event_get_draw_task(e);
    lv_draw_dsc_base_t *base = (lv_draw_dsc_base_t *)lv_draw_task_get_draw_dsc(t);
    kbd_t *k = kbd_of(kb);
    if (!k || !base || base->part != LV_PART_ITEMS || base->id1 != k->shift_idx) {
        return;
    }
    if (lv_draw_task_get_type(t) == LV_DRAW_TASK_TYPE_FILL) {
        lv_draw_task_get_fill_dsc(t)->color = lv_color_hex(HY_C_TEXT);
    } else if (lv_draw_task_get_type(t) == LV_DRAW_TASK_TYPE_LABEL) {
        lv_draw_task_get_label_dsc(t)->color = lv_color_hex(HY_C_BG);
    }
}

static void kb_delete_cb(lv_event_t *e)
{
    kbd_t *k = kbd_of(lv_event_get_current_target_obj(e));
    if (k) {
        lv_free(k->prev);
        lv_free(k);
    }
}

lv_obj_t *ui_kbd_create(lv_obj_t *parent, lv_obj_t *ta)
{
    if (!s_font_ready) {   /* one font for every keyboard */
        s_kb_font = *hy_type.title;
        s_kb_font.fallback = &lv_font_montserrat_14;
        s_font_ready = true;
    }
    lv_obj_t *kb = lv_keyboard_create(parent);
    kbd_t *k = lv_malloc_zeroed(sizeof(kbd_t));
    if (!k) {
        ESP_LOGE(TAG, "out of memory");
        return kb;
    }
    k->ta = ta;
    lv_obj_set_user_data(kb, k);
    lv_obj_set_size(kb, HY_SCREEN, HY_KBD_H);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_LOWER, kb_map_letters, kb_ctrl_letters);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_UPPER, kb_map_upper, kb_ctrl_upper);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_SPECIAL, kb_map_symbols, kb_ctrl_symbols);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(kb, ta);
    lv_obj_set_style_bg_color(kb, lv_color_hex(HY_C_BG), 0);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(kb, 0, 0);
    lv_obj_set_style_radius(kb, 0, 0);
    lv_obj_set_style_pad_all(kb, KB_PAD, 0);
    lv_obj_set_style_pad_gap(kb, KB_GAP_PX, 0);
    lv_obj_set_style_text_font(kb, &s_kb_font, LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, lv_color_hex(HY_C_TEXT), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, lv_color_hex(HY_C_SURFACE2), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_radius(kb, KB_KEY_R, LV_PART_ITEMS);
    lv_obj_set_style_border_width(kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, lv_color_hex(HY_C_FILL), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(kb, lv_color_hex(HY_C_FILL), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(kb, lv_color_hex(HY_C_TEXT), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_remove_event_cb(kb, lv_keyboard_def_event_cb);   /* kb_value_cb calls it for every key but shift */
    lv_obj_add_event_cb(kb, kb_press_cb, LV_EVENT_PRESSED | LV_EVENT_PREPROCESS, NULL);   /* before the key's own handler */
    lv_obj_add_event_cb(kb, consume_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(kb, kb_value_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_set_send_draw_task_events(kb, true);
    lv_obj_add_event_cb(kb, kb_draw_cb, LV_EVENT_DRAW_TASK_ADDED, NULL);
    lv_obj_add_event_cb(kb, kb_delete_cb, LV_EVENT_DELETE, NULL);
    kb_no_repeat(kb);
    k->shift_idx = LV_BUTTONMATRIX_BUTTON_NONE;   /* never matches a drawn button if the text is missing */
    for (uint32_t i = 0; i < 64; i++) {
        const char *t = lv_buttonmatrix_get_button_text(kb, i);
        if (!t) {
            break;
        }
        if (strcmp(t, KB_SHIFT) == 0) {
            k->shift_idx = i;
            break;
        }
    }
    return kb;
}
