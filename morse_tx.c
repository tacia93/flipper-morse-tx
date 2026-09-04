/*
 * Morse TX - send text in Morse code from a Flipper Zero.
 * Copyright (C) 2026 Emanuele Colucci
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * General Public License in the LICENSE file for details.
 */
/**
 * Morse TX - keys a CW/OOK carrier on the Flipper Zero external CC1101 module.
 *
 * The message is compiled into a list of level/duration elements and fed to
 * the sub-ghz async transmitter, which drives the module's GDO0 pin: carrier
 * on for dots/dashes, off for the gaps.
 */
#include "morse.h"

#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_subghz.h>

#include <stdio.h>
#include <string.h>

#include <gui/gui.h>
#include <gui/elements.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_input.h>
#include <gui/modules/variable_item_list.h>
#include <gui/modules/widget.h>
#include <notification/notification_messages.h>
#include <storage/storage.h>
#include <toolbox/level_duration.h>

#include <lib/flipper_format/flipper_format.h>
#include <lib/subghz/devices/devices.h>

/* Device names from the sub-ghz device registry. */
#define MORSE_TX_DEVICE_EXTERNAL "cc1101_ext"
#define MORSE_TX_DEVICE_INTERNAL "cc1101_int"

/* The radio check report is also written here so it can be read over the CLI. */
#define MORSE_TX_REPORT_DIR  EXT_PATH("apps_data/morse_tx")
#define MORSE_TX_REPORT_FILE EXT_PATH("apps_data/morse_tx/radio_check.txt")
#define MORSE_TX_TXLOG_FILE  EXT_PATH("apps_data/morse_tx/last_tx.txt")

#define MORSE_TX_CONFIG_FILE    EXT_PATH("apps_data/morse_tx/morse_tx.conf")
#define MORSE_TX_CONFIG_HEADER  "Morse TX settings"
#define MORSE_TX_CONFIG_VERSION 1

#define MORSE_TX_TICK_PERIOD_MS  20
#define MORSE_TX_SIDETONE_HZ     700.0f
#define MORSE_TX_SIDETONE_VOLUME 0.6f

typedef enum {
    MorseTxViewMenu,
    MorseTxViewSettings,
    MorseTxViewMessage,
    MorseTxViewTx,
    MorseTxViewDiag,
    MorseTxViewAbout,
} MorseTxViewId;

typedef enum {
    MorseTxMenuTransmit,
    MorseTxMenuMessage,
    MorseTxMenuSettings,
    MorseTxMenuDiag,
    MorseTxMenuAbout,
} MorseTxMenuIndex;

static const uint32_t morse_tx_frequencies[] = {
    300000000,
    303875000,
    310000000,
    315000000,
    318000000,
    390000000,
    418000000,
    433075000,
    433420000,
    433920000,
    434775000,
    868350000,
    915000000,
};

static const uint8_t morse_tx_speeds[] = {5, 8, 10, 12, 15, 18, 20, 25, 30, 40};

static const uint8_t morse_tx_repeats[] = {1, 2, 3, 5, 10, 0}; /* 0 = infinite */
static const char* const morse_tx_repeat_names[] = {"1", "2", "3", "5", "10", "loop"};

static const char* const morse_tx_module_names[] = {"External", "Internal"};

/* Header pins that could carry the chip select of a non-standard module.
   SCK/MOSI/MISO are fixed by the SPI peripheral, so they are not scanned. */
typedef struct {
    const GpioPin* pin;
    const char* name;
} MorseTxPinCandidate;

static const MorseTxPinCandidate morse_tx_cs_candidates[] = {
    {&gpio_ext_pa4, "A4 p4"},
    {&gpio_ext_pa7, "A7 p2"},
    {&gpio_ext_pc3, "C3 p7"},
    {&gpio_ext_pc1, "C1 p15"},
    {&gpio_ext_pc0, "C0 p16"},
};

typedef struct {
    char message[MORSE_MESSAGE_MAX + 1];
    const char* pattern;
    uint32_t frequency;
    uint32_t elapsed_s;
    uint32_t total_s;
    char error[64];
    size_t char_index;
    int sym_index;
    uint8_t wpm;
    uint8_t progress;
    uint8_t repeats_left;
    bool repeat_infinite;
    bool key_down;
    bool active;
} MorseTxViewModel;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    NotificationApp* notifications;
    Submenu* submenu;
    VariableItemList* settings;
    TextInput* text_input;
    Widget* about;
    Widget* diag;
    View* tx_view;
    VariableItem* module_item;

    /* settings */
    char message[MORSE_MESSAGE_MAX + 1];
    uint8_t freq_index;
    uint8_t speed_index;
    uint8_t repeat_index;
    uint8_t module_index;
    bool sidetone;
    bool power_5v;

    /* radio / transmission state */
    const SubGhzDevice* device;
    MorseSequence* seq;
    uint32_t tx_start_tick;
    size_t locate_hint;
    uint8_t repeats_left;
    bool device_open;
    bool tx_running;
    bool otg_enabled_by_us;
    bool speaker_acquired;
    bool tone_on;
    bool led_on;
    bool tx_sampled;
    uint8_t tx_marcstate;
} MorseTxApp;

/* ------------------------------------------------------------------ radio */

/** Called from the sub-ghz DMA/interrupt context: must stay allocation free. */
static LevelDuration morse_tx_yield(void* context) {
    MorseTxApp* app = context;
    MorseSequence* seq = app->seq;
    if(!seq || seq->pos >= seq->count) return level_duration_reset();
    const MorseElement* element = &seq->elements[seq->pos++];
    return level_duration_make(element->level != 0, element->duration_us);
}

static void morse_tx_radio_stop(MorseTxApp* app) {
    if(app->tx_running) {
        subghz_devices_stop_async_tx(app->device);
        app->tx_running = false;
    }
    if(app->device_open) {
        subghz_devices_idle(app->device);
        subghz_devices_sleep(app->device);
        subghz_devices_end(app->device);
        app->device_open = false;
    }
    app->device = NULL;
    if(app->otg_enabled_by_us) {
        furi_hal_power_disable_otg();
        app->otg_enabled_by_us = false;
    }
    if(app->seq) {
        morse_sequence_free(app->seq);
        app->seq = NULL;
    }
}

static void morse_tx_set_error(char* error, size_t error_size, const char* text) {
    strncpy(error, text, error_size - 1);
    error[error_size - 1] = '\0';
}

