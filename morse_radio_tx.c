#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view.h>
#include <gui/elements.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/text_input.h>
#include <gui/modules/variable_item_list.h>
#include <notification/notification_messages.h>
#include <lib/toolbox/level_duration.h>
#include <lib/subghz/devices/devices.h>
#include <lib/subghz/devices/cc1101_int/cc1101_int_interconnect.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define MAX_TEXT  40
#define MAX_ELEMS 700

typedef enum {
    ViewMain,
    ViewSettings,
    ViewText
} ViewId;
typedef enum {
    BandPMR,
    BandLPD
} Band;

typedef struct {
    uint32_t dur_us;
    bool on;
} Elem;

typedef struct MorseApp {
    ViewDispatcher* vd;
    View* main_view;
    VariableItemList* settings;
    TextInput* text_input;
    NotificationApp* notif;

    VariableItem* item_msg;
    VariableItem* item_band;
    VariableItem* item_chan;
    VariableItem* item_wpm;
    VariableItem* item_mode;
    VariableItem* item_tone;

    // impostazioni
    Band band;
    uint8_t channel; // 1-based
    uint8_t wpm;
    uint8_t mode; // 0 = tono FM, 1 = tono OOK, 2 = CW OOK
    uint8_t tone_idx;
    char text[MAX_TEXT + 1];

    // trasmissione
    FuriThread* thread;
    volatile bool abort;
    volatile bool sending;
    volatile uint8_t result; // 0 ok, 1 TX non permessa, 2 errore
    Elem elems[MAX_ELEMS];
    volatile size_t count;
    volatile size_t idx;
    uint32_t unit_us;
    uint32_t half_us; // mezzo periodo del tono (0 = CW)
    uint32_t rem;
    bool started;
    bool phase;

    char chan_text[32];
    char wpm_text[8];
    char tone_text[12];
    char msg_text[20];
} MorseApp;

typedef struct {
    MorseApp* app;
} MainModel;

static const uint16_t tone_hz[] = {500, 700, 1000};
static const char* band_names[] = {"PMR446", "LPD433"};
// 0 = FM: portante sempre accesa, tono in modulazione di frequenza (squelch resta aperto)
// 1 = OOK: tono ottenuto accendendo/spegnendo la portante
// 2 = CW: portante fissa a impulsi
static const char* mode_names[] = {"Tono FM", "Tono OOK", "CW (OOK)"};

/* ---------- Frequenze ---------- */

static uint8_t max_channel(Band b) {
    return (b == BandPMR) ? 16 : 69;
}

static uint32_t channel_freq(Band b, uint8_t ch) {
    if(b == BandPMR) return 446006250UL + 12500UL * (ch - 1); // 446.00625 + 12.5 kHz
    return 433075000UL + 25000UL * (ch - 1); // 433.075 + 25 kHz
}

/* ---------- Tabella morse ---------- */

typedef struct {
    char c;
    const char* code;
} MorseEntry;

static const MorseEntry morse_table[] = {
    {'A', ".-"},     {'B', "-..."},   {'C', "-.-."},   {'D', "-.."},    {'E', "."},
    {'F', "..-."},   {'G', "--."},    {'H', "...."},   {'I', ".."},     {'J', ".---"},
    {'K', "-.-"},    {'L', ".-.."},   {'M', "--"},     {'N', "-."},     {'O', "---"},
    {'P', ".--."},   {'Q', "--.-"},   {'R', ".-."},    {'S', "..."},    {'T', "-"},
    {'U', "..-"},    {'V', "...-"},   {'W', ".--"},    {'X', "-..-"},   {'Y', "-.--"},
    {'Z', "--.."},   {'0', "-----"},  {'1', ".----"},  {'2', "..---"},  {'3', "...--"},
    {'4', "....-"},  {'5', "....."},  {'6', "-...."},  {'7', "--..."},  {'8', "---.."},
    {'9', "----."},  {'.', ".-.-.-"}, {',', "--..--"}, {'?', "..--.."}, {'/', "-..-."},
    {'=', "-...-"},  {'-', "-....-"}, {'+', ".-.-."},  {'@', ".--.-."}, {'(', "-.--."},
    {')', "-.--.-"}, {'\'', ".----."}, {':', "---..."}, {'!', "-.-.--"},
};

static const char* morse_lookup(char c) {
    for(size_t i = 0; i < COUNT_OF(morse_table); i++) {
        if(morse_table[i].c == c) return morse_table[i].code;
    }
    return NULL;
}

/* ---------- Costruzione sequenza on/off ---------- */

