/**
 * @file lv_ime_pinyin.c
 *
 */

/*********************
 *      INCLUDES
 *********************/
#include "llm_lv_ime_pinyin.h"
#if LV_USE_IME_PINYIN != 0

#include <stdio.h>
#include <string.h>

/*********************
 *      DEFINES
 *********************/
#define MY_CLASS    &lv_ime_pinyin_class
#define LV_IME_CAND_BUF_MAX   16
#define LV_IME_CAND_POOL_MAX  96

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/
static void lv_ime_pinyin_constructor(const lv_obj_class_t * class_p, lv_obj_t * obj);
static void lv_ime_pinyin_destructor(const lv_obj_class_t * class_p, lv_obj_t * obj);
static void lv_ime_pinyin_style_change_event(lv_event_t * e);
static void lv_ime_pinyin_kb_event(lv_event_t * e);
static void lv_ime_pinyin_cand_panel_event(lv_event_t * e);

static void init_pinyin_dict(lv_obj_t * obj, lv_pinyin_dict_t * dict);
static void pinyin_input_proc(lv_obj_t * obj);
static void pinyin_page_proc(lv_obj_t * obj, uint16_t btn);
static void pinyin_build_candidates(lv_obj_t * obj);
static void pinyin_cand_fill_page(lv_obj_t * obj);
static void pinyin_ime_clear_data(lv_obj_t * obj);
static int pinyin_utf8_chars(const char * s);
static int pinyin_exact_syllable(lv_obj_t * obj, const char * py, const char ** mb_out);
static int pinyin_prefix_syllable(lv_obj_t * obj, const char * py, const char ** mb_out);
static int pinyin_longest_syllable(lv_obj_t * obj, const char * py);
static int pinyin_phrase_py_len(const char * word);
static void cand_pool_add(const char * word);
static void cand_pool_add_chars(const char * mb);

#if LV_IME_PINYIN_USE_K9_MODE
    static void pinyin_k9_init_data(lv_obj_t * obj);
    static void pinyin_k9_get_legal_py(lv_obj_t * obj, char * k9_input, const char * py9_map[]);
    static bool pinyin_k9_is_valid_py(lv_obj_t * obj, char * py_str);
    static void pinyin_k9_fill_cand(lv_obj_t * obj);
    static void pinyin_k9_cand_page_proc(lv_obj_t * obj, uint16_t dir);
#endif

/**********************
 *  STATIC VARIABLES
 **********************/
const lv_obj_class_t lv_ime_pinyin_class = {
    .constructor_cb = lv_ime_pinyin_constructor,
    .destructor_cb  = lv_ime_pinyin_destructor,
    .width_def      = LV_SIZE_CONTENT,
    .height_def     = LV_SIZE_CONTENT,
    .group_def      = LV_OBJ_CLASS_GROUP_DEF_TRUE,
    .instance_size  = sizeof(lv_ime_pinyin_t),
    .base_class     = &lv_obj_class
};

#if LV_IME_PINYIN_USE_K9_MODE
static char * lv_btnm_def_pinyin_k9_map[LV_IME_PINYIN_K9_CAND_TEXT_NUM + 20] = {\
                                                                                ",\0", "1#\0",  "abc \0", "def\0",  LV_SYMBOL_BACKSPACE"\0", "\n\0",
                                                                                ".\0", "ghi\0", "jkl\0", "mno\0",  LV_SYMBOL_KEYBOARD"\0", "\n\0",
                                                                                "?\0", "pqrs\0", "tuv\0", "wxyz\0",  LV_SYMBOL_NEW_LINE"\0", "\n\0",
                                                                                LV_SYMBOL_LEFT"\0", "\0"
                                                                               };

static lv_btnmatrix_ctrl_t default_kb_ctrl_k9_map[LV_IME_PINYIN_K9_CAND_TEXT_NUM + 16] = { 1 };
static char   lv_pinyin_k9_cand_str[LV_IME_PINYIN_K9_CAND_TEXT_NUM + 2][LV_IME_PINYIN_K9_MAX_INPUT] = {0};
#endif

static char   lv_pinyin_cand_str[LV_IME_PINYIN_CAND_TEXT_NUM][LV_IME_CAND_BUF_MAX];
static char * lv_btnm_def_pinyin_sel_map[LV_IME_PINYIN_CAND_TEXT_NUM + 3];
static char   lv_ime_cand_pool[LV_IME_CAND_POOL_MAX][LV_IME_CAND_BUF_MAX];
static uint16_t lv_ime_cand_pool_n;

#if LV_IME_PINYIN_USE_DEFAULT_DICT
/* 简体字库 + 词组 */
#include "pinyin_dict_sc.inc"
#include "pinyin_phrase_sc.inc"
#endif


/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/
lv_obj_t * lv_ime_pinyin_create(lv_obj_t * parent)
{
    LV_LOG_INFO("begin");
    lv_obj_t * obj = lv_obj_class_create_obj(MY_CLASS, parent);
    lv_obj_class_init_obj(obj);
    return obj;
}


/*=====================
 * Setter functions
 *====================*/

/**
 * Set the keyboard of Pinyin input method.
 * @param obj  pointer to a Pinyin input method object
 * @param dict pointer to a Pinyin input method keyboard
 */
void lv_ime_pinyin_set_keyboard(lv_obj_t * obj, lv_obj_t * kb)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    lv_ime_pinyin_t * pinyin_ime = (lv_ime_pinyin_t *)obj;

    if(pinyin_ime->kb && lv_obj_is_valid(pinyin_ime->kb))
        lv_obj_remove_event_cb(pinyin_ime->kb, lv_ime_pinyin_kb_event);

    pinyin_ime->kb = kb;
    if(kb == NULL) return;

    LV_ASSERT_OBJ(kb, &lv_keyboard_class);
    lv_obj_add_event_cb(pinyin_ime->kb, lv_ime_pinyin_kb_event, LV_EVENT_VALUE_CHANGED, obj);
    if(pinyin_ime->cand_panel)
        lv_obj_align_to(pinyin_ime->cand_panel, pinyin_ime->kb, LV_ALIGN_OUT_TOP_MID, 0, 0);
}

