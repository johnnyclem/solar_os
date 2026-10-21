#include "solar_os_cardkb2_codec.h"

#include <string.h>

#include "solar_os_keys.h"

#define CARDKB2_FRAME_HEAD 0xaaU
#define CARDKB2_DATA_LEN 0x03U
#define CARDKB2_STATE_PRESS 0x01U
#define CARDKB2_STATE_RELEASE 0x02U
#define CARDKB2_KEY_AA 22U
#define CARDKB2_KEY_FN 33U
#define CARDKB2_KEY_SYM 34U

typedef struct {
    uint8_t normal;
    uint8_t shifted;
    uint8_t symbol;
} cardkb2_glyph_t;

/* Index = row * 11 + column. Blank entries produce no character. */
static const cardkb2_glyph_t cardkb2_glyphs[SOLAR_OS_CARDKB2_KEY_COUNT] = {
    {'1', '1', '!'}, {'2', '2', '@'}, {'3', '3', '#'}, {'4', '4', '$'},
    {'5', '5', '%'}, {'6', '6', '^'}, {'7', '7', '&'}, {'8', '8', '*'},
    {'9', '9', '('}, {'0', '0', ')'}, {0, 0, 0},
    {'q', 'Q', '~'}, {'w', 'W', '`'}, {'e', 'E', '?'}, {'r', 'R', '\\'},
    {'t', 'T', '/'}, {'y', 'Y', '|'}, {'u', 'U', '_'}, {'i', 'I', '-'},
    {'o', 'O', '+'}, {'p', 'P', '='}, {'\b', '\b', '\b'},
    {0, 0, 0},
    {'a', 'A', '{'}, {'s', 'S', '}'}, {'d', 'D', '^'}, {'f', 'F', '['},
    {'g', 'G', ']'}, {'h', 'H', '"'}, {'j', 'J', '\''}, {'k', 'K', ';'},
    {'l', 'L', ':'}, {'\n', '\n', '\n'},
    {0, 0, 0}, {0, 0, 0},
    {'z', 'Z', 'Z'}, {'x', 'X', 'X'}, {'c', 'C', 'C'}, {'v', 'V', '<'},
    {'b', 'B', '>'}, {'n', 'N', ','}, {'m', 'M', '.'}, {' ', ' ', ' '},
    {0, 0, 0},
};

static uint8_t cardkb2_fn_key(uint8_t key_id)
{
    switch (key_id) {
    case 0U:
        return SOLAR_OS_KEY_ESCAPE;
    case 25U:
        return SOLAR_OS_KEY_UP;
    case 35U:
        return SOLAR_OS_KEY_LEFT;
    case 36U:
        return SOLAR_OS_KEY_DOWN;
    case 37U:
        return SOLAR_OS_KEY_RIGHT;
    default:
        return 0;
    }
}

static uint8_t cardkb2_translate(const solar_os_cardkb2_state_t *state, uint8_t key_id)
{
    const cardkb2_glyph_t *glyph;

    if (state->fn_down && state->sym_down) {
        return 0;
    }
    if (state->fn_down) {
        return cardkb2_fn_key(key_id);
    }
    glyph = &cardkb2_glyphs[key_id];
    if (state->sym_latched) {
        return glyph->symbol;
    }
    if (state->caps_lock || state->aa_down || state->caps_oneshot) {
        return glyph->shifted;
    }
    return glyph->normal;
}

static void cardkb2_shift_frame(solar_os_cardkb2_parser_t *parser)
{
    uint8_t start = 1U;

    while (start < parser->length && parser->frame[start] != CARDKB2_FRAME_HEAD) {
        start++;
    }
    if (start >= parser->length) {
        parser->length = 0;
        return;
    }
    memmove(parser->frame, parser->frame + start, (size_t)(parser->length - start));
    parser->length = (uint8_t)(parser->length - start);
}