/**
 * Probe the GPIO header for a CC1101 module. begin() always has to be paired
 * with end(), even when it reports a failure, or the next begin() asserts.
 */
static bool morse_tx_probe_external(uint32_t settle_ms) {
    const SubGhzDevice* device = subghz_devices_get_by_name(MORSE_TX_DEVICE_EXTERNAL);
    if(!device) return false;
    if(settle_ms) furi_delay_ms(settle_ms);

    subghz_devices_begin(device);
    bool connected = subghz_devices_is_connect(device);
    subghz_devices_end(device);
    return connected;
}

/** True when the chip identifies itself as a CC1101 (or a known clone). */
static bool morse_tx_version_is_cc1101(uint8_t version) {
    return version == 0x14 || version == 0x04 || version == 0x17 || version == 0x07;
}

static bool morse_tx_probe_begin_result(void) {
    const SubGhzDevice* device = subghz_devices_get_by_name(MORSE_TX_DEVICE_EXTERNAL);
    if(!device) return false;
    bool begin_ok = subghz_devices_begin(device);
    subghz_devices_end(device);
    return begin_ok;
}

/**
 * Give the chip select a clean falling edge before talking to the module.
 * Straight after a failed driver init the line can be left asserted, and the
 * CC1101 then ignores the next transaction: releasing CS to input and driving
 * it again resynchronises its SPI state machine.
 */
static void morse_tx_cs_wakeup(void) {
    const FuriHalSpiBusHandle* handle = &furi_hal_spi_bus_handle_external;
    furi_hal_gpio_init(handle->cs, GpioModeInput, GpioPullUp, GpioSpeedLow);
    furi_delay_ms(2);
    furi_hal_gpio_init(handle->cs, GpioModeOutputPushPull, GpioPullNo, GpioSpeedVeryHigh);
    furi_hal_gpio_write(handle->cs, true);
    furi_delay_us(100);
}

/**
 * Talk to the CC1101 on the external header without going through the driver:
 * status registers are read with the burst bit set (addr | 0xC0), and the chip
 * pulls MISO low once it is ready to answer.
 */
static bool morse_tx_raw_probe_cs(const GpioPin* cs, uint8_t* partnum, uint8_t* version) {
    const FuriHalSpiBusHandle* handle = &furi_hal_spi_bus_handle_external;
    uint8_t tx[2] = {0};
    uint8_t rx[2] = {0};

    *partnum = 0xFF;
    *version = 0xFF;

    furi_hal_spi_acquire(handle);
    if(cs != handle->cs) {
        furi_hal_gpio_init(cs, GpioModeOutputPushPull, GpioPullNo, GpioSpeedLow);
        furi_hal_gpio_write(handle->cs, true); /* keep the standard CS deasserted */
    }
    furi_hal_gpio_write(cs, false);

    uint32_t timeout = 2000; /* ~20 ms waiting for the chip to be ready */
    while(furi_hal_gpio_read(handle->miso) && timeout) {
        furi_delay_us(10);
        timeout--;
    }
    bool ready = timeout > 0;

    if(ready) {
        tx[0] = 0xF0; /* PARTNUM, read + burst */
        furi_hal_spi_bus_trx(handle, tx, rx, 2, 100);
        *partnum = rx[1];
        tx[0] = 0xF1; /* VERSION, read + burst */
        furi_hal_spi_bus_trx(handle, tx, rx, 2, 100);
        *version = rx[1];
    }

    furi_hal_gpio_write(cs, true);
    if(cs != handle->cs) furi_hal_gpio_init(cs, GpioModeAnalog, GpioPullNo, GpioSpeedLow);
    furi_hal_spi_release(handle);
    return ready;
}

static bool morse_tx_raw_probe(uint8_t* partnum, uint8_t* version) {
    return morse_tx_raw_probe_cs(furi_hal_spi_bus_handle_external.cs, partnum, version);
}

/**
 * Read MARCSTATE (0x35) while the module is keying. The SPI bus is idle during
 * an async transmission - the data goes out over GDO0 - so this is safe.
 * A chip that is really transmitting reports 0x13 (TX).
 */
static uint8_t morse_tx_read_marcstate(void) {
    const FuriHalSpiBusHandle* handle = &furi_hal_spi_bus_handle_external;
    uint8_t tx[2] = {0xF5, 0x00}; /* MARCSTATE, read + burst */
    uint8_t rx[2] = {0};

    furi_hal_spi_acquire(handle);
    furi_hal_gpio_write(handle->cs, true);
    furi_delay_us(50);
    furi_hal_gpio_write(handle->cs, false);

    uint32_t timeout = 500;
    while(furi_hal_gpio_read(handle->miso) && timeout) {
        furi_delay_us(10);
        timeout--;
    }
    furi_hal_spi_bus_trx(handle, tx, rx, 2, 100);

    furi_hal_gpio_write(handle->cs, true);
    furi_hal_spi_release(handle);
    return rx[1] & 0x1F;
}

/**
 * Sample a header pin without driving it. A powered CC1101 clocks its GDO0 by
 * default, so a toggling pin proves the chip is alive; a pin that just follows
 * the internal pull-up/pull-down is not connected to anything driving it.
 */
static const char* morse_tx_pin_state(const GpioPin* pin) {
    furi_hal_gpio_init(pin, GpioModeInput, GpioPullNo, GpioSpeedLow);
    uint32_t highs = 0;
    for(uint32_t i = 0; i < 200; i++) {
        if(furi_hal_gpio_read(pin)) highs++;
        furi_delay_us(20);
    }

    furi_hal_gpio_init(pin, GpioModeInput, GpioPullUp, GpioSpeedLow);
    furi_delay_us(500);
    bool with_pullup = furi_hal_gpio_read(pin);
    furi_hal_gpio_init(pin, GpioModeInput, GpioPullDown, GpioSpeedLow);
    furi_delay_us(500);
    bool with_pulldown = furi_hal_gpio_read(pin);

    furi_hal_gpio_init(pin, GpioModeAnalog, GpioPullNo, GpioSpeedLow);

    if(highs > 20 && highs < 180) return "TOGGLING";
    if(with_pullup != with_pulldown) return "floating";
    return with_pullup ? "driven hi" : "driven lo";
}

static void morse_tx_save_file(const char* path, const char* text) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(storage, MORSE_TX_REPORT_DIR);
    File* file = storage_file_alloc(storage);
    if(storage_file_open(file, path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_write(file, text, strlen(text));
    }
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}

static void morse_tx_save_report(const char* text) {
    morse_tx_save_file(MORSE_TX_REPORT_FILE, text);
}

