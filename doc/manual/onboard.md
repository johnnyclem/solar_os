+++
id = "onboard"
title = "Agent setup"
section = "app"
summary = "Go from a fresh flash to a working agent: Wi-Fi, provider, key, model, and a test request"
aliases = ["agent.setup", "agent.doctor"]
keywords = "onboard setup first run wifi api key endpoint model doctor check agent"
packages_any = ["app_agent"]
+++
# Agent setup

`onboard` takes a freshly flashed device to a working agent in one guided pass.
It works on the display and on port shells such as UART, USB CDC, and Telnet.
Each answer is saved as soon as it is given, so leaving early with `Esc` or
the app-exit key keeps what was already set.

You rarely need to start it yourself. When `agent` cannot start because Wi-Fi
is down or no server is configured, it opens `onboard` instead and continues
into the agent when setup is done. A fresh device also prints a hint under
the welcome line.

## What it does

1. **Wi-Fi.** If the device has no address, `onboard` tries the saved network
   first. Otherwise it scans, lists networks by signal strength, and asks for
   a number and a password. A hidden network can be typed by name. A failed
   join says whether the password looks wrong. Wi-Fi is turned back on for the
   next boot if it had been disabled.
2. **Server.** Choose OpenAI, which uses the Responses API, or any
   OpenAI-compatible Chat Completions server such as Ollama, LM Studio, or
   llama.cpp. For a local server, `192.168.1.20:11434` is enough: a missing
   `http://` and a missing `/v1/chat/completions` are added.
3. **API key.** Typed input is masked. When a key is already saved, Enter keeps
   it. Local servers usually need none.
4. **Server check.** One request to the server's model list proves the name
   resolves, the connection works, and the key is accepted. A failure names
   the stage, such as `server name not found`, `could not connect`, or
   `key rejected`, and offers a retry or a change.
5. **Model.** The models the server offers are listed, newest version numbers
   first, with a default marked `*`. Enter picks it. Any name can be typed,
   including one not shown. Servers without a model list ask for the name.
6. **Test request.** One short prompt goes through the same path `agent` uses.
   A rejected model or key returns to that step.
7. **Done.** Enter starts the agent. `c` changes the server, key, or model.

## Running it again

On a device that is already set up, `onboard` shows the saved endpoint, model,
and whether a key is saved, then runs the server check and the test request.
If both pass it is done. If one fails, it asks only for the part that failed.

## Checking a setup

`onboard check`, also reachable as `agent doctor`, runs the same checks with no
prompts and changes nothing. It reports Wi-Fi, the saved settings, the server
check, whether the saved model is offered, and the test request, then says
whether everything works.

```text
onboard check
agent doctor
```

Each run of the test request is a real, very small model request.

## Security

The key is stored like `agent config key`: in the `agent` NVS namespace, which
is not encrypted. It is never printed. The server check sends the key only to
the scheme, host, and port of the configured endpoint.

## Quick reference

`onboard` is the guided first-run setup for Wi-Fi and the agent: network,
server, API key, server check, model, and one test request, each saved as it
is answered. Re-running it re-checks a saved setup and asks only for what
fails. `onboard check` and `agent doctor` run the checks without prompts.
`agent` opens `onboard` automatically when it cannot start yet. `Esc` or the
app-exit key leaves at any time.