/**
 * Set the dictionary of Pinyin input method.
 * @param obj  pointer to a Pinyin input method object
 * @param dict pointer to a Pinyin input method dictionary
 */
void lv_ime_pinyin_set_dict(lv_obj_t * obj, lv_pinyin_dict_t * dict)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);

    init_pinyin_dict(obj, dict);
}

/**
 * Set mode, 26-key input(k26) or 9-key input(k9).
 * @param obj  pointer to a Pinyin input method object
 * @param mode   the mode from 'lv_keyboard_mode_t'
 */
void lv_ime_pinyin_set_mode(lv_obj_t * obj, lv_ime_pinyin_mode_t mode)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    lv_ime_pinyin_t * pinyin_ime = (lv_ime_pinyin_t *)obj;

    LV_ASSERT_OBJ(pinyin_ime->kb, &lv_keyboard_class);

    pinyin_ime->mode = mode;

#if LV_IME_PINYIN_USE_K9_MODE
    if(pinyin_ime->mode == LV_IME_PINYIN_MODE_K9) {
        pinyin_k9_init_data(obj);
        lv_keyboard_set_map(pinyin_ime->kb, LV_KEYBOARD_MODE_USER_1, (const char *)lv_btnm_def_pinyin_k9_map,
                            (const)default_kb_ctrl_k9_map);
        lv_keyboard_set_mode(pinyin_ime->kb, LV_KEYBOARD_MODE_USER_1);
    }
#endif
}

/*=====================
 * Getter functions
 *====================*/

/**
 * Set the dictionary of Pinyin input method.
 * @param obj  pointer to a Pinyin IME object
 * @return     pointer to the Pinyin IME keyboard
 */
lv_obj_t * lv_ime_pinyin_get_kb(lv_obj_t * obj)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);

    lv_ime_pinyin_t * pinyin_ime = (lv_ime_pinyin_t *)obj;

    return pinyin_ime->kb;
}

/**
 * Set the dictionary of Pinyin input method.
 * @param obj  pointer to a Pinyin input method object
 * @return     pointer to the Pinyin input method candidate panel
 */
lv_obj_t * lv_ime_pinyin_get_cand_panel(lv_obj_t * obj)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);

    lv_ime_pinyin_t * pinyin_ime = (lv_ime_pinyin_t *)obj;

    return pinyin_ime->cand_panel;
}

/**
 * Set the dictionary of Pinyin input method.
 * @param obj  pointer to a Pinyin input method object
 * @return     pointer to the Pinyin input method dictionary
 */
lv_pinyin_dict_t * lv_ime_pinyin_get_dict(lv_obj_t * obj)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);

    lv_ime_pinyin_t * pinyin_ime = (lv_ime_pinyin_t *)obj;

    return pinyin_ime->dict;
}

/*=====================
 * Other functions
 *====================*/

/**********************
 *   STATIC FUNCTIONS
 **********************/