/* ------------------------------------------------------------- settings io */

/* Settings are stored by value, not by index, so reordering the lists in a
   later version cannot silently change what a saved file means. */

static uint8_t
    morse_tx_index_of_u32(const uint32_t* list, size_t count, uint32_t value, uint8_t fallback) {
    for(size_t i = 0; i < count; i++) {
        if(list[i] == value) return (uint8_t)i;
    }
    return fallback;
}

static uint8_t
    morse_tx_index_of_u8(const uint8_t* list, size_t count, uint32_t value, uint8_t fallback) {
    for(size_t i = 0; i < count; i++) {
        if(list[i] == value) return (uint8_t)i;
    }
    return fallback;
}

static bool morse_tx_settings_load(MorseTxApp* app) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* file = flipper_format_file_alloc(storage);
    FuriString* header = furi_string_alloc();
    FuriString* message = furi_string_alloc();
    uint32_t version = 0;
    uint32_t value = 0;
    bool loaded = false;

    do {
        if(!flipper_format_file_open_existing(file, MORSE_TX_CONFIG_FILE)) break;
        if(!flipper_format_read_header(file, header, &version)) break;
        if(furi_string_cmp_str(header, MORSE_TX_CONFIG_HEADER) != 0) break;
        if(version != MORSE_TX_CONFIG_VERSION) break;

        /* keys are read in the order they are written */
        if(flipper_format_read_string(file, "Message", message)) {
            strncpy(app->message, furi_string_get_cstr(message), MORSE_MESSAGE_MAX);
            app->message[MORSE_MESSAGE_MAX] = '\0';
        }
        if(flipper_format_read_uint32(file, "Frequency", &value, 1)) {
            app->freq_index = morse_tx_index_of_u32(
                morse_tx_frequencies, COUNT_OF(morse_tx_frequencies), value, app->freq_index);
        }
        if(flipper_format_read_uint32(file, "Speed", &value, 1)) {
            app->speed_index = morse_tx_index_of_u8(
                morse_tx_speeds, COUNT_OF(morse_tx_speeds), value, app->speed_index);
        }
        if(flipper_format_read_uint32(file, "Repeat", &value, 1)) {
            app->repeat_index = morse_tx_index_of_u8(
                morse_tx_repeats, COUNT_OF(morse_tx_repeats), value, app->repeat_index);
        }
        if(flipper_format_read_uint32(file, "Module", &value, 1)) {
            app->module_index = (value < COUNT_OF(morse_tx_module_names)) ? (uint8_t)value : 0;
        }
        if(flipper_format_read_uint32(file, "Power5v", &value, 1)) app->power_5v = value != 0;
        if(flipper_format_read_uint32(file, "Sidetone", &value, 1)) app->sidetone = value != 0;
        loaded = true;
    } while(false);

    furi_string_free(message);
    furi_string_free(header);
    flipper_format_free(file);
    furi_record_close(RECORD_STORAGE);
    return loaded;
}

static void morse_tx_settings_save(MorseTxApp* app) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(storage, MORSE_TX_REPORT_DIR);
    FlipperFormat* file = flipper_format_file_alloc(storage);

    if(flipper_format_file_open_always(file, MORSE_TX_CONFIG_FILE)) {
        uint32_t value;
        flipper_format_write_header_cstr(file, MORSE_TX_CONFIG_HEADER, MORSE_TX_CONFIG_VERSION);
        flipper_format_write_string_cstr(file, "Message", app->message);
        value = morse_tx_frequencies[app->freq_index];
        flipper_format_write_uint32(file, "Frequency", &value, 1);
        value = morse_tx_speeds[app->speed_index];
        flipper_format_write_uint32(file, "Speed", &value, 1);
        value = morse_tx_repeats[app->repeat_index];
        flipper_format_write_uint32(file, "Repeat", &value, 1);
        value = app->module_index;
        flipper_format_write_uint32(file, "Module", &value, 1);
        value = app->power_5v ? 1 : 0;
        flipper_format_write_uint32(file, "Power5v", &value, 1);
        value = app->sidetone ? 1 : 0;
        flipper_format_write_uint32(file, "Sidetone", &value, 1);
    }

    flipper_format_free(file);
    furi_record_close(RECORD_STORAGE);
}

static bool morse_tx_external_present(void) {
    bool otg_was_enabled = furi_hal_power_is_otg_enabled();
    bool otg_enable_ok = otg_was_enabled;
    if(!otg_was_enabled) otg_enable_ok = furi_hal_power_enable_otg();
    bool otg_now = furi_hal_power_is_otg_enabled();
    bool otg_fault = furi_hal_power_check_otg_fault();
    bool charging = furi_hal_power_is_charging();

    if(!otg_was_enabled) furi_delay_ms(250);

    morse_tx_cs_wakeup();
    uint8_t partnum = 0;
    uint8_t version = 0;
    bool raw_ready = morse_tx_raw_probe(&partnum, &version);

    /* The plugin's is_connect() reports "absent" on some boards even when the
       chip answers and its own begin() succeeds, so identify the chip here. */
    bool connected = raw_ready && morse_tx_version_is_cc1101(version);

    morse_tx_cs_wakeup();
    bool driver_says_connected = morse_tx_probe_external(0);
    morse_tx_cs_wakeup();
    bool begin_ok = morse_tx_probe_begin_result();

    if(!otg_was_enabled) furi_hal_power_disable_otg();

    FuriString* report = furi_string_alloc();
    furi_string_cat_printf(
        report,
        "startup probe\next driver: %s\nint driver: %s\n5v already on: %s\n"
        "otg enable ok: %s\notg on now: %s\notg fault: %s\ncharging: %s\n"
        "driver found: %s\nbegin returned: %s\nraw says cc1101: %s\n"
        "raw spi ready: %s\nraw partnum: 0x%02X\nraw version: 0x%02X\n",
        subghz_devices_get_by_name(MORSE_TX_DEVICE_EXTERNAL) ? "loaded" : "MISSING",
        subghz_devices_get_by_name(MORSE_TX_DEVICE_INTERNAL) ? "loaded" : "MISSING",
        otg_was_enabled ? "yes" : "no",
        otg_enable_ok ? "yes" : "no",
        otg_now ? "yes" : "no",
        otg_fault ? "YES" : "no",
        charging ? "yes" : "no",
        driver_says_connected ? "yes" : "no",
        begin_ok ? "yes" : "no",
        connected ? "yes" : "no",
        raw_ready ? "yes" : "no",
        partnum,
        version);

    if(!connected) {
        /* only worth the extra probing when something is wrong */
        for(size_t i = 0; i < COUNT_OF(morse_tx_cs_candidates); i++) {
            const GpioPin* pin = morse_tx_cs_candidates[i].pin;
            const char* state = morse_tx_pin_state(pin);
            furi_string_cat_printf(report, "%s: %s", morse_tx_cs_candidates[i].name, state);
            if(strcmp(state, "floating") == 0) {
                uint8_t cs_part = 0;
                uint8_t cs_ver = 0;
                morse_tx_raw_probe_cs(pin, &cs_part, &cs_ver);
                furi_string_cat_printf(report, " ver %02X", cs_ver);
            }
            furi_string_cat_printf(report, "\n");
        }
    }

    morse_tx_save_report(furi_string_get_cstr(report));
    furi_string_free(report);

    return connected;
}

