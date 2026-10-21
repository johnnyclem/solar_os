#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SOLAR_OS_CARDKB2_BAUD_RATE 115200U
#define SOLAR_OS_CARDKB2_FRAME_LEN 5U
#define SOLAR_OS_CARDKB2_KEY_COUNT 44U
#define SOLAR_OS_CARDKB2_DOUBLE_CLICK_MS 400U

typedef struct {
    uint8_t frame[SOLAR_OS_CARDKB2_FRAME_LEN];
    uint8_t length;
} solar_os_cardkb2_parser_t;

typedef struct {
    bool fn_down;
    bool sym_down;
    bool sym_chord;
    bool sym_latched;
    bool aa_down;
    bool aa_typed;
    bool caps_lock;
    bool caps_oneshot;
    bool aa_release_valid;
    uint32_t aa_release_ms;
    bool down[SOLAR_OS_CARDKB2_KEY_COUNT];
    uint8_t emitted[SOLAR_OS_CARDKB2_KEY_COUNT];
} solar_os_cardkb2_state_t;

typedef struct {
    bool emit;
    bool pressed;
    uint16_t physical;
    uint8_t key;
} solar_os_cardkb2_output_t;

/* CardKB2 UART frame: AA 03 KEY_ID STATE checksum. STATE is 1 press, 2 release. */
bool solar_os_cardkb2_parse(solar_os_cardkb2_parser_t *parser,
                            uint8_t byte,
                            uint8_t *key_id,
                            uint8_t *key_state);

bool solar_os_cardkb2_apply(solar_os_cardkb2_state_t *state,
                            uint8_t key_id,
                            bool pressed,
                            uint32_t now_ms,
                            solar_os_cardkb2_output_t *output);
