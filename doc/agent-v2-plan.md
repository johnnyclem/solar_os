# Agent v2 plan: usable, configurable, always-on

Scope of this fork's agent work. Goal: `agent` is the one command a person
needs; a first-run `onboard` gets Wi-Fi, endpoint, key, and model working in
under two minutes; the agent talks to one Anthropic-compatible endpoint; and an
always-on `agentd` job keeps a persistent conversation alive so work can be
resumed from the TUI, a schedule, or an HTTP request.

Everything below builds on the existing `service.agent` / `app.agent` stack
(`src/services/solar_os_agent*.c`, `src/apps/solar_os_agent_app.c`,
`src/shell/solar_os_shell_agent.c`). We are not rewriting it. The provider
abstraction, tool registry, policy enforcement, conversation store, and
streaming event model are all good and stay.

## Why `agent ask` fails today

Three independent causes, all in the launch path, none in the model call:

1. **Argument limits.** `agent ask PROMPT...` launches the app through
   `solar_os_context_request_launch`, which copies argv into a fixed
   `argv[SOLAR_OS_APP_ARG_MAX=8][SOLAR_OS_APP_ARG_LEN=160]` table
   (`src/solar_os.h:10`, `src/solar_os.c:290`). `agent` and `ask` use two
   slots, so any prompt longer than **six words** fails with
   `agent: launch failed: invalid argument`. The app then re-joins the words
   with spaces (`agent_app_build_prompt`), so the split was pointless anyway.
2. **Shell quoting.** The shell tokenizer treats `'` as a quote character
   (`src/shell/solar_os_shell_parse.c:55`). `agent ask what's my IP` is an
   "unterminated single quote" error before the agent is ever involved.
3. **Preconditions with no path forward.** The app exits immediately unless
   Wi-Fi has an IP and endpoint and model are set, printing one line and
   returning to the shell. There is no hint about `agent config` and no way to
   fix it from inside the agent.

Separately, ask mode is a dead end by design: the answer stays on screen until
`Esc`, nothing is saved, and you cannot follow up. That is the opposite of what
a Claude Code style `claude "prompt"` does.

## Target experience

```text
> agent
agent: not configured. Starting onboard...
  Wi-Fi .......... connected (HomeNet, 192.168.1.42)
  Endpoint ....... https://api.anthropic.com/v1/messages
  API key ........ ******************************** (saved)
  Reachability ... DNS ok, TLS ok, GET /v1/models 200 in 612 ms
  Model .......... claude-opus-5-5  [1] opus-5-5 [2] sonnet-5-5 [3] haiku-5-5
  Test request ... ok (14 tokens)
Done. Run `agent` to chat, `agent doctor` to re-check.

> agent
agent (claude-opus-5-5) resumed conversation 2 — "battery drain on sleep"
johnny> what did we conclude about the ADC path?
agent: ...

johnny> /new
johnny> /model claude-haiku-5-5
johnny> /tools readonly
johnny> /quit
```

```text
> agent ask summarize the last three inbox messages
```

opens the same chat view, submits the prompt, streams the answer, and leaves
you at the prompt so you can keep going. The turn is saved like any other.

## Phases

Ordered by value per day of work. Each phase is one PR and is shippable alone.

### Phase 0: fix `ask` and the launch path (half a day)

- Drop the `AGENT_APP_MODE_ASK` code path. `agent ask ...` becomes "open chat,
  auto-submit this prompt". Follow-ups work, the turn is saved, `Esc` exits.
- Pass the prompt out of band instead of through argv: the shell command joins
  `argv[2..]` into a PSRAM buffer handed to the service as a pending prompt
  (`solar_os_agent_set_pending_prompt()`), then launches with `argc == 2`.
  The app takes the pending prompt in `start()`. This removes the six-word
  limit without touching `SOLAR_OS_APP_ARG_MAX`.
- Shell quoting stays as is (changing the tokenizer is a global behavior
  change). The `agent ask` error path says: "wrap the prompt in double quotes
  when it contains an apostrophe".