static void push_us(MorseApp* a, bool on, uint32_t d) {
    if(a->count && a->elems[a->count - 1].on == on) {
        a->elems[a->count - 1].dur_us += d;
        return;
    }
    if(a->count < MAX_ELEMS) {
        a->elems[a->count].on = on;
        a->elems[a->count].dur_us = d;
        a->count++;
    }
}

static void push(MorseApp* a, bool on, uint32_t units) {
    push_us(a, on, units * a->unit_us);
}

static void push_code(MorseApp* a, const char* code) {
    for(; *code; code++) {
        push(a, true, (*code == '-') ? 3 : 1);
        if(code[1]) push(a, false, 1);
    }
}

static void prepare(MorseApp* a) {
    a->count = 0;
    a->idx = 0;
    a->unit_us = 1200000UL / a->wpm;
    a->half_us = (a->mode < 2) ? (500000UL / tone_hz[a->tone_idx]) : 0;
    // modo FM: la portante parte prima del morse, cosi' lo squelch ha il tempo di aprirsi
    if(a->mode == 0) push_us(a, false, 400000UL);
}

static void finish(MorseApp* a) {
    // modo FM: portante ancora accesa per un attimo dopo l'ultimo segno
    if(a->mode == 0) push_us(a, false, 300000UL);
}

static void build_text(MorseApp* a, const char* s) {
    prepare(a);
    for(; *s; s++) {
        char c = toupper((unsigned char)*s);
        if(c == ' ') {
            push(a, false, 4); // 3 (fine lettera) + 4 = 7 unita'
            continue;
        }
        const char* code = morse_lookup(c);
        if(!code) continue;
        push_code(a, code);
        push(a, false, 3);
    }
    finish(a);
}

static void build_sos(MorseApp* a) {
    prepare(a);
    for(int i = 0; i < 3; i++) {
        push_code(a, "...---..."); // prosign SOS, senza pause tra le lettere
        push(a, false, 7);
    }
    finish(a);
}

/* ---------- Callback TX asincrono (chiamato dal driver) ---------- */

static LevelDuration tx_cb(void* ctx) {
    MorseApp* a = ctx;
    if(a->abort || a->idx >= a->count) return level_duration_reset();

    Elem* e = &a->elems[a->idx];
    if(!a->started) {
        a->rem = e->dur_us;
        a->started = true;
        a->phase = true;
    }

    uint32_t chunk;
    LevelDuration ld;
    if(e->on && a->half_us) {
        // modo "tono": portante accesa/spenta a frequenza audio
        chunk = (a->half_us < a->rem) ? a->half_us : a->rem;
        ld = level_duration_make(a->phase, chunk);
        a->phase = !a->phase;
    } else {
        chunk = a->rem;
        ld = level_duration_make(e->on, chunk);
    }

    a->rem -= chunk;
    if(a->rem == 0) {
        a->idx++;
        a->started = false;
    }
    return ld;
}

/* ---------- UI helpers ---------- */

static void redraw(MorseApp* a) {
    with_view_model(a->main_view, MainModel * m, { UNUSED(m); }, true);
}

/* ---------- Thread di trasmissione ---------- */

static int32_t tx_thread(void* ctx) {
    MorseApp* a = ctx;
    uint32_t f = channel_freq(a->band, a->channel);

    if(!furi_hal_subghz_is_tx_allowed(f)) {
        a->result = 1;
    } else {
        // LED blu fisso durante la trasmissione
        notification_message(a->notif, &sequence_set_blue_255);

        subghz_devices_init();
        const SubGhzDevice* dev = subghz_devices_get_by_name(SUBGHZ_DEVICE_CC1101_INT_NAME);

        if(!dev) {
            a->result = 2;
        } else {
            subghz_devices_begin(dev);
            subghz_devices_reset(dev);
            subghz_devices_idle(dev);
            subghz_devices_load_preset(
                dev,
                (a->mode == 0) ? FuriHalSubGhzPreset2FSKDev238Async :
                                 FuriHalSubGhzPresetOok650Async,
                NULL);
            subghz_devices_set_frequency(dev, f);

            a->started = false;
            if(subghz_devices_start_async_tx(dev, tx_cb, a)) {
                while(!subghz_devices_is_async_complete_tx(dev) && !a->abort) {
                    furi_delay_ms(20);
                    redraw(a);
                }
                subghz_devices_stop_async_tx(dev);
            } else {
                a->result = 2;
            }
            subghz_devices_sleep(dev);
            subghz_devices_end(dev);
        }
        subghz_devices_deinit();

        // spegne il LED blu
        notification_message(a->notif, &sequence_reset_blue);
    }

    a->sending = false;
    redraw(a);
    return 0;
}

