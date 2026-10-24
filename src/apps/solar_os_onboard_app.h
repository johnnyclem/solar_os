#pragma once

#include <stdbool.h>

#include "solar_os.h"

/*
 * `onboard`: the guided path from a fresh flash to a working agent. It joins
 * Wi-Fi, picks a provider, takes the API key, checks the server, picks a
 * model, sends one test request, and hands off to `agent`.
 *
 *   onboard              guided setup; re-running it re-checks a saved setup
 *   onboard check        the checks only, no prompts (`agent doctor`)
 *   onboard --for-agent  launched by `agent` when it cannot start yet
 */
extern const solar_os_app_t solar_os_onboard_app;

/* True while the agent has no endpoint or no model configured. */
bool solar_os_onboard_needed(void);