static bool morse_tx_radio_start(MorseTxApp* app, char* error, size_t error_size) {
    uint8_t wpm = morse_tx_speeds[app->speed_index];
    uint32_t frequency = morse_tx_frequencies[app->freq_index];

    app->seq = morse_sequence_alloc(app->message, wpm);
    if(!app->seq) {
        morse_tx_set_error(error, error_size, "Empty message.\nNothing to send.");
        return false;
    }

    bool external = (app->module_index == 0);
    bool usb_blocks_5v = false;
    if(external && app->power_5v && !furi_hal_power_is_otg_enabled()) {
        if(furi_hal_power_enable_otg()) {
            app->otg_enabled_by_us = true;
            furi_delay_ms(250); /* let the module power rail settle */
        } else {
            /* the boost cannot run while the USB cable is supplying VBUS */
            usb_blocks_5v = furi_hal_power_is_charging();
        }
    }

    app->device =
        subghz_devices_get_by_name(external ? MORSE_TX_DEVICE_EXTERNAL : MORSE_TX_DEVICE_INTERNAL);
    if(!app->device) {
        morse_tx_set_error(error, error_size, "Radio device\nnot registered");
        return false;
    }

    /* Identify the external chip ourselves before handing over to the driver:
       its is_connect() says "absent" on boards that answer perfectly well. */
    if(external) {
        morse_tx_cs_wakeup();
        uint8_t partnum = 0;
        uint8_t version = 0;
        morse_tx_raw_probe(&partnum, &version);
        if(!morse_tx_version_is_cc1101(version)) {
            if(usb_blocks_5v) {
                morse_tx_set_error(
                    error,
                    error_size,
                    "No 5V while USB is\nplugged in. Unplug\nthe cable, or power\nthe module from 3V3");
            } else {
                morse_tx_set_error(
                    error, error_size, "External module\nnot answering.\nCheck wiring / 5V");
            }
            return false;
        }
        morse_tx_cs_wakeup();
    }

    /* From here on end() is mandatory, so mark the device open right away. */
    subghz_devices_begin(app->device);
    app->device_open = true;

    if(!external && !subghz_devices_is_connect(app->device)) {
        morse_tx_set_error(error, error_size, "Internal radio\nnot responding");
        return false;
    }

    subghz_devices_reset(app->device);
    subghz_devices_load_preset(app->device, FuriHalSubGhzPresetOok650Async, NULL);

    if(!subghz_devices_is_frequency_valid(app->device, frequency)) {
        morse_tx_set_error(error, error_size, "Frequency not\nsupported by\nthe module");
        return false;
    }
    subghz_devices_set_frequency(app->device, frequency);

    morse_sequence_rewind(app->seq);
    if(!subghz_devices_start_async_tx(app->device, morse_tx_yield, app)) {
        morse_tx_set_error(error, error_size, "TX blocked.\nFrequency not\nallowed here");
        return false;
    }
    app->tx_running = true;
    return true;
}

/* ------------------------------------------------------------ tx feedback */

static void morse_tx_sidetone(MorseTxApp* app, bool on) {
    if(!app->speaker_acquired || app->tone_on == on) return;
    if(on) {
        furi_hal_speaker_start(MORSE_TX_SIDETONE_HZ, MORSE_TX_SIDETONE_VOLUME);
    } else {
        furi_hal_speaker_stop();
    }
    app->tone_on = on;
}

static void morse_tx_led(MorseTxApp* app, bool on) {
    if(app->led_on == on) return;
    notification_message(app->notifications, on ? &sequence_set_only_red_255 : &sequence_reset_red);
    app->led_on = on;
}

static void morse_tx_stop(MorseTxApp* app) {
    morse_tx_sidetone(app, false);
    if(app->speaker_acquired) {
        furi_hal_speaker_release();
        app->speaker_acquired = false;
    }
    morse_tx_led(app, false);
    morse_tx_radio_stop(app);
    notification_message(app->notifications, &sequence_display_backlight_enforce_auto);

    with_view_model(
        app->tx_view,
        MorseTxViewModel * model,
        {
            model->active = false;
            model->key_down = false;
        },
        true);
}

static void morse_tx_start(MorseTxApp* app) {
    char error[64] = {0};
    app->locate_hint = 0;
    app->tx_sampled = false;
    app->tx_marcstate = 0xFF;
    app->repeats_left = morse_tx_repeats[app->repeat_index];

    bool ok = morse_tx_radio_start(app, error, sizeof(error));
    uint32_t total_s = app->seq ? (app->seq->total_us / 1000000u) : 0;

    with_view_model(
        app->tx_view,
        MorseTxViewModel * model,
        {
            strncpy(model->message, app->message, MORSE_MESSAGE_MAX);
            model->message[MORSE_MESSAGE_MAX] = '\0';
            strncpy(model->error, error, sizeof(model->error) - 1);
            model->error[sizeof(model->error) - 1] = '\0';
            model->frequency = morse_tx_frequencies[app->freq_index];
            model->wpm = morse_tx_speeds[app->speed_index];
            model->repeat_infinite = (morse_tx_repeats[app->repeat_index] == 0);
            model->repeats_left = app->repeats_left;
            model->total_s = total_s;
            model->elapsed_s = 0;
            model->progress = 0;
            model->char_index = 0;
            model->sym_index = -1;
            model->pattern = NULL;
            model->key_down = false;
            model->active = ok;
        },
        true);

    FuriString* log = furi_string_alloc();
    furi_string_cat_printf(
        log,
        "module: %s\nfrequency: %lu\nwpm: %u\nmessage: %s\nstarted: %s\nerror: %s\n",
        morse_tx_module_names[app->module_index],
        (unsigned long)morse_tx_frequencies[app->freq_index],
        morse_tx_speeds[app->speed_index],
        app->message,
        ok ? "yes" : "no",
        error[0] ? error : "-");
    morse_tx_save_file(MORSE_TX_TXLOG_FILE, furi_string_get_cstr(log));
    furi_string_free(log);

    if(ok) {
        app->tx_start_tick = furi_get_tick();
        notification_message(app->notifications, &sequence_display_backlight_enforce_on);
        if(app->sidetone && furi_hal_speaker_acquire(100)) {
            app->speaker_acquired = true;
        }
    } else {
        morse_tx_radio_stop(app);
    }

    view_dispatcher_switch_to_view(app->view_dispatcher, MorseTxViewTx);
}