static void start_tx(MorseApp* a, bool sos) {
    if(a->sending) return;
    if(a->thread) {
        furi_thread_join(a->thread);
        furi_thread_free(a->thread);
        a->thread = NULL;
    }
    if(sos) {
        build_sos(a);
    } else {
        build_text(a, a->text);
    }
    if(!a->count) return;

    a->abort = false;
    a->result = 0;
    a->sending = true;
    a->thread = furi_thread_alloc_ex("MorseTx", 2048, tx_thread, a);
    furi_thread_start(a->thread);
    redraw(a);
}

/* ---------- Inserimento testo ---------- */

static void text_done(void* ctx) {
    MorseApp* a = ctx;
    view_dispatcher_switch_to_view(a->vd, ViewMain);
}

static void text_setup(MorseApp* a) {
    text_input_set_header_text(a->text_input, "Messaggio morse");
    text_input_set_result_callback(a->text_input, text_done, a, a->text, MAX_TEXT + 1, false);
}

/* ---------- Impostazioni ---------- */

static void update_chan_text(MorseApp* a) {
    uint32_t f = channel_freq(a->band, a->channel);
    snprintf(
        a->chan_text,
        sizeof(a->chan_text),
        "%02u %lu.%05lu",
        a->channel,
        f / 1000000UL,
        (f % 1000000UL) / 10UL);
    variable_item_set_current_value_text(a->item_chan, a->chan_text);
}

static void update_tone_text(MorseApp* a) {
    snprintf(a->tone_text, sizeof(a->tone_text), "%u Hz", tone_hz[a->tone_idx]);
    variable_item_set_current_value_text(a->item_tone, a->tone_text);
}

static void update_msg_text(MorseApp* a) {
    if(a->text[0] == '\0') {
        snprintf(a->msg_text, sizeof(a->msg_text), "(vuoto)");
    } else {
        snprintf(
            a->msg_text,
            sizeof(a->msg_text),
            "%.14s%s",
            a->text,
            strlen(a->text) > 14 ? ".." : "");
    }
    variable_item_set_current_value_text(a->item_msg, a->msg_text);
}

static void cb_band(VariableItem* it) {
    MorseApp* a = variable_item_get_context(it);
    a->band = variable_item_get_current_value_index(it);
    uint8_t maxc = max_channel(a->band);
    if(a->channel > maxc) a->channel = maxc;
    variable_item_set_current_value_text(it, band_names[a->band]);
    variable_item_set_values_count(a->item_chan, maxc);
    variable_item_set_current_value_index(a->item_chan, a->channel - 1);
    update_chan_text(a);
}

static void cb_chan(VariableItem* it) {
    MorseApp* a = variable_item_get_context(it);
    a->channel = variable_item_get_current_value_index(it) + 1;
    update_chan_text(a);
}

static void cb_wpm(VariableItem* it) {
    MorseApp* a = variable_item_get_context(it);
    a->wpm = variable_item_get_current_value_index(it) + 5;
    snprintf(a->wpm_text, sizeof(a->wpm_text), "%u", a->wpm);
    variable_item_set_current_value_text(it, a->wpm_text);
}

static void cb_mode(VariableItem* it) {
    MorseApp* a = variable_item_get_context(it);
    a->mode = variable_item_get_current_value_index(it);
    variable_item_set_current_value_text(it, mode_names[a->mode]);
}

static void cb_tone(VariableItem* it) {
    MorseApp* a = variable_item_get_context(it);
    a->tone_idx = variable_item_get_current_value_index(it);
    update_tone_text(a);
}

static void refresh_settings(MorseApp* a) {
    update_msg_text(a);

    variable_item_set_current_value_index(a->item_band, a->band);
    variable_item_set_current_value_text(a->item_band, band_names[a->band]);

    variable_item_set_values_count(a->item_chan, max_channel(a->band));
    variable_item_set_current_value_index(a->item_chan, a->channel - 1);
    update_chan_text(a);

    variable_item_set_current_value_index(a->item_wpm, a->wpm - 5);
    snprintf(a->wpm_text, sizeof(a->wpm_text), "%u", a->wpm);
    variable_item_set_current_value_text(a->item_wpm, a->wpm_text);

    variable_item_set_current_value_index(a->item_mode, a->mode);
    variable_item_set_current_value_text(a->item_mode, mode_names[a->mode]);

    variable_item_set_current_value_index(a->item_tone, a->tone_idx);
    update_tone_text(a);
}

static void cb_noop(VariableItem* it) {
    UNUSED(it);
}