- When Wi-Fi or config is missing, `agent` prints the specific missing item
  and, on a display shell, offers to launch `onboard` (Phase 2) instead of
  dumping back to the prompt.
- Files: `src/shell/solar_os_shell_agent.c`, `src/apps/solar_os_agent_app.c`,
  `src/services/solar_os_agent.{c,h}`, `doc/manual/agent.md`,
  `doc/manual/commands.md` row for `agent`.

### Phase 1: Anthropic Messages provider (3 to 4 days)

New file `src/services/solar_os_agent_anthropic.c`, registered beside
`solar_os_agent_openai_provider`. Provider selection is by endpoint path, same
convention the OpenAI adapter uses for `/responses`: a URL ending in
`/messages` selects the Anthropic wire protocol. Everything else keeps the
current behavior, so OpenAI-compatible local servers keep working.

Wire contract (API version `2026-10-21`):

- Headers: `x-api-key: KEY`, `anthropic-version: 2026-10-21`,
  `content-type: application/json`. No `Authorization: Bearer`.
- Body: `model`, `max_tokens`, `stream: true`, `system` (the existing
  SolarOS instructions string), `messages`, `tools`. Tool descriptors map
  one-to-one from the registry: `name`, `description`, `input_schema`
  (the existing `parameters_json`). No `thinking` field is sent, which gives
  adaptive thinking on Claude 5.x models and no thinking on older ones. The
  `reasoning` setting maps to `output_config.effort` (`none` omits it,
  `minimal` becomes `low`).
- `max_tokens` is 4096. The device captures at most 16 KiB of assistant text
  per turn and holds the request body in a 32 KiB PSRAM buffer, so a larger
  value buys nothing on this hardware.
- Streaming parser handles `message_start` (input tokens),
  `content_block_start` with `tool_use` (id and name),
  `content_block_delta` with `text_delta` and `input_json_delta`
  (accumulate `partial_json` into `tool_arguments`), `message_delta`
  (`stop_reason`, output tokens), `message_stop`, and `event: error`.
  `stop_reason: "refusal"` is reported as a provider error with the
  `stop_details.category` text, not as an empty reply.
- Resume mode is `LOCAL_HISTORY`. There is no server-side continuation ID.
  History comes from the conversation store exactly as the Chat Completions
  path already does.
- Tool continuation within one request builds
  `[...history, user(prompt), assistant([tool_use]), user([tool_result])]`.
  Earlier tool exchanges in the same request are folded into the `system`
  string as "Earlier tool results in this request", which is what the Chat
  Completions path does today. This keeps the body bounded no matter how many
  tools the model chains.
- Parallel tool calls: the service already rejects more than one tool call
  per turn. The adapter sends `tool_choice: {"type":"auto",
  "disable_parallel_tool_use": true}` so the model is told, not surprised.

Tests: factor the SSE handling as a pure `agent_anthropic_feed()` over a
line buffer so `tests/host/agent_anthropic_test.c` can replay canned event
streams (text, tool call with chunked `partial_json`, usage, refusal, error)
without HTTP. Add the target to `tests/host/Makefile`. Add the source to
`[packages.service_agent]` in `packages/solar_os_packages.toml`.

Defaults change in this phase: `agent config endpoint` defaults to
`https://api.anthropic.com/v1/messages`, `model` to `claude-opus-5-5`.
`agent status` shows `Provider: anthropic`.

### Phase 2: `onboard` (3 days)

A foreground app, not a plain shell command. Shell commands cannot block for
input (`solar_os_shell_io.h` has no line reader), and the existing Wi-Fi TUI
(`src/shell/solar_os_shell_wifi_tui.c`) is the precedent. It must work on port
shells (UART, USB CDC, Telnet) as well as the display, so the input handling
is a small line-reader state machine in the app's event callback, same as the
agent chat input, with `*` echo for secrets.

Steps, each skippable when already satisfied and each re-runnable:

1. **Wi-Fi.** If no IP: scan, number the networks, read a choice and a
   password, `solar_os_wifi_connect`, wait for `has_ip` with a 20 s deadline.
   Existing saved profiles are offered first.