/** Runs in the view dispatcher thread, every MORSE_TX_TICK_PERIOD_MS. */
static void morse_tx_tick(void* context) {
    MorseTxApp* app = context;
    if(!app->tx_running || !app->seq) return;

    MorseSequence* seq = app->seq;
    uint32_t elapsed_ms = furi_get_tick() - app->tx_start_tick;
    uint32_t elapsed_us = elapsed_ms * 1000u;

    /* The DMA buffer is refilled ahead of time, so progress is tracked
       against the wall clock rather than against the encoder cursor. */
    bool played_out = elapsed_us >= seq->total_us;
    bool complete = played_out && (subghz_devices_is_async_complete_tx(app->device) ||
                                   elapsed_us > seq->total_us + 500000u);

    /* once per transmission, ask the chip whether it is really in TX */
    if(!app->tx_sampled && elapsed_ms > 300 && app->module_index == 0) {
        app->tx_sampled = true;
        app->tx_marcstate = morse_tx_read_marcstate();
    }

    size_t index = morse_sequence_locate(seq, elapsed_us, app->locate_hint);
    app->locate_hint = index;
    const MorseElement* element = &seq->elements[index];
    bool key_down = !played_out && (element->level != 0);

    morse_tx_sidetone(app, key_down);
    morse_tx_led(app, key_down);

    uint8_t progress =
        played_out ? 100 : (uint8_t)((uint64_t)elapsed_us * 100u / seq->total_us);
    uint32_t elapsed_s = elapsed_ms / 1000u;
    uint8_t repeats_left = app->repeats_left;

    with_view_model(
        app->tx_view,
        MorseTxViewModel * model,
        {
            model->key_down = key_down;
            model->char_index = element->char_index;
            model->sym_index = element->sym_index;
            model->pattern = morse_pattern(model->message[element->char_index]);
            model->elapsed_s = elapsed_s;
            model->progress = progress;
            model->repeats_left = repeats_left;
        },
        true);

    if(!complete) return;

    bool again = (app->repeats_left == 0);
    if(!again && app->repeats_left > 1) {
        app->repeats_left--;
        again = true;
    }

    if(again) {
        subghz_devices_stop_async_tx(app->device);
        app->tx_running = false;
        morse_sequence_rewind(seq);
        app->locate_hint = 0;
        if(subghz_devices_start_async_tx(app->device, morse_tx_yield, app)) {
            app->tx_running = true;
            app->tx_start_tick = furi_get_tick();
        } else {
            morse_tx_stop(app);
            view_dispatcher_switch_to_view(app->view_dispatcher, MorseTxViewMenu);
        }
    } else {
        morse_tx_stop(app);

        FuriString* log = furi_string_alloc();
        furi_string_cat_printf(
            log,
            "module: %s\nfrequency: %lu\nwpm: %u\nmessage: %s\nstarted: yes\n"
            "completed: yes\nelapsed ms: %lu\nmarcstate: 0x%02X (%s)\n",
            morse_tx_module_names[app->module_index],
            (unsigned long)morse_tx_frequencies[app->freq_index],
            morse_tx_speeds[app->speed_index],
            app->message,
            (unsigned long)elapsed_ms,
            app->tx_marcstate,
            app->tx_marcstate == 0x13 ? "TX" :
                (app->tx_marcstate == 0x14 ? "TX_END" :
                 (app->tx_marcstate == 0x01 ? "IDLE" : "other")));
        morse_tx_save_file(MORSE_TX_TXLOG_FILE, furi_string_get_cstr(log));
        furi_string_free(log);

        notification_message(app->notifications, &sequence_success);
        view_dispatcher_switch_to_view(app->view_dispatcher, MorseTxViewMenu);
    }
}

/* --------------------------------------------------------------- tx view */

static void morse_tx_view_draw(Canvas* canvas, void* context) {
    MorseTxViewModel* model = context;
    char buffer[40];

    canvas_clear(canvas);

    if(model->error[0]) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 4, AlignCenter, AlignTop, "Cannot transmit");
        canvas_set_font(canvas, FontSecondary);
        elements_multiline_text_aligned(canvas, 64, 22, AlignCenter, AlignTop, model->error);
        canvas_draw_str_aligned(canvas, 64, 56, AlignCenter, AlignTop, "Press Back");
        return;
    }

    canvas_set_font(canvas, FontSecondary);
    snprintf(
        buffer,
        sizeof(buffer),
        "%lu.%02lu MHz",
        (unsigned long)(model->frequency / 1000000UL),
        (unsigned long)((model->frequency % 1000000UL) / 10000UL));
    canvas_draw_str(canvas, 2, 8, buffer);
    snprintf(buffer, sizeof(buffer), "%u WPM", model->wpm);
    canvas_draw_str(canvas, 66, 8, buffer);

    if(model->key_down) {
        canvas_draw_box(canvas, 106, 0, 22, 10);
        canvas_set_color(canvas, ColorWhite);
        canvas_draw_str(canvas, 109, 8, "AIR");
        canvas_set_color(canvas, ColorBlack);
    } else {
        canvas_draw_frame(canvas, 106, 0, 22, 10);
        canvas_draw_str(canvas, 109, 8, "AIR");
    }

    /* message, scrolled so the character being sent stays visible */
    size_t len = strlen(model->message);
    const size_t window = 20;
    size_t start = 0;
    if(len > window) {
        if(model->char_index > window / 2) start = model->char_index - window / 2;
        if(start + window > len) start = len - window;
    }
    for(size_t i = start; i < len && i < start + window; i++) {
        int x = 3 + (int)(i - start) * 6;
        char glyph[2] = {model->message[i], '\0'};
        if(i == model->char_index && model->active) {
            canvas_draw_box(canvas, x - 1, 14, 7, 11);
            canvas_set_color(canvas, ColorWhite);
            canvas_draw_str(canvas, x, 23, glyph);
            canvas_set_color(canvas, ColorBlack);
        } else {
            canvas_draw_str(canvas, x, 23, glyph);
        }
    }

    /* dots and dashes of the character being sent */
    if(model->pattern) {
        int width = 0;
        for(const char* p = model->pattern; *p; p++) {
            width += ((*p == '-') ? 9 : 3) + 3;
        }
        if(width > 0) width -= 3;
        int x = 64 - width / 2;
        int index = 0;
        for(const char* p = model->pattern; *p; p++) {
            int w = (*p == '-') ? 9 : 3;
            if(index <= model->sym_index) {
                canvas_draw_box(canvas, x, 31, w, 5);
            } else {
                canvas_draw_frame(canvas, x, 31, w, 5);
            }
            x += w + 3;
            index++;
        }
    }

    canvas_draw_frame(canvas, 2, 42, 124, 8);
    int filled = 122 * model->progress / 100;
    if(filled > 0) canvas_draw_box(canvas, 3, 43, filled, 6);

    if(model->repeat_infinite) {
        snprintf(
            buffer,
            sizeof(buffer),
            "%lus/%lus loop",
            (unsigned long)model->elapsed_s,
            (unsigned long)model->total_s);
    } else {
        snprintf(
            buffer,
            sizeof(buffer),
            "%lus/%lus x%u",
            (unsigned long)model->elapsed_s,
            (unsigned long)model->total_s,
            model->repeats_left);
    }
    canvas_draw_str(canvas, 2, 62, buffer);
    canvas_draw_str_aligned(canvas, 126, 62, AlignRight, AlignBottom, "Back = stop");
}