bool solar_os_cardkb2_parse(solar_os_cardkb2_parser_t *parser,
                            uint8_t byte,
                            uint8_t *key_id,
                            uint8_t *key_state)
{
    if (parser == NULL || key_id == NULL || key_state == NULL) {
        return false;
    }
    if (parser->length == 0 && byte != CARDKB2_FRAME_HEAD) {
        return false;
    }
    if (parser->length >= SOLAR_OS_CARDKB2_FRAME_LEN) {
        parser->length = 0;
    }
    parser->frame[parser->length++] = byte;

    while (parser->length >= SOLAR_OS_CARDKB2_FRAME_LEN) {
        const uint8_t data_len = parser->frame[1];
        const uint8_t id = parser->frame[2];
        const uint8_t state = parser->frame[3];
        const uint8_t checksum = parser->frame[4];
        const uint8_t expect = (uint8_t)((data_len + id + state) & 0xffU);

        if (parser->frame[0] == CARDKB2_FRAME_HEAD &&
            data_len == CARDKB2_DATA_LEN &&
            id < SOLAR_OS_CARDKB2_KEY_COUNT &&
            (state == CARDKB2_STATE_PRESS || state == CARDKB2_STATE_RELEASE) &&
            checksum == expect) {
            *key_id = id;
            *key_state = state;
            parser->length = 0;
            return true;
        }
        cardkb2_shift_frame(parser);
    }
    return false;
}

bool solar_os_cardkb2_apply(solar_os_cardkb2_state_t *state,
                            uint8_t key_id,
                            bool pressed,
                            uint32_t now_ms,
                            solar_os_cardkb2_output_t *output)
{
    uint8_t key;

    if (state == NULL || output == NULL || key_id >= SOLAR_OS_CARDKB2_KEY_COUNT) {
        return false;
    }
    memset(output, 0, sizeof(*output));

    if (key_id == CARDKB2_KEY_FN) {
        state->fn_down = pressed;
        return true;
    }
    if (key_id == CARDKB2_KEY_SYM) {
        if (pressed) {
            state->sym_down = true;
            state->sym_chord = state->fn_down;
            return true;
        }
        state->sym_down = false;
        if (!state->sym_chord && !state->fn_down) {
            state->sym_latched = !state->sym_latched;
        }
        state->sym_chord = false;
        return true;
    }
    if (key_id == CARDKB2_KEY_AA) {
        if (state->sym_latched) {
            return true;
        }
        if (pressed) {
            state->aa_down = true;
            state->aa_typed = false;
            return true;
        }
        state->aa_down = false;
        if (state->aa_typed) {
            return true;
        }
        if (state->aa_release_valid &&
            (uint32_t)(now_ms - state->aa_release_ms) <= SOLAR_OS_CARDKB2_DOUBLE_CLICK_MS) {
            state->caps_lock = !state->caps_lock;
            state->caps_oneshot = false;
            state->aa_release_valid = false;
            return true;
        }
        state->aa_release_ms = now_ms;
        state->aa_release_valid = true;
        if (state->caps_lock) {
            state->caps_lock = false;
            return true;
        }
        state->caps_oneshot = true;
        return true;
    }

    if (!pressed) {
        if (state->down[key_id] && state->emitted[key_id] != 0) {
            output->emit = true;
            output->pressed = false;
            output->physical = (uint16_t)key_id + 1U;
            output->key = state->emitted[key_id];
        }
        state->down[key_id] = false;
        state->emitted[key_id] = 0;
        return true;
    }
    if (state->down[key_id]) {
        return true;
    }

    key = cardkb2_translate(state, key_id);
    state->down[key_id] = true;
    if (key == 0) {
        return true;
    }
    state->emitted[key_id] = key;
    output->emit = true;
    output->pressed = true;
    output->physical = (uint16_t)key_id + 1U;
    output->key = key;
    if (state->aa_down) {
        state->aa_typed = true;
    }
    if (state->caps_oneshot && key >= 'A' && key <= 'Z') {
        state->caps_oneshot = false;
    }
    return true;
}