2. **Endpoint.** Default `https://api.anthropic.com/v1/messages`, Enter
   accepts. Validated with the existing `agent_endpoint_valid` rules.
3. **API key.** Read masked, stored through `solar_os_agent_set_api_key` (NVS
   `agent` namespace, same as today; NVS is not encrypted and the manual
   already says so).
4. **Reachability check.** One `GET https://api.anthropic.com/v1/models`
   with the key through the shared HTTP client. This proves DNS, TLS trust
   bundle, routing, and key validity in a single round trip, and the response
   doubles as the model picker. Failures are reported by stage: "no DNS",
   "TLS handshake failed", "HTTP 401: key rejected", "HTTP 429".
5. **Model.** Pick from the fetched list, filtered to `claude-*`, with
   `claude-opus-5-5` preselected. Non-Anthropic endpoints skip the fetch and
   prompt for a name.
6. **Smoke test.** One `POST /v1/messages` with `max_tokens: 16` and
   "reply with ok". Reports latency and tokens.
7. **Always-on.** Yes/No: append `job start agentd` to the selected
   `.shell/startup` script (Phase 4). Same mechanism `io` uses for bus
   autostart.

Scriptable forms for headless boards and tests, no prompts:

```text
onboard wifi SSID [PASSWORD]
onboard key KEY
onboard check            # steps 4 and 6 only; also exposed as `agent doctor`
onboard model NAME
```

Files: `src/apps/solar_os_onboard_app.c`, `src/shell/solar_os_shell_onboard.c`,
new `[packages.app_onboard]` depending on `service_agent` and `service_wifi`,
enabled in the same flavors as `app_agent`, `doc/manual/onboard.md`, a
`commands.md` row. Must pass `scripts/check_foreground_app_state.py` (cold
state behind a pointer, no mutable statics).

### Phase 3: chat UI, Claude Code style (2 days)

All inside `src/apps/solar_os_agent_app.c` and the shell command.

- **Bare `agent` resumes** the most recent conversation. `agent new` starts a
  fresh one. This flips the current default and is the single biggest
  "always on" feel for the cost of one `conversations_list` call.
- **Slash commands** in the input line, dispatched before a prompt is sent:
  `/new`, `/list`, `/resume N`, `/model NAME`, `/tools off|readonly|confirm|all`,
  `/status`, `/help`, `/quit`. Config changes call the existing setters, so
  nothing new is persisted anywhere else. You never have to leave the chat to
  fix a setting.
- **Footer** gains a state word (`idle`, `thinking`, `tool: storage_list`,
  `waiting y/N`) next to the token counts. Narrow displays keep the
  abbreviations.
- **Tool lines** collapse to one line each with the outcome (`tool
  storage_stat ok`), and the confirmation prompt shows arguments on one
  bounded line.
- Remove the ask-only "Press Esc to return" tail (gone with Phase 0).
- Keep: streaming deltas, Page Up/Down, `Esc` cancel, 30 s confirm deadline,
  the 1023-byte prompt limit.

### Phase 4: `agentd`, the always-on job (4 to 5 days)

Honest framing first. An ESP32 cannot run a thinking loop in the background:
the service allows one request at a time, every request has a 90 s deadline,
a background worker must leave the 32 KiB internal reserve alone, and there is
nobody present to answer `Allow once? [y/N]`. So "always on" here means:

- a **persistent default conversation** that survives reboot and is shared by
  the TUI, the daemon, and any trigger;
- a **request queue** so prompts can come from somewhere other than the
  keyboard;
- **delivery of answers** to places you will see them later.

Design:

- `src/jobs/solar_os_agentd_job.c`, registered in the job registry, package
  `job_agentd` depending on `service_agent`. Worker stack 16 KiB, role
  background, so admission can park it as `waiting` under pressure instead of
  starving a foreground launch.