static bool morse_tx_view_input(InputEvent* event, void* context) {
    MorseTxApp* app = context;
    if(event->key == InputKeyBack &&
       (event->type == InputTypeShort || event->type == InputTypeLong)) {
        morse_tx_stop(app);
        view_dispatcher_switch_to_view(app->view_dispatcher, MorseTxViewMenu);
    }
    return true; /* swallow everything else while on air */
}

/* --------------------------------------------------------------- settings */

static void morse_tx_frequency_text(VariableItem* item, uint32_t frequency) {
    char buffer[16];
    snprintf(
        buffer,
        sizeof(buffer),
        "%lu.%02lu",
        (unsigned long)(frequency / 1000000UL),
        (unsigned long)((frequency % 1000000UL) / 10000UL));
    variable_item_set_current_value_text(item, buffer);
}

static void morse_tx_frequency_changed(VariableItem* item) {
    MorseTxApp* app = variable_item_get_context(item);
    app->freq_index = variable_item_get_current_value_index(item);
    morse_tx_frequency_text(item, morse_tx_frequencies[app->freq_index]);
}

static void morse_tx_speed_changed(VariableItem* item) {
    MorseTxApp* app = variable_item_get_context(item);
    app->speed_index = variable_item_get_current_value_index(item);
    char buffer[16];
    snprintf(buffer, sizeof(buffer), "%u wpm", morse_tx_speeds[app->speed_index]);
    variable_item_set_current_value_text(item, buffer);
}

static void morse_tx_repeat_changed(VariableItem* item) {
    MorseTxApp* app = variable_item_get_context(item);
    app->repeat_index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, morse_tx_repeat_names[app->repeat_index]);
}

static void morse_tx_module_changed(VariableItem* item) {
    MorseTxApp* app = variable_item_get_context(item);
    app->module_index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, morse_tx_module_names[app->module_index]);
}

static void morse_tx_sidetone_changed(VariableItem* item) {
    MorseTxApp* app = variable_item_get_context(item);
    app->sidetone = variable_item_get_current_value_index(item) == 1;
    variable_item_set_current_value_text(item, app->sidetone ? "ON" : "OFF");
}

static void morse_tx_power_changed(VariableItem* item) {
    MorseTxApp* app = variable_item_get_context(item);
    app->power_5v = variable_item_get_current_value_index(item) == 1;
    variable_item_set_current_value_text(item, app->power_5v ? "ON" : "OFF");
}

static void morse_tx_settings_build(MorseTxApp* app) {
    VariableItem* item;

    item = variable_item_list_add(
        app->settings,
        "Frequency",
        COUNT_OF(morse_tx_frequencies),
        morse_tx_frequency_changed,
        app);
    variable_item_set_current_value_index(item, app->freq_index);
    morse_tx_frequency_text(item, morse_tx_frequencies[app->freq_index]);

    item = variable_item_list_add(
        app->settings, "Speed", COUNT_OF(morse_tx_speeds), morse_tx_speed_changed, app);
    variable_item_set_current_value_index(item, app->speed_index);
    morse_tx_speed_changed(item);

    item = variable_item_list_add(
        app->settings, "Repeat", COUNT_OF(morse_tx_repeats), morse_tx_repeat_changed, app);
    variable_item_set_current_value_index(item, app->repeat_index);
    variable_item_set_current_value_text(item, morse_tx_repeat_names[app->repeat_index]);

    item = variable_item_list_add(
        app->settings, "Module", COUNT_OF(morse_tx_module_names), morse_tx_module_changed, app);
    variable_item_set_current_value_index(item, app->module_index);
    variable_item_set_current_value_text(item, morse_tx_module_names[app->module_index]);
    app->module_item = item;

    item = variable_item_list_add(app->settings, "5V on GPIO", 2, morse_tx_power_changed, app);
    variable_item_set_current_value_index(item, app->power_5v ? 1 : 0);
    variable_item_set_current_value_text(item, app->power_5v ? "ON" : "OFF");

    item = variable_item_list_add(app->settings, "Sidetone", 2, morse_tx_sidetone_changed, app);
    variable_item_set_current_value_index(item, app->sidetone ? 1 : 0);
    variable_item_set_current_value_text(item, app->sidetone ? "ON" : "OFF");
}

/* ------------------------------------------------------------ diagnostics */