static void settings_enter(void* ctx, uint32_t index) {
    MorseApp* a = ctx;
    if(index == 0) { // "Messaggio"
        text_setup(a);
        view_dispatcher_switch_to_view(a->vd, ViewText);
    }
}

/* ---------- Vista principale ---------- */

static void draw_arrow(Canvas* c, int x, int y, bool left) {
    for(int i = 0; i < 4; i++) {
        int xx = left ? x + i : x - i;
        canvas_draw_line(c, xx, y - i, xx, y + i);
    }
}

static void main_draw(Canvas* c, void* model) {
    MorseApp* a = ((MainModel*)model)->app;
    char buf[48];

    canvas_clear(c);

    // ---- intestazione ----
    canvas_set_color(c, ColorBlack);
    canvas_draw_box(c, 0, 0, 128, 12);
    canvas_set_color(c, ColorWhite);
    canvas_set_font(c, FontPrimary);
    canvas_draw_str(c, 3, 10, "Morse Radio TX");
    uint32_t lit = a->sending ? (1 + (furi_get_tick() / 200) % 3) : 3;
    for(uint32_t k = 0; k < 3; k++) {
        uint8_t h = 3 + k * 2;
        if(k < lit) {
            canvas_draw_box(c, 112 + k * 5, 11 - h, 3, h);
        } else {
            canvas_draw_frame(c, 112 + k * 5, 11 - h, 3, h);
        }
    }
    canvas_set_color(c, ColorBlack);

    // ---- banda / canale con frecce ----
    uint32_t f = channel_freq(a->band, a->channel);
    canvas_set_font(c, FontPrimary);
    snprintf(buf, sizeof(buf), "%s  CH %02u", band_names[a->band], a->channel);
    canvas_draw_str_aligned(c, 64, 14, AlignCenter, AlignTop, buf);
    draw_arrow(c, 6, 19, true);
    draw_arrow(c, 121, 19, false);

    // ---- frequenza ----
    canvas_set_font(c, FontSecondary);
    snprintf(buf, sizeof(buf), "%lu.%05lu MHz", f / 1000000UL, (f % 1000000UL) / 10UL);
    canvas_draw_str_aligned(c, 64, 25, AlignCenter, AlignTop, buf);

    // ---- riga info ----
    if(a->sending) {
        snprintf(buf, sizeof(buf), "TRASMISSIONE...");
    } else if(a->result == 1) {
        snprintf(buf, sizeof(buf), "TX non permessa (regione)");
    } else if(a->result == 2) {
        snprintf(buf, sizeof(buf), "Errore avvio TX");
    } else if(a->mode == 0) {
        snprintf(buf, sizeof(buf), "%u WPM  -  FM %u Hz", a->wpm, tone_hz[a->tone_idx]);
    } else if(a->mode == 1) {
        snprintf(buf, sizeof(buf), "%u WPM  -  OOK %u Hz", a->wpm, tone_hz[a->tone_idx]);
    } else {
        snprintf(buf, sizeof(buf), "%u WPM  -  CW", a->wpm);
    }
    canvas_draw_str_aligned(c, 64, 33, AlignCenter, AlignTop, buf);

    // ---- riquadro messaggio / barra di avanzamento ----
    canvas_draw_rframe(c, 2, 43, 124, 12, 3);
    if(a->sending) {
        size_t cnt = a->count ? a->count : 1;
        canvas_draw_box(c, 5, 46, (118 * a->idx) / cnt, 6);
    } else if(a->text[0] == '\0') {
        canvas_draw_str(c, 7, 52, "OK per scrivere il messaggio");
    } else {
        FuriString* t = furi_string_alloc_set(a->text);
        elements_string_fit_width(c, t, 114);
        canvas_draw_str(c, 7, 52, furi_string_get_cstr(t));
        furi_string_free(t);
    }

    // ---- suggerimenti ----
    if(a->sending) {
        canvas_draw_str_aligned(c, 64, 63, AlignCenter, AlignBottom, "Back = stop");
    } else if(a->text[0] == '\0') {
        canvas_draw_str_aligned(c, 64, 63, AlignCenter, AlignBottom, "OK scrivi   v SOS   ^ menu");
    } else {
        canvas_draw_str_aligned(c, 64, 63, AlignCenter, AlignBottom, "OK invia   v SOS   ^ menu");
    }
}

