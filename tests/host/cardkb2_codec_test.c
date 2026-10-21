#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_cardkb2_codec.h"
#include "solar_os_keys.h"

static void feed_frame(solar_os_cardkb2_parser_t *parser,
                       const uint8_t *frame,
                       uint8_t *key_id,
                       uint8_t *key_state)
{
    bool parsed = false;
    for (size_t i = 0; i < SOLAR_OS_CARDKB2_FRAME_LEN; i++) {
        parsed = solar_os_cardkb2_parse(parser, frame[i], key_id, key_state);
    }
    assert(parsed);
}

static solar_os_cardkb2_output_t press(solar_os_cardkb2_state_t *state,
                                       uint8_t key_id,
                                       uint32_t now_ms)
{
    solar_os_cardkb2_output_t output;
    assert(solar_os_cardkb2_apply(state, key_id, true, now_ms, &output));
    return output;
}

static solar_os_cardkb2_output_t release(solar_os_cardkb2_state_t *state,
                                         uint8_t key_id,
                                         uint32_t now_ms)
{
    solar_os_cardkb2_output_t output;
    assert(solar_os_cardkb2_apply(state, key_id, false, now_ms, &output));
    return output;
}

static void click(solar_os_cardkb2_state_t *state, uint8_t key_id, uint32_t now_ms)
{
    (void)press(state, key_id, now_ms);
    (void)release(state, key_id, now_ms);
}

int main(void)
{
    solar_os_cardkb2_parser_t parser;
    solar_os_cardkb2_state_t state;
    solar_os_cardkb2_output_t output;
    uint8_t key_id = 0xffU;
    uint8_t key_state = 0;
    const uint8_t press_one[] = {0xaaU, 0x03U, 0x00U, 0x01U, 0x04U};
    const uint8_t release_one[] = {0xaaU, 0x03U, 0x00U, 0x02U, 0x05U};
    const uint8_t bad_sum[] = {0xaaU, 0x03U, 0x00U, 0x01U, 0x00U};

    memset(&parser, 0, sizeof(parser));
    memset(&state, 0, sizeof(state));

    assert(!solar_os_cardkb2_parse(NULL, 0xaaU, &key_id, &key_state));
    assert(!solar_os_cardkb2_parse(&parser, 'q', &key_id, &key_state));
    feed_frame(&parser, press_one, &key_id, &key_state);
    assert(key_id == 0 && key_state == 0x01U);
    feed_frame(&parser, release_one, &key_id, &key_state);
    assert(key_id == 0 && key_state == 0x02U);

    memset(&parser, 0, sizeof(parser));
    for (size_t i = 0; i < sizeof(bad_sum); i++) {
        assert(!solar_os_cardkb2_parse(&parser, bad_sum[i], &key_id, &key_state));
    }
    assert(!solar_os_cardkb2_parse(&parser, 0x00U, &key_id, &key_state));
    feed_frame(&parser, press_one, &key_id, &key_state);
    assert(key_id == 0 && key_state == 0x01U);

    output = press(&state, 11U, 0);
    assert(output.emit && output.pressed && output.key == 'q' && output.physical == 12U);
    output = press(&state, 11U, 10);
    assert(!output.emit);
    output = release(&state, 11U, 20);
    assert(output.emit && !output.pressed && output.key == 'q');

    output = press(&state, 22U, 100);
    assert(!output.emit);
    output = press(&state, 11U, 120);
    assert(output.emit && output.key == 'Q');
    output = release(&state, 11U, 140);
    output = release(&state, 22U, 160);
    assert(!output.emit);
    assert(!state.caps_lock && !state.caps_oneshot);

    click(&state, 22U, 1000);
    output = press(&state, 12U, 1100);
    assert(output.emit && output.key == 'W');
    output = release(&state, 12U, 1110);
    output = press(&state, 12U, 1200);
    assert(output.emit && output.key == 'w');
    output = release(&state, 12U, 1210);

    click(&state, 22U, 2000);
    click(&state, 22U, 2100);
    assert(state.caps_lock);
    output = press(&state, 13U, 2200);
    assert(output.emit && output.key == 'E');
    output = release(&state, 13U, 2210);
    click(&state, 22U, 3000);
    assert(!state.caps_lock);

    click(&state, 34U, 4000);
    output = press(&state, 11U, 4100);
    assert(output.emit && output.key == '~');
    output = release(&state, 11U, 4110);
    click(&state, 34U, 4200);
    output = press(&state, 11U, 4300);
    assert(output.emit && output.key == 'q');
    output = release(&state, 11U, 4310);

    output = press(&state, 33U, 5000);
    output = press(&state, 25U, 5010);
    assert(output.emit && output.key == SOLAR_OS_KEY_UP);
    output = release(&state, 25U, 5020);
    output = press(&state, 0U, 5030);
    assert(output.emit && output.key == SOLAR_OS_KEY_ESCAPE);
    output = release(&state, 0U, 5040);
    output = release(&state, 33U, 5050);

    output = press(&state, 21U, 6000);
    assert(output.emit && output.key == '\b');
    output = release(&state, 21U, 6010);
    output = press(&state, 32U, 6020);
    assert(output.emit && output.key == '\n');
    output = release(&state, 32U, 6030);

    assert(!solar_os_cardkb2_apply(&state, 44U, true, 0, &output));

    puts("CardKB2 codec tests: ok");
    return 0;
}