- State machine: `idle` -> wait for `wifi.has_ip` -> pop queue -> run
  through `solar_os_agent_run` with the daemon's own event handler -> publish
  -> back to `idle`. If the foreground app holds the service
  (`ESP_ERR_INVALID_STATE`), retry with backoff; the TUI always wins.
- Queue: a small PSRAM ring (8 entries x 1 KiB prompt) plus a persisted
  schedule file under `.solar/agent/schedule`. Sources in v1:
  - `agent send PROMPT...` from any shell (uses the Phase 0 out-of-band
    prompt path).
  - `agent schedule add "every 30m" PROMPT...` / `list` / `remove N`.
    Intervals only; no cron grammar.
  - `POST /agent` on the existing `httpd` job when it is running, body is the
    prompt text, response is the reply. Bearer check against a separate
    `agent config http-token` so the LLM key is never the inbound secret.
- Delivery: every daemon turn is committed to the default conversation (so
  `agent` shows it) and a 256-byte summary is published to the inbox
  (`solar_os_inbox_publish`, source `agent`, topic `reply`) so it shows up in
  `inbox` and on the status line. The full text lives in the conversation.
- Tool policy for unattended turns is **separate and stricter**:
  `agent config daemon-tools readonly|all`, default `readonly`. Under
  `confirm` with no confirmer present, the executor's existing denial path
  returns a structured "tool denied" to the model, which already copes with
  that. Nothing silently escalates.
- Autostart: `job start agentd` in `.shell/startup`, written by `onboard`.
  No new boot hook.

Deferred from v1 of the daemon, each a small follow-up once the queue exists:
MQTT topic trigger, MeshCore or chat-message trigger, email trigger, Lua or
Python `solaros.agent.send()` binding.

### Phase 5: small utilities (1 day, fold into whichever PR is open)

- `agent doctor`: the Phase 2 reachability and smoke test without prompts.
- `agent config` with no arguments prints the full configuration (key masked).
- `agent export SLOT PATH.md`: dump a conversation as Markdown for the
  `reader` app or `scp`.

## What we are deliberately not building

- A general tool set like bash, git, or arbitrary HTTP. The typed, policy
  gated registry is the right shape for a device that can brick its own
  storage; new tools go through it.
- Multiple providers at once, provider switching per conversation, or
  per-conversation model memory. One endpoint, one key, one model, changed
  with `/model` when you want.
- Long context. History is bounded at roughly 8 KiB of raw text on the
  device and conversations cap at 10 KiB on flash or 48 KiB on SD. Resume is
  "the model sees a bounded recent window", not "the model remembers
  everything". Server-side continuation would fix this but is not part of the
  Messages API; revisit if it becomes available.
- Multi-line input, image input, voice.

## Validation plan

Per PR:

- `python3 -m unittest discover tests` (flavor and package catalog, manual
  generation, foreground app state checker).
- `make -C tests/host test` including the new `agent_anthropic_test`.
- `pio run -e <your board>` for the default `full` flavor, which has
  `agent = true`.
- Hardware: run `onboard` from a port shell and from the display; run `agent
  ask` with a 20-word prompt containing an apostrophe inside double quotes;
  confirm a tool round trip (`what files are in /`) streams and is saved;
  reboot and confirm bare `agent` resumes it; start `agentd`, `agent send`,
  and confirm an inbox entry and a new conversation turn.

## Suggested order and sizing

| Phase | Work | Size | Depends on |
| --- | --- | --- | --- |
| 0 | `ask` fix, out-of-band prompt, better missing-config path | 0.5 day | none |
| 1 | Anthropic Messages provider + host test | 3 to 4 days | none |
| 2 | `onboard` app, `agent doctor` | 3 days | 1 (model list, smoke test) |
| 3 | resume-by-default, slash commands, footer state | 2 days | 0 |
| 4 | `agentd` queue, schedule, HTTP trigger, inbox delivery | 4 to 5 days | 0, 1, 3 |
| 5 | utilities | 1 day | any |

Phases 0 and 1 can start in parallel. Phase 2 before Phase 3 because easy
configuration is the stated priority and the chat polish is cosmetic until the
endpoint works.