/** Probes the header with and without the 5V rail and reports what happened. */
static void morse_tx_diagnose(MorseTxApp* app) {
    const SubGhzDevice* external = subghz_devices_get_by_name(MORSE_TX_DEVICE_EXTERNAL);
    const SubGhzDevice* internal = subghz_devices_get_by_name(MORSE_TX_DEVICE_INTERNAL);
    bool otg_was_enabled = furi_hal_power_is_otg_enabled();
    bool otg_available = otg_was_enabled;
    bool found_unpowered = false;
    bool found_powered = false;
    uint8_t partnum = 0;
    uint8_t version = 0;
    bool raw_ready = false;

    if(external) {
        if(otg_was_enabled) {
            furi_hal_power_disable_otg();
            furi_delay_ms(250);
        }
        morse_tx_cs_wakeup();
        found_unpowered = morse_tx_raw_probe(&partnum, &version) &&
                          morse_tx_version_is_cc1101(version);

        otg_available = furi_hal_power_enable_otg();
        if(otg_available) {
            furi_delay_ms(250);
            morse_tx_cs_wakeup();
            found_powered =
                morse_tx_raw_probe(&partnum, &version) && morse_tx_version_is_cc1101(version);
        }
        raw_ready = found_powered || found_unpowered;

        if(!otg_was_enabled) furi_hal_power_disable_otg();
    }
    bool charging = furi_hal_power_is_charging();

    FuriString* text = furi_string_alloc();
    furi_string_cat_printf(text, "\e#Radio check\n");
    furi_string_cat_printf(text, "ext driver: %s\n", external ? "loaded" : "MISSING");
    furi_string_cat_printf(text, "int driver: %s\n", internal ? "loaded" : "MISSING");
    furi_string_cat_printf(text, "usb power: %s\n\n", charging ? "connected" : "unplugged");

    if(!external) {
        furi_string_cat_printf(
            text,
            "The cc1101_ext driver is not\n"
            "registered by this firmware,\n"
            "so no external module can be\n"
            "used at all.\n\n");
    } else {
        furi_string_cat_printf(text, "\e#Probe\n");
        furi_string_cat_printf(text, "on 3V3 only: %s\n", found_unpowered ? "FOUND" : "no");
        furi_string_cat_printf(
            text, "with 5V on : %s\n", otg_available ? (found_powered ? "FOUND" : "no") : "n/a");
        furi_string_cat_printf(text, "5V rail: %s\n", otg_available ? "available" : "OFF");
        furi_string_cat_printf(
            text, "raw SPI: part %02X ver %02X%s\n\n", partnum, version, raw_ready ? "" : " (busy)");

        if(!otg_available && charging) {
            furi_string_cat_printf(
                text,
                "\e#USB is blocking 5V\n"
                "While the USB cable is\n"
                "plugged in, the charger\n"
                "cannot run its boost, so\n"
                "pin 1 stays dead. Unplug\n"
                "the cable and run this\n"
                "check again, or power the\n"
                "module from 3V3 (pin 9).\n\n");
        } else if(found_powered && !found_unpowered) {
            furi_string_cat_printf(text, "Module needs 5V: keep\n5V on GPIO = ON.\n\n");
        } else if(!found_powered && !found_unpowered) {
            furi_string_cat_printf(
                text,
                "Nothing answers on the SPI\n"
                "bus (a live CC1101 reports\n"
                "version 0x14). Check that\n"
                "the module is seated on\n"
                "pins 1-8 and that its own\n"
                "power switch or jumper is\n"
                "set.\n\n");
        }
    }

    /* Which header pins does the board actually drive, and is the chip select
       somewhere else than A4? Only pins nothing is driving are safe to drive. */
    furi_string_cat_printf(text, "\e#Header pins\n");
    for(size_t i = 0; i < COUNT_OF(morse_tx_cs_candidates); i++) {
        const GpioPin* pin = morse_tx_cs_candidates[i].pin;
        const char* state = morse_tx_pin_state(pin);
        furi_string_cat_printf(text, "%s: %s", morse_tx_cs_candidates[i].name, state);

        if(strcmp(state, "floating") == 0) {
            uint8_t cs_part = 0;
            uint8_t cs_ver = 0;
            morse_tx_raw_probe_cs(pin, &cs_part, &cs_ver);
            furi_string_cat_printf(text, " ver %02X", cs_ver);
            if(cs_ver == 0x14 || cs_ver == 0x04 || cs_ver == 0x17) {
                furi_string_cat_printf(text, " <<< CC1101 HERE");
            }
        }
        furi_string_cat_printf(text, "\n");
    }
    furi_string_cat_printf(text, "\n");

    furi_string_cat_printf(
        text,
        "\e#Expected wiring\n"
        "GDO0 pin 2 (A7)\n"
        "MISO pin 3 (A6)\n"
        "CS   pin 4 (A4)\n"
        "SCK  pin 5 (B3)\n"
        "MOSI pin 6 (B2)\n"
        "GND  pin 8/11/18\n"
        "VCC  pin 9 (3V3) or pin 1 (5V)");

    if(found_powered || found_unpowered) {
        app->module_index = 0;
        app->power_5v = found_powered && !found_unpowered ? true : app->power_5v;
        if(app->module_item) {
            variable_item_set_current_value_index(app->module_item, app->module_index);
            variable_item_set_current_value_text(
                app->module_item, morse_tx_module_names[app->module_index]);
        }
    }

    morse_tx_save_report(furi_string_get_cstr(text));

    widget_reset(app->diag);
    widget_add_text_scroll_element(app->diag, 0, 0, 128, 64, furi_string_get_cstr(text));
    furi_string_free(text);

    view_dispatcher_switch_to_view(app->view_dispatcher, MorseTxViewDiag);
}

/* ------------------------------------------------------------- navigation */

static void morse_tx_message_done(void* context) {
    MorseTxApp* app = context;
    for(size_t i = 0; app->message[i]; i++) {
        if(app->message[i] >= 'a' && app->message[i] <= 'z') {
            app->message[i] = (char)(app->message[i] - 'a' + 'A');
        }
    }
    view_dispatcher_switch_to_view(app->view_dispatcher, MorseTxViewMenu);
}

static void morse_tx_menu_callback(void* context, uint32_t index) {
    MorseTxApp* app = context;
    switch(index) {
    case MorseTxMenuTransmit:
        morse_tx_start(app);
        break;
    case MorseTxMenuMessage:
        view_dispatcher_switch_to_view(app->view_dispatcher, MorseTxViewMessage);
        break;
    case MorseTxMenuSettings:
        view_dispatcher_switch_to_view(app->view_dispatcher, MorseTxViewSettings);
        break;
    case MorseTxMenuDiag:
        morse_tx_diagnose(app);
        break;
    case MorseTxMenuAbout:
        view_dispatcher_switch_to_view(app->view_dispatcher, MorseTxViewAbout);
        break;
    default:
        break;
    }
}

