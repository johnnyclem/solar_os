#include "solar_os_cardkb2.h"

static const solar_os_expansion_binding_spec_t binding_specs[] = {
    {
        .key = "uart",
        .value_hint = "bus",
        .kind = SOLAR_OS_EXPANSION_BINDING_UART_PORT,
        .required = true,
    },
};

const solar_os_expansion_driver_t solar_os_cardkb2_expansion_driver = {
    .name = "cardkb2",
    .summary = "UART QWERTY keyboard",
    .required_capabilities = SOLAR_OS_BOARD_CAP_EXPANSION_UART,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = solar_os_cardkb2_attach,
    .detach = solar_os_cardkb2_detach,
};