static void lv_ime_pinyin_constructor(const lv_obj_class_t * class_p, lv_obj_t * obj)
{
    LV_UNUSED(class_p);
    LV_TRACE_OBJ_CREATE("begin");

    lv_ime_pinyin_t * pinyin_ime = (lv_ime_pinyin_t *)obj;

    uint16_t py_str_i = 0;
    uint16_t btnm_i = 0;
    for(btnm_i = 0; btnm_i < (LV_IME_PINYIN_CAND_TEXT_NUM + 3); btnm_i++) {
        if(btnm_i == 0) {
            lv_btnm_def_pinyin_sel_map[btnm_i] = "<";
        }
        else if(btnm_i == (LV_IME_PINYIN_CAND_TEXT_NUM + 1)) {
            lv_btnm_def_pinyin_sel_map[btnm_i] = ">";
        }
        else if(btnm_i == (LV_IME_PINYIN_CAND_TEXT_NUM + 2)) {
            lv_btnm_def_pinyin_sel_map[btnm_i] = "";
        }
        else {
            lv_pinyin_cand_str[py_str_i][0] = ' ';
            lv_btnm_def_pinyin_sel_map[btnm_i] = lv_pinyin_cand_str[py_str_i];
            py_str_i++;
        }
    }

    pinyin_ime->mode = LV_IME_PINYIN_MODE_K26;
    pinyin_ime->py_page = 0;
    pinyin_ime->ta_count = 0;
    pinyin_ime->cand_num = 0;
    lv_memset_00(pinyin_ime->input_char, sizeof(pinyin_ime->input_char));
    lv_memset_00(pinyin_ime->py_num, sizeof(pinyin_ime->py_num));
    lv_memset_00(pinyin_ime->py_pos, sizeof(pinyin_ime->py_pos));

    /* llm_panel：勿铺底 55% 可点层，否则挡住软键盘下排 */
    lv_obj_set_size(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);

#if LV_IME_PINYIN_USE_DEFAULT_DICT
    init_pinyin_dict(obj, lv_ime_pinyin_def_dict);
#endif

    /* Init pinyin_ime->cand_panel */
    pinyin_ime->cand_panel = lv_btnmatrix_create(lv_scr_act());
    lv_btnmatrix_set_map(pinyin_ime->cand_panel, (const char **)lv_btnm_def_pinyin_sel_map);
    lv_obj_set_size(pinyin_ime->cand_panel, LV_PCT(100), LV_PCT(5));
    lv_obj_add_flag(pinyin_ime->cand_panel, LV_OBJ_FLAG_HIDDEN);

    lv_btnmatrix_set_one_checked(pinyin_ime->cand_panel, true);

    /* Set cand_panel style*/
    // Default style
    lv_obj_set_style_bg_opa(pinyin_ime->cand_panel, LV_OPA_0, 0);
    lv_obj_set_style_border_width(pinyin_ime->cand_panel, 0, 0);
    lv_obj_set_style_pad_all(pinyin_ime->cand_panel, 8, 0);
    lv_obj_set_style_pad_gap(pinyin_ime->cand_panel, 0, 0);
    lv_obj_set_style_radius(pinyin_ime->cand_panel, 0, 0);
    lv_obj_set_style_pad_gap(pinyin_ime->cand_panel, 0, 0);
    lv_obj_set_style_base_dir(pinyin_ime->cand_panel, LV_BASE_DIR_LTR, 0);

    // LV_PART_ITEMS style（须不透明 + 有字色，否则候选像「没出来」）
    lv_obj_set_style_radius(pinyin_ime->cand_panel, 12, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(pinyin_ime->cand_panel, lv_color_hex(0x243040), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(pinyin_ime->cand_panel, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_text_color(pinyin_ime->cand_panel, lv_color_hex(0xe6edf3), LV_PART_ITEMS);
    lv_obj_set_style_shadow_opa(pinyin_ime->cand_panel, LV_OPA_0, LV_PART_ITEMS);

    // LV_PART_ITEMS | LV_STATE_PRESSED style
    lv_obj_set_style_bg_opa(pinyin_ime->cand_panel, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(pinyin_ime->cand_panel, lv_color_white(), LV_PART_ITEMS | LV_STATE_PRESSED);

    /* event handler */
    lv_obj_add_event_cb(pinyin_ime->cand_panel, lv_ime_pinyin_cand_panel_event, LV_EVENT_VALUE_CHANGED, obj);
    lv_obj_add_event_cb(obj, lv_ime_pinyin_style_change_event, LV_EVENT_STYLE_CHANGED, NULL);

#if LV_IME_PINYIN_USE_K9_MODE
    pinyin_ime->k9_input_str_len = 0;
    pinyin_ime->k9_py_ll_pos = 0;
    pinyin_ime->k9_legal_py_count = 0;
    lv_memset_00(pinyin_ime->k9_input_str, LV_IME_PINYIN_K9_MAX_INPUT);

    pinyin_k9_init_data(obj);

    _lv_ll_init(&(pinyin_ime->k9_legal_py_ll), sizeof(ime_pinyin_k9_py_str_t));
#endif
}


static void lv_ime_pinyin_destructor(const lv_obj_class_t * class_p, lv_obj_t * obj)
{
    LV_UNUSED(class_p);

    lv_ime_pinyin_t * pinyin_ime = (lv_ime_pinyin_t *)obj;

    if(lv_obj_is_valid(pinyin_ime->kb))
        lv_obj_del(pinyin_ime->kb);

    if(lv_obj_is_valid(pinyin_ime->cand_panel))
        lv_obj_del(pinyin_ime->cand_panel);
}


static void lv_ime_pinyin_kb_event(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * kb = lv_event_get_target(e);
    lv_obj_t * obj = lv_event_get_user_data(e);

    lv_ime_pinyin_t * pinyin_ime = (lv_ime_pinyin_t *)obj;

#if LV_IME_PINYIN_USE_K9_MODE
    static const char * k9_py_map[8] = {"abc", "def", "ghi", "jkl", "mno", "pqrs", "tuv", "wxyz"};
#endif

    if(code == LV_EVENT_VALUE_CHANGED) {
        uint16_t btn_id  = lv_btnmatrix_get_selected_btn(kb);
        if(btn_id == LV_BTNMATRIX_BTN_NONE) return;

        const char * txt = lv_btnmatrix_get_btn_text(kb, lv_btnmatrix_get_selected_btn(kb));
        if(txt == NULL) return;

#if LV_IME_PINYIN_USE_K9_MODE
        if(pinyin_ime->mode == LV_IME_PINYIN_MODE_K9) {
            lv_obj_t * ta = lv_keyboard_get_textarea(pinyin_ime->kb);
            uint16_t tmp_btn_str_len = strlen(pinyin_ime->input_char);
            if((btn_id >= 16) && (tmp_btn_str_len > 0) && (btn_id < (16 + LV_IME_PINYIN_K9_CAND_TEXT_NUM))) {
                tmp_btn_str_len = strlen(pinyin_ime->input_char);
                lv_memset_00(pinyin_ime->input_char, sizeof(pinyin_ime->input_char));
                strcat(pinyin_ime->input_char, txt);
                pinyin_input_proc(obj);

                for(int index = 0; index < (pinyin_ime->ta_count + tmp_btn_str_len); index++) {
                    lv_textarea_del_char(ta);
                }

                pinyin_ime->ta_count = tmp_btn_str_len;
                pinyin_ime->k9_input_str_len = tmp_btn_str_len;
                lv_textarea_add_text(ta, pinyin_ime->input_char);

                return;
            }
        }
#endif

        if(strcmp(txt, "Enter") == 0 || strcmp(txt, LV_SYMBOL_NEW_LINE) == 0) {
            pinyin_ime_clear_data(obj);
            lv_obj_add_flag(pinyin_ime->cand_panel, LV_OBJ_FLAG_HIDDEN);
        }
        else if(strcmp(txt, LV_SYMBOL_BACKSPACE) == 0) {
            // del input char
            if(pinyin_ime->ta_count > 0) {
                if(pinyin_ime->mode == LV_IME_PINYIN_MODE_K26)
                    pinyin_ime->input_char[pinyin_ime->ta_count - 1] = '\0';
#if LV_IME_PINYIN_USE_K9_MODE
                else
                    pinyin_ime->k9_input_str[pinyin_ime->ta_count - 1] = '\0';
#endif

                pinyin_ime->ta_count = pinyin_ime->ta_count - 1;
                if(pinyin_ime->ta_count <= 0) {
                    lv_obj_add_flag(pinyin_ime->cand_panel, LV_OBJ_FLAG_HIDDEN);
#if LV_IME_PINYIN_USE_K9_MODE
                    lv_memset_00(lv_pinyin_k9_cand_str, sizeof(lv_pinyin_k9_cand_str));
                    strcpy(lv_pinyin_k9_cand_str[LV_IME_PINYIN_K9_CAND_TEXT_NUM], LV_SYMBOL_RIGHT"\0");
                    strcpy(lv_pinyin_k9_cand_str[LV_IME_PINYIN_K9_CAND_TEXT_NUM + 1], "\0");
#endif
                }
                else if(pinyin_ime->mode == LV_IME_PINYIN_MODE_K26) {
                    pinyin_input_proc(obj);
                }
#if LV_IME_PINYIN_USE_K9_MODE
                else if(pinyin_ime->mode == LV_IME_PINYIN_MODE_K9) {
                    pinyin_ime->k9_input_str_len = strlen(pinyin_ime->input_char) - 1;
                    pinyin_k9_get_legal_py(obj, pinyin_ime->k9_input_str, k9_py_map);
                    pinyin_k9_fill_cand(obj);
                    pinyin_input_proc(obj);
                }
#endif
            }
        }
        else if((strcmp(txt, "ABC") == 0) || (strcmp(txt, "abc") == 0) || (strcmp(txt, "1#") == 0)) {
            pinyin_ime->ta_count = 0;
            lv_memset_00(pinyin_ime->input_char, sizeof(pinyin_ime->input_char));
            return;
        }
        else if(strcmp(txt, LV_SYMBOL_KEYBOARD) == 0) {
            /* 拼/英由 ui 层处理；此处只清拼音，并停掉默认 CANCEL */
            pinyin_ime_clear_data(obj);
            lv_event_stop_processing(e);
        }
        else if(strcmp(txt, LV_SYMBOL_OK) == 0) {
            pinyin_ime_clear_data(obj);
        }
        else if((pinyin_ime->mode == LV_IME_PINYIN_MODE_K26) && ((txt[0] >= 'a' && txt[0] <= 'z') || (txt[0] >= 'A' &&
                                                                                                      txt[0] <= 'Z'))) {
            size_t n = strlen(pinyin_ime->input_char);
            char ch;
            if(n + 1 >= sizeof(pinyin_ime->input_char)) return;
            ch = (char)((txt[0] >= 'A' && txt[0] <= 'Z') ? (txt[0] + 32) : txt[0]);
            pinyin_ime->input_char[n] = ch;
            pinyin_ime->input_char[n + 1] = '\0';
            pinyin_ime->ta_count++;
            pinyin_input_proc(obj);
        }
#if LV_IME_PINYIN_USE_K9_MODE
        else if((pinyin_ime->mode == LV_IME_PINYIN_MODE_K9) && (txt[0] >= 'a' && txt[0] <= 'z')) {
            for(uint16_t i = 0; i < 8; i++) {
                if((strcmp(txt, k9_py_map[i]) == 0) || (strcmp(txt, "abc ") == 0)) {
                    if(strcmp(txt, "abc ") == 0)    pinyin_ime->k9_input_str_len += strlen(k9_py_map[i]) + 1;
                    else                            pinyin_ime->k9_input_str_len += strlen(k9_py_map[i]);
                    pinyin_ime->k9_input_str[pinyin_ime->ta_count] = 50 + i;

                    break;
                }
            }
            pinyin_k9_get_legal_py(obj, pinyin_ime->k9_input_str, k9_py_map);
            pinyin_k9_fill_cand(obj);
            pinyin_input_proc(obj);
        }
        else if(strcmp(txt, LV_SYMBOL_LEFT) == 0) {
            pinyin_k9_cand_page_proc(obj, 0);
        }
        else if(strcmp(txt, LV_SYMBOL_RIGHT) == 0) {
            pinyin_k9_cand_page_proc(obj, 1);
        }
#endif
    }
}


static void lv_ime_pinyin_cand_panel_event(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * cand_panel = lv_event_get_target(e);
    lv_obj_t * obj = (lv_obj_t *)lv_event_get_user_data(e);
    lv_ime_pinyin_t * pinyin_ime = (lv_ime_pinyin_t *)obj;

    if(code == LV_EVENT_VALUE_CHANGED) {
        uint32_t id = lv_btnmatrix_get_selected_btn(cand_panel);
        char rest[sizeof(pinyin_ime->input_char)];
        int consumed;
        uint16_t index;
        lv_obj_t * ta;
        const char * txt;

        if(id == 0) {
            pinyin_page_proc(obj, 0);
            return;
        }
        if(id == (LV_IME_PINYIN_CAND_TEXT_NUM + 1)) {
            pinyin_page_proc(obj, 1);
            return;
        }

        txt = lv_btnmatrix_get_btn_text(cand_panel, id);
        if(txt == NULL || txt[0] == '\0' || txt[0] == ' ') return;

        ta = lv_keyboard_get_textarea(pinyin_ime->kb);
        if(ta == NULL) return;

        for(index = 0; index < pinyin_ime->ta_count; index++)
            lv_textarea_del_char(ta);
        lv_textarea_add_text(ta, txt);

        /* 词组按词拼音长度消费；单字按首音节，剩余继续出候选（连打） */
        if(pinyin_utf8_chars(txt) >= 2) {
            consumed = pinyin_phrase_py_len(txt);
            if(consumed <= 0) consumed = (int)strlen(pinyin_ime->input_char);
        }
        else {
            consumed = pinyin_longest_syllable(obj, pinyin_ime->input_char);
            if(consumed <= 0) consumed = (int)strlen(pinyin_ime->input_char);
        }

        rest[0] = '\0';
        if(consumed > 0 && consumed < (int)strlen(pinyin_ime->input_char)) {
            strncpy(rest, pinyin_ime->input_char + consumed, sizeof(rest) - 1);
            rest[sizeof(rest) - 1] = '\0';
        }

        if(rest[0]) {
            lv_textarea_add_text(ta, rest);
            memset(pinyin_ime->input_char, 0, sizeof(pinyin_ime->input_char));
            strncpy(pinyin_ime->input_char, rest, sizeof(pinyin_ime->input_char) - 1);
            pinyin_ime->ta_count = (uint16_t)strlen(pinyin_ime->input_char);
            pinyin_input_proc(obj);
        }
        else {
            pinyin_ime_clear_data(obj);
        }
    }
}

static int pinyin_utf8_chars(const char * s)
{
    int n = 0;
    if(s == NULL) return 0;
    while(*s) {
        unsigned char c = (unsigned char)*s;
        if(c < 0x80) s += 1;
        else if((c & 0xE0) == 0xC0) s += 2;
        else if((c & 0xF0) == 0xE0) s += 3;
        else if((c & 0xF8) == 0xF0) s += 4;
        else s += 1;
        n++;
    }
    return n;
}

static void cand_pool_add(const char * word)
{
    uint16_t i;
    size_t n;
    if(word == NULL || word[0] == '\0') return;
    if(lv_ime_cand_pool_n >= LV_IME_CAND_POOL_MAX) return;
    n = strlen(word);
    if(n == 0 || n >= LV_IME_CAND_BUF_MAX) return;
    for(i = 0; i < lv_ime_cand_pool_n; i++) {
        if(strcmp(lv_ime_cand_pool[i], word) == 0) return;
    }
    memcpy(lv_ime_cand_pool[lv_ime_cand_pool_n], word, n);
    lv_ime_cand_pool[lv_ime_cand_pool_n][n] = '\0';
    lv_ime_cand_pool_n++;
}

static void cand_pool_add_chars(const char * mb)
{
    if(mb == NULL) return;
    while(mb[0]) {
        char one[8];
        unsigned char c = (unsigned char)mb[0];
        int step = 1;
        if((c & 0xF0) == 0xE0) step = 3;
        else if((c & 0xE0) == 0xC0) step = 2;
        else if((c & 0xF8) == 0xF0) step = 4;
        if(step > 7) break;
        memcpy(one, mb, (size_t)step);
        one[step] = '\0';
        cand_pool_add(one);
        mb += step;
    }
}

static int pinyin_exact_syllable(lv_obj_t * obj, const char * py, const char ** mb_out)
{
    lv_ime_pinyin_t * pinyin_ime = (lv_ime_pinyin_t *)obj;
    uint8_t offset, len, index;
    uint16_t count;
    lv_pinyin_dict_t * cpHZ;

    if(mb_out) *mb_out = NULL;
    if(py == NULL || py[0] == '\0') return 0;
    if(py[0] < 'a' || py[0] > 'z') return 0;

    offset = (uint8_t)(py[0] - 'a');
    len = (uint8_t)strlen(py);
    count = pinyin_ime->py_num[offset];
    if(count == 0) return 0;

    cpHZ = &pinyin_ime->dict[pinyin_ime->py_pos[offset]];
    while(count--) {
        if(strlen(cpHZ->py) == len) {
            for(index = 0; index < len; index++) {
                if(py[index] != cpHZ->py[index]) break;
            }
            if(index == len) {
                if(mb_out) *mb_out = cpHZ->py_mb;
                return 1;
            }
        }
        cpHZ++;
    }
    return 0;
}

/* 前缀命中：打 n/ni 过程中也能出字 */
static int pinyin_prefix_syllable(lv_obj_t * obj, const char * py, const char ** mb_out)
{
    lv_ime_pinyin_t * pinyin_ime = (lv_ime_pinyin_t *)obj;
    uint8_t offset, len, index;
    uint16_t count;
    lv_pinyin_dict_t * cpHZ;

    if(mb_out) *mb_out = NULL;
    if(py == NULL || py[0] == '\0') return 0;
    if(py[0] < 'a' || py[0] > 'z') return 0;

    offset = (uint8_t)(py[0] - 'a');
    len = (uint8_t)strlen(py);
    count = pinyin_ime->py_num[offset];
    if(count == 0) return 0;

    cpHZ = &pinyin_ime->dict[pinyin_ime->py_pos[offset]];
    while(count--) {
        for(index = 0; index < len; index++) {
            if(cpHZ->py[index] == '\0' || py[index] != cpHZ->py[index]) break;
        }
        if(index == len) {
            if(mb_out) *mb_out = cpHZ->py_mb;
            return 1;
        }
        cpHZ++;
    }
    return 0;
}

static int pinyin_longest_syllable(lv_obj_t * obj, const char * py)
{
    int n;
    if(py == NULL) return 0;
    n = (int)strlen(py);
    if(n > 6) n = 6;
    for(; n >= 1; n--) {
        char tmp[8];
        memcpy(tmp, py, (size_t)n);
        tmp[n] = '\0';
        if(pinyin_exact_syllable(obj, tmp, NULL)) return n;
    }
    if(pinyin_prefix_syllable(obj, py, NULL)) return (int)strlen(py);
    return 0;
}

static int pinyin_phrase_py_len(const char * word)
{
    const lv_pinyin_phrase_t * p;
    if(word == NULL) return -1;
    for(p = lv_ime_pinyin_phrases; p->py != NULL; p++) {
        if(strcmp(p->word, word) == 0) return (int)strlen(p->py);
    }
    return -1;
}

static void pinyin_build_candidates(lv_obj_t * obj)
{
    lv_ime_pinyin_t * pinyin_ime = (lv_ime_pinyin_t *)obj;
    const char * py = pinyin_ime->input_char;
    const char * mb = NULL;
    const lv_pinyin_phrase_t * p;
    size_t len;
    int syl;

    lv_ime_cand_pool_n = 0;
    if(py == NULL || py[0] == '\0') {
        pinyin_ime->cand_num = 0;
        return;
    }
    len = strlen(py);

    for(p = lv_ime_pinyin_phrases; p->py != NULL; p++) {
        size_t plen = strlen(p->py);
        int syl1;

        if(strcmp(p->py, py) == 0) {
            /* 整词拼完：nihao → 你好 */
            cand_pool_add(p->word);
        }
        else if(plen > len && strncmp(p->py, py, len) == 0) {
            /*
             * 未打完的词组：必须已经打进第二音节，
             * 避免 ni 就出「你好」（nihao 的首音节只有 ni）。
             */
            syl1 = pinyin_longest_syllable(obj, p->py);
            if(syl1 <= 0) syl1 = 1;
            if((int)len > syl1)
                cand_pool_add(p->word);
        }
        else if(len > plen && plen >= 2 && strncmp(py, p->py, plen) == 0) {
            /* 已超出该词拼音：仍可作为首词候选 */
            cand_pool_add(p->word);
        }
    }

    if(pinyin_exact_syllable(obj, py, &mb) && mb) {
        cand_pool_add_chars(mb);
    }
    else if(pinyin_prefix_syllable(obj, py, &mb) && mb) {
        cand_pool_add_chars(mb);
    }
    else {
        syl = pinyin_longest_syllable(obj, py);
        if(syl > 0 && syl < (int)len) {
            char first[8];
            memcpy(first, py, (size_t)syl);
            first[syl] = '\0';
            if(pinyin_exact_syllable(obj, first, &mb) && mb)
                cand_pool_add_chars(mb);
            else if(pinyin_prefix_syllable(obj, first, &mb) && mb)
                cand_pool_add_chars(mb);
        }
    }

    pinyin_ime->cand_num = lv_ime_cand_pool_n;
    pinyin_ime->cand_str = lv_ime_cand_pool_n ? lv_ime_cand_pool[0] : NULL;
}

static void pinyin_cand_fill_page(lv_obj_t * obj)
{
    lv_ime_pinyin_t * pinyin_ime = (lv_ime_pinyin_t *)obj;
    uint16_t page = pinyin_ime->py_page;
    uint16_t base = (uint16_t)(page * LV_IME_PINYIN_CAND_TEXT_NUM);
    uint8_t i;

    for(i = 0; i < LV_IME_PINYIN_CAND_TEXT_NUM; i++) {
        memset(lv_pinyin_cand_str[i], 0, sizeof(lv_pinyin_cand_str[i]));
        lv_pinyin_cand_str[i][0] = ' ';
        if(base + i < lv_ime_cand_pool_n) {
            strncpy(lv_pinyin_cand_str[i], lv_ime_cand_pool[base + i],
                    LV_IME_CAND_BUF_MAX - 1);
        }
    }

    lv_btnmatrix_set_map(pinyin_ime->cand_panel, (const char **)lv_btnm_def_pinyin_sel_map);
    if(pinyin_ime->kb)
        lv_obj_align_to(pinyin_ime->cand_panel, pinyin_ime->kb, LV_ALIGN_OUT_TOP_MID, 0, 0);
    lv_obj_clear_flag(pinyin_ime->cand_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(pinyin_ime->cand_panel);
    lv_obj_invalidate(pinyin_ime->cand_panel);
}

static void pinyin_input_proc(lv_obj_t * obj)
{
    lv_ime_pinyin_t * pinyin_ime = (lv_ime_pinyin_t *)obj;
    uint8_t i;

    pinyin_build_candidates(obj);
    if(lv_ime_cand_pool_n == 0) {
        for(i = 0; i < LV_IME_PINYIN_CAND_TEXT_NUM; i++) {
            memset(lv_pinyin_cand_str[i], 0, sizeof(lv_pinyin_cand_str[i]));
            lv_pinyin_cand_str[i][0] = ' ';
        }
        lv_btnmatrix_set_map(pinyin_ime->cand_panel, (const char **)lv_btnm_def_pinyin_sel_map);
        lv_obj_add_flag(pinyin_ime->cand_panel, LV_OBJ_FLAG_HIDDEN);
        fprintf(stderr, "rkllm_ime: no cand py='%s'\n", pinyin_ime->input_char);
        return;
    }

    pinyin_ime->py_page = 0;
    pinyin_cand_fill_page(obj);
    fprintf(stderr, "rkllm_ime: cand py='%s' n=%u\n",
            pinyin_ime->input_char, (unsigned)pinyin_ime->cand_num);
}

static void pinyin_page_proc(lv_obj_t * obj, uint16_t dir)
{
    lv_ime_pinyin_t * pinyin_ime = (lv_ime_pinyin_t *)obj;
    uint16_t page_num;
    uint16_t sur;

    if(lv_ime_cand_pool_n == 0) return;

    page_num = (uint16_t)(lv_ime_cand_pool_n / LV_IME_PINYIN_CAND_TEXT_NUM);
    sur = (uint16_t)(lv_ime_cand_pool_n % LV_IME_PINYIN_CAND_TEXT_NUM);
    if(sur == 0 && page_num > 0) page_num -= 1;

    if(dir == 0) {
        if(pinyin_ime->py_page) pinyin_ime->py_page--;
    }
    else {
        if(pinyin_ime->py_page < page_num) pinyin_ime->py_page++;
        else return;
    }
    pinyin_cand_fill_page(obj);
}

static void lv_ime_pinyin_style_change_event(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * obj = lv_event_get_target(e);

    lv_ime_pinyin_t * pinyin_ime = (lv_ime_pinyin_t *)obj;

    if(code == LV_EVENT_STYLE_CHANGED) {
        const lv_font_t * font = lv_obj_get_style_text_font(obj, LV_PART_MAIN);
        lv_obj_set_style_text_font(pinyin_ime->cand_panel, font, 0);
    }
}


static void init_pinyin_dict(lv_obj_t * obj, lv_pinyin_dict_t * dict)
{
    lv_ime_pinyin_t * pinyin_ime = (lv_ime_pinyin_t *)obj;
    uint16_t i;
    char prev = 0;
    uint16_t count = 0;

    pinyin_ime->dict = dict;
    lv_memset_00(pinyin_ime->py_num, sizeof(pinyin_ime->py_num));
    lv_memset_00(pinyin_ime->py_pos, sizeof(pinyin_ime->py_pos));

    /* 允许缺 i/u/v 等空洞，避免 h/t 的 py_num 被写成 0 */
    for(i = 0; dict[i].py != NULL && dict[i].py_mb != NULL; i++) {
        char h = dict[i].py[0];
        if(h < 'a' || h > 'z') continue;
        if(prev == 0) {
            prev = h;
            pinyin_ime->py_pos[h - 'a'] = i;
            count = 1;
        }
        else if(h == prev) {
            count++;
        }
        else {
            pinyin_ime->py_num[prev - 'a'] = count;
            prev = h;
            pinyin_ime->py_pos[h - 'a'] = i;
            count = 1;
        }
    }
    if(prev)
        pinyin_ime->py_num[prev - 'a'] = count;

    fprintf(stderr, "rkllm_ime: dict ready h=%u t=%u n=%u\n",
            (unsigned)pinyin_ime->py_num['h' - 'a'],
            (unsigned)pinyin_ime->py_num['t' - 'a'],
            (unsigned)pinyin_ime->py_num['n' - 'a']);
}

/* pinyin_search_matching removed: see pinyin_build_candidates */

static void pinyin_ime_clear_data(lv_obj_t * obj)
{
    lv_ime_pinyin_t * pinyin_ime = (lv_ime_pinyin_t *)obj;
    uint8_t i;

#if LV_IME_PINYIN_USE_K9_MODE
    if(pinyin_ime->mode == LV_IME_PINYIN_MODE_K9) {
        pinyin_ime->k9_input_str_len = 0;
        pinyin_ime->k9_py_ll_pos = 0;
        pinyin_ime->k9_legal_py_count = 0;
        lv_memset_00(pinyin_ime->k9_input_str,  LV_IME_PINYIN_K9_MAX_INPUT);
        lv_memset_00(lv_pinyin_k9_cand_str, sizeof(lv_pinyin_k9_cand_str));
        strcpy(lv_pinyin_k9_cand_str[LV_IME_PINYIN_K9_CAND_TEXT_NUM], LV_SYMBOL_RIGHT"\0");
        strcpy(lv_pinyin_k9_cand_str[LV_IME_PINYIN_K9_CAND_TEXT_NUM + 1], "\0");
    }
#endif

    pinyin_ime->ta_count = 0;
    pinyin_ime->cand_num = 0;
    pinyin_ime->py_page = 0;
    pinyin_ime->cand_str = NULL;
    lv_ime_cand_pool_n = 0;
    lv_memset_00(pinyin_ime->input_char, sizeof(pinyin_ime->input_char));

    /* 不可清成 ""：LVGL btnmatrix 空串表示换行，会弄坏 map → 选字后再打无候选 */
    for(i = 0; i < LV_IME_PINYIN_CAND_TEXT_NUM; i++) {
        memset(lv_pinyin_cand_str[i], 0, sizeof(lv_pinyin_cand_str[i]));
        lv_pinyin_cand_str[i][0] = ' ';
    }
    if(pinyin_ime->cand_panel) {
        lv_btnmatrix_set_map(pinyin_ime->cand_panel, (const char **)lv_btnm_def_pinyin_sel_map);
        lv_obj_add_flag(pinyin_ime->cand_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

#if LV_IME_PINYIN_USE_K9_MODE
static void pinyin_k9_init_data(lv_obj_t * obj)
{
    lv_ime_pinyin_t * pinyin_ime = (lv_ime_pinyin_t *)obj;

    uint16_t py_str_i = 0;
    uint16_t btnm_i = 0;
    for(btnm_i = 19; btnm_i < (LV_IME_PINYIN_K9_CAND_TEXT_NUM + 21); btnm_i++) {
        if(py_str_i == LV_IME_PINYIN_K9_CAND_TEXT_NUM) {
            strcpy(lv_pinyin_k9_cand_str[py_str_i], LV_SYMBOL_RIGHT"\0");
        }
        else if(py_str_i == LV_IME_PINYIN_K9_CAND_TEXT_NUM + 1) {
            strcpy(lv_pinyin_k9_cand_str[py_str_i], "\0");
        }
        else {
            strcpy(lv_pinyin_k9_cand_str[py_str_i], " \0");
        }

        lv_btnm_def_pinyin_k9_map[btnm_i] = lv_pinyin_k9_cand_str[py_str_i];
        py_str_i++;
    }

    default_kb_ctrl_k9_map[0]  = LV_KEYBOARD_CTRL_BTN_FLAGS | 1;
    default_kb_ctrl_k9_map[4]  = LV_KEYBOARD_CTRL_BTN_FLAGS | 1;
    default_kb_ctrl_k9_map[5]  = LV_KEYBOARD_CTRL_BTN_FLAGS | 1;
    default_kb_ctrl_k9_map[9]  = LV_KEYBOARD_CTRL_BTN_FLAGS | 1;
    default_kb_ctrl_k9_map[10] = LV_KEYBOARD_CTRL_BTN_FLAGS | 1;
    default_kb_ctrl_k9_map[14] = LV_KEYBOARD_CTRL_BTN_FLAGS | 1;
    default_kb_ctrl_k9_map[15] = LV_KEYBOARD_CTRL_BTN_FLAGS | 1;
    default_kb_ctrl_k9_map[LV_IME_PINYIN_K9_CAND_TEXT_NUM + 16] = LV_KEYBOARD_CTRL_BTN_FLAGS | 1;
}

static void pinyin_k9_get_legal_py(lv_obj_t * obj, char * k9_input, const char * py9_map[])
{
    lv_ime_pinyin_t * pinyin_ime = (lv_ime_pinyin_t *)obj;

    uint16_t len = strlen(k9_input);

    if((len == 0) || (len >= LV_IME_PINYIN_K9_MAX_INPUT)) {
        return;
    }

    char py_comp[LV_IME_PINYIN_K9_MAX_INPUT] = {0};
    int mark[LV_IME_PINYIN_K9_MAX_INPUT] = {0};
    int index = 0;
    int flag = 0;
    int count = 0;

    uint32_t ll_len = 0;
    ime_pinyin_k9_py_str_t * ll_index = NULL;

    ll_len = _lv_ll_get_len(&pinyin_ime->k9_legal_py_ll);
    ll_index = _lv_ll_get_head(&pinyin_ime->k9_legal_py_ll);

    while(index != -1) {
        if(index == len) {
            if(pinyin_k9_is_valid_py(obj, py_comp)) {
                if((count >= ll_len) || (ll_len == 0)) {
                    ll_index = _lv_ll_ins_tail(&pinyin_ime->k9_legal_py_ll);
                    strcpy(ll_index->py_str, py_comp);
                }
                else if((count < ll_len)) {
                    strcpy(ll_index->py_str, py_comp);
                    ll_index = _lv_ll_get_next(&pinyin_ime->k9_legal_py_ll, ll_index);
                }
                count++;
            }
            index--;
        }
        else {
            flag = mark[index];
            if(flag < strlen(py9_map[k9_input[index] - '2'])) {
                py_comp[index] = py9_map[k9_input[index] - '2'][flag];
                mark[index] = mark[index] + 1;
                index++;
            }
            else {
                mark[index] = 0;
                index--;
            }
        }
    }

    if(count > 0) {
        pinyin_ime->ta_count++;
        pinyin_ime->k9_legal_py_count = count;
    }
}


/*true: visible; false: not visible*/
static bool pinyin_k9_is_valid_py(lv_obj_t * obj, char * py_str)
{
    lv_ime_pinyin_t * pinyin_ime = (lv_ime_pinyin_t *)obj;

    lv_pinyin_dict_t * cpHZ = NULL;
    uint8_t index = 0, len = 0, offset = 0;
    uint16_t ret = 1;
    volatile uint8_t count = 0;

    if(*py_str == '\0')    return false;
    if(*py_str == 'i')     return false;
    if(*py_str == 'u')     return false;
    if(*py_str == 'v')     return false;

    offset = py_str[0] - 'a';
    len = strlen(py_str);

    cpHZ  = &pinyin_ime->dict[pinyin_ime->py_pos[offset]];
    count = pinyin_ime->py_num[offset];

    while(count--) {
        for(index = 0; index < len; index++) {
            if(*(py_str + index) != *((cpHZ->py) + index)) {
                break;
            }
        }

        // perfect match
        if(len == 1 || index == len) {
            return true;
        }
        cpHZ++;
    }
    return false;
}


static void pinyin_k9_fill_cand(lv_obj_t * obj)
{
    static uint16_t len = 0;
    uint16_t index = 0, tmp_len = 0;
    ime_pinyin_k9_py_str_t * ll_index = NULL;

    lv_ime_pinyin_t * pinyin_ime = (lv_ime_pinyin_t *)obj;

    tmp_len = pinyin_ime->k9_legal_py_count;

    if(tmp_len != len) {
        lv_memset_00(lv_pinyin_k9_cand_str, sizeof(lv_pinyin_k9_cand_str));
        strcpy(lv_pinyin_k9_cand_str[LV_IME_PINYIN_K9_CAND_TEXT_NUM], LV_SYMBOL_RIGHT"\0");
        strcpy(lv_pinyin_k9_cand_str[LV_IME_PINYIN_K9_CAND_TEXT_NUM + 1], "\0");
        len = tmp_len;
    }

    ll_index = _lv_ll_get_head(&pinyin_ime->k9_legal_py_ll);
    strcpy(pinyin_ime->input_char, ll_index->py_str);
    while(ll_index) {
        if((index >= LV_IME_PINYIN_K9_CAND_TEXT_NUM) || \
           (index >= pinyin_ime->k9_legal_py_count))
            break;

        strcpy(lv_pinyin_k9_cand_str[index], ll_index->py_str);
        ll_index = _lv_ll_get_next(&pinyin_ime->k9_legal_py_ll, ll_index); /*Find the next list*/
        index++;
    }
    pinyin_ime->k9_py_ll_pos = index;

    lv_obj_t * ta = lv_keyboard_get_textarea(pinyin_ime->kb);
    for(index = 0; index < pinyin_ime->k9_input_str_len; index++) {
        lv_textarea_del_char(ta);
    }
    pinyin_ime->k9_input_str_len = strlen(pinyin_ime->input_char);
    lv_textarea_add_text(ta, pinyin_ime->input_char);
}


static void pinyin_k9_cand_page_proc(lv_obj_t * obj, uint16_t dir)
{
    lv_ime_pinyin_t * pinyin_ime = (lv_ime_pinyin_t *)obj;

    lv_obj_t * ta = lv_keyboard_get_textarea(pinyin_ime->kb);
    uint16_t ll_len =  _lv_ll_get_len(&pinyin_ime->k9_legal_py_ll);

    if((ll_len > LV_IME_PINYIN_K9_CAND_TEXT_NUM) && (pinyin_ime->k9_legal_py_count > LV_IME_PINYIN_K9_CAND_TEXT_NUM)) {
        ime_pinyin_k9_py_str_t * ll_index = NULL;
        uint16_t tmp_btn_str_len = 0;
        int count = 0;

        ll_index = _lv_ll_get_head(&pinyin_ime->k9_legal_py_ll);
        while(ll_index) {
            if(count >= pinyin_ime->k9_py_ll_pos)   break;

            ll_index = _lv_ll_get_next(&pinyin_ime->k9_legal_py_ll, ll_index); /*Find the next list*/
            count++;
        }

        if((NULL == ll_index) && (dir == 1))   return;

        lv_memset_00(lv_pinyin_k9_cand_str, sizeof(lv_pinyin_k9_cand_str));
        strcpy(lv_pinyin_k9_cand_str[LV_IME_PINYIN_K9_CAND_TEXT_NUM], LV_SYMBOL_RIGHT"\0");
        strcpy(lv_pinyin_k9_cand_str[LV_IME_PINYIN_K9_CAND_TEXT_NUM + 1], "\0");

        // next page
        if(dir == 1) {
            count = 0;
            while(ll_index) {
                if(count >= (LV_IME_PINYIN_K9_CAND_TEXT_NUM - 1))
                    break;

                strcpy(lv_pinyin_k9_cand_str[count], ll_index->py_str);
                ll_index = _lv_ll_get_next(&pinyin_ime->k9_legal_py_ll, ll_index); /*Find the next list*/
                count++;
            }
            pinyin_ime->k9_py_ll_pos += count - 1;

        }
        // previous page
        else {
            count = LV_IME_PINYIN_K9_CAND_TEXT_NUM - 1;
            ll_index = _lv_ll_get_prev(&pinyin_ime->k9_legal_py_ll, ll_index);
            while(ll_index) {
                if(count < 0)  break;

                strcpy(lv_pinyin_k9_cand_str[count], ll_index->py_str);
                ll_index = _lv_ll_get_prev(&pinyin_ime->k9_legal_py_ll, ll_index); /*Find the previous list*/
                count--;
            }

            if(pinyin_ime->k9_py_ll_pos > LV_IME_PINYIN_K9_CAND_TEXT_NUM)
                pinyin_ime->k9_py_ll_pos -= 1;
        }

        lv_textarea_set_cursor_pos(ta, LV_TEXTAREA_CURSOR_LAST);
    }
}

#endif  /*LV_IME_PINYIN_USE_K9_MODE*/

#endif  /*LV_USE_IME_PINYIN*/