static bool main_input(InputEvent* e, void* ctx) {
    MorseApp* a = ctx;

    if(a->sending) {
        if(e->key == InputKeyBack && e->type == InputTypeShort) a->abort = true;
        return true; // durante la TX consuma tutto
    }

    bool consumed = true;
    uint8_t maxc = max_channel(a->band);

    if(e->type == InputTypeShort || e->type == InputTypeRepeat) {
        switch(e->key) {
        case InputKeyLeft:
            a->channel = (a->channel > 1) ? a->channel - 1 : maxc;
            a->result = 0;
            redraw(a);
            break;
        case InputKeyRight:
            a->channel = (a->channel < maxc) ? a->channel + 1 : 1;
            a->result = 0;
            redraw(a);
            break;
        case InputKeyDown:
            if(e->type == InputTypeShort) start_tx(a, true); // SOS rapido
            break;
        case InputKeyUp:
            if(e->type == InputTypeShort) {
                refresh_settings(a);
                view_dispatcher_switch_to_view(a->vd, ViewSettings);
            }
            break;
        case InputKeyOk:
            if(e->type == InputTypeShort) {
                if(a->text[0] == '\0') { // nessun messaggio: apre l'editor
                    text_setup(a);
                    view_dispatcher_switch_to_view(a->vd, ViewText);
                } else {
                    start_tx(a, false);
                }
            }
            break;
        default:
            consumed = false; // Back -> esce
            break;
        }
    } else if(e->type == InputTypeLong && e->key == InputKeyOk) {
        text_setup(a);
        view_dispatcher_switch_to_view(a->vd, ViewText);
    } else {
        consumed = (e->key != InputKeyBack);
    }
    return consumed;
}

static uint32_t main_prev(void* ctx) {
    UNUSED(ctx);
    return VIEW_NONE; // esce dall'app
}

static uint32_t to_main(void* ctx) {
    UNUSED(ctx);
    return ViewMain;
}

/* ---------- Entry point ---------- */

int32_t morse_radio_tx_app(void* p) {
    UNUSED(p);
    MorseApp* a = malloc(sizeof(MorseApp));
    memset(a, 0, sizeof(MorseApp));

    a->band = BandPMR;
    a->channel = 1;
    a->wpm = 15;
    a->mode = 0;
    a->tone_idx = 1;

    a->notif = furi_record_open(RECORD_NOTIFICATION);
    Gui* gui = furi_record_open(RECORD_GUI);
    a->vd = view_dispatcher_alloc();
    view_dispatcher_attach_to_gui(a->vd, gui, ViewDispatcherTypeFullscreen);

    // vista principale
    a->main_view = view_alloc();
    view_allocate_model(a->main_view, ViewModelTypeLockFree, sizeof(MainModel));
    with_view_model(a->main_view, MainModel * m, { m->app = a; }, false);
    view_set_context(a->main_view, a);
    view_set_draw_callback(a->main_view, main_draw);
    view_set_input_callback(a->main_view, main_input);
    view_set_previous_callback(a->main_view, main_prev);
    view_dispatcher_add_view(a->vd, ViewMain, a->main_view);

    // impostazioni
    a->settings = variable_item_list_alloc();
    a->item_msg = variable_item_list_add(a->settings, "Messaggio (OK)", 1, cb_noop, a);
    variable_item_list_set_enter_callback(a->settings, settings_enter, a);
    a->item_band = variable_item_list_add(a->settings, "Banda", 2, cb_band, a);
    a->item_chan = variable_item_list_add(a->settings, "Canale", 16, cb_chan, a);
    a->item_wpm = variable_item_list_add(a->settings, "Velocita WPM", 26, cb_wpm, a);
    a->item_mode = variable_item_list_add(a->settings, "Modo", 3, cb_mode, a);
    a->item_tone = variable_item_list_add(a->settings, "Tono", 3, cb_tone, a);
    view_set_previous_callback(variable_item_list_get_view(a->settings), to_main);
    view_dispatcher_add_view(a->vd, ViewSettings, variable_item_list_get_view(a->settings));

    // inserimento testo
    a->text_input = text_input_alloc();
    view_set_previous_callback(text_input_get_view(a->text_input), to_main);
    view_dispatcher_add_view(a->vd, ViewText, text_input_get_view(a->text_input));

    view_dispatcher_switch_to_view(a->vd, ViewMain);
    view_dispatcher_run(a->vd);

    // cleanup
    a->abort = true;
    if(a->thread) {
        furi_thread_join(a->thread);
        furi_thread_free(a->thread);
    }
    view_dispatcher_remove_view(a->vd, ViewMain);
    view_dispatcher_remove_view(a->vd, ViewSettings);
    view_dispatcher_remove_view(a->vd, ViewText);
    view_free(a->main_view);
    variable_item_list_free(a->settings);
    text_input_free(a->text_input);
    view_dispatcher_free(a->vd);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    free(a);
    return 0;
}