static uint32_t morse_tx_exit(void* context) {
    UNUSED(context);
    return VIEW_NONE;
}

static uint32_t morse_tx_back_to_menu(void* context) {
    UNUSED(context);
    return MorseTxViewMenu;
}

/* --------------------------------------------------------- app life cycle */

static MorseTxApp* morse_tx_app_alloc(void) {
    MorseTxApp* app = malloc(sizeof(MorseTxApp));
    memset(app, 0, sizeof(MorseTxApp));

    subghz_devices_init();

    strncpy(app->message, "CQ CQ DE FLIPPER", MORSE_MESSAGE_MAX);
    app->freq_index = 9; /* 433.92 MHz */
    app->speed_index = 6; /* 20 WPM */
    app->repeat_index = 0; /* send once */
    app->power_5v = true;
    app->sidetone = true;

    /* A saved choice wins; without one, prefer the external module and fall
       back to the built-in radio. */
    if(!morse_tx_settings_load(app)) {
        app->module_index = morse_tx_external_present() ? 0 : 1;
    }

    app->gui = furi_record_open(RECORD_GUI);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);

    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_tick_event_callback(
        app->view_dispatcher, morse_tx_tick, MORSE_TX_TICK_PERIOD_MS);

    app->submenu = submenu_alloc();
    submenu_set_header(app->submenu, "Morse TX");
    submenu_add_item(app->submenu, "Transmit", MorseTxMenuTransmit, morse_tx_menu_callback, app);
    submenu_add_item(app->submenu, "Message", MorseTxMenuMessage, morse_tx_menu_callback, app);
    submenu_add_item(app->submenu, "Settings", MorseTxMenuSettings, morse_tx_menu_callback, app);
    submenu_add_item(app->submenu, "Radio check", MorseTxMenuDiag, morse_tx_menu_callback, app);
    submenu_add_item(app->submenu, "About", MorseTxMenuAbout, morse_tx_menu_callback, app);
    view_set_previous_callback(submenu_get_view(app->submenu), morse_tx_exit);
    view_dispatcher_add_view(app->view_dispatcher, MorseTxViewMenu, submenu_get_view(app->submenu));

    app->settings = variable_item_list_alloc();
    morse_tx_settings_build(app);
    view_set_previous_callback(variable_item_list_get_view(app->settings), morse_tx_back_to_menu);
    view_dispatcher_add_view(
        app->view_dispatcher, MorseTxViewSettings, variable_item_list_get_view(app->settings));

    app->text_input = text_input_alloc();
    text_input_set_header_text(app->text_input, "Message to send");
    text_input_set_result_callback(
        app->text_input, morse_tx_message_done, app, app->message, MORSE_MESSAGE_MAX + 1, false);
    view_set_previous_callback(text_input_get_view(app->text_input), morse_tx_back_to_menu);
    view_dispatcher_add_view(
        app->view_dispatcher, MorseTxViewMessage, text_input_get_view(app->text_input));

    app->about = widget_alloc();
    widget_add_text_scroll_element(
        app->about,
        0,
        0,
        128,
        64,
        "\e#Morse TX\n"
        "Keys an OOK carrier with the\n"
        "message in Morse code.\n\n"
        "\e#Radio\n"
        "The external module is probed\n"
        "at startup; without it the\n"
        "built-in radio is used.\n"
        "Override in Settings/Module.\n\n"
        "\e#External CC1101\n"
        "GDO0 - pin 2 (A7)\n"
        "MISO - pin 3 (A6)\n"
        "CS   - pin 4 (A4)\n"
        "SCK  - pin 5 (B3)\n"
        "MOSI - pin 6 (B2)\n"
        "GND  - pin 8/11/18\n"
        "VCC  - 3V3 (pin 9) or 5V\n"
        "(pin 1, needs 5V on GPIO)\n\n"
        "\e#Timing\n"
        "unit = 1200 ms / WPM\n"
        "dot 1, dash 3, letter gap 3,\n"
        "word gap 7 units.\n\n"
        "\e#Legal\n"
        "This transmits a real carrier.\n"
        "Respect the power, duty cycle\n"
        "and licensing rules of your\n"
        "country.");
    view_set_previous_callback(widget_get_view(app->about), morse_tx_back_to_menu);
    view_dispatcher_add_view(app->view_dispatcher, MorseTxViewAbout, widget_get_view(app->about));

    app->diag = widget_alloc();
    view_set_previous_callback(widget_get_view(app->diag), morse_tx_back_to_menu);
    view_dispatcher_add_view(app->view_dispatcher, MorseTxViewDiag, widget_get_view(app->diag));

    app->tx_view = view_alloc();
    view_set_context(app->tx_view, app);
    view_allocate_model(app->tx_view, ViewModelTypeLocking, sizeof(MorseTxViewModel));
    view_set_draw_callback(app->tx_view, morse_tx_view_draw);
    view_set_input_callback(app->tx_view, morse_tx_view_input);
    view_dispatcher_add_view(app->view_dispatcher, MorseTxViewTx, app->tx_view);

    view_dispatcher_switch_to_view(app->view_dispatcher, MorseTxViewMenu);
    return app;
}

static void morse_tx_app_free(MorseTxApp* app) {
    morse_tx_stop(app);
    morse_tx_settings_save(app);
    subghz_devices_deinit();

    view_dispatcher_remove_view(app->view_dispatcher, MorseTxViewMenu);
    view_dispatcher_remove_view(app->view_dispatcher, MorseTxViewSettings);
    view_dispatcher_remove_view(app->view_dispatcher, MorseTxViewMessage);
    view_dispatcher_remove_view(app->view_dispatcher, MorseTxViewAbout);
    view_dispatcher_remove_view(app->view_dispatcher, MorseTxViewDiag);
    view_dispatcher_remove_view(app->view_dispatcher, MorseTxViewTx);

    submenu_free(app->submenu);
    variable_item_list_free(app->settings);
    text_input_free(app->text_input);
    widget_free(app->about);
    widget_free(app->diag);
    view_free(app->tx_view);
    view_dispatcher_free(app->view_dispatcher);

    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_GUI);
    free(app);
}

int32_t morse_tx_app(void* p) {
    UNUSED(p);
    MorseTxApp* app = morse_tx_app_alloc();
    view_dispatcher_run(app->view_dispatcher);
    morse_tx_app_free(app);
    return 0;
}
