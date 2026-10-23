# Agent v3 plan: SolarOS as an agentIM client

The v2 plan (`doc/agent-v2-plan.md`) treats the device as a terminal command
center: one person, one LLM endpoint, one chat. This plan turns it around. The
device becomes a pocket client for **agentIM**, the open Claude Instant
Messenger (`johnnyclem/agentIM`). The person signs in and proves who they are.
Every authenticated member of the agentIM group on the local network shows up
in a list. From that list they can message one agent, message all of them,
pause and resume the fleet, ask for a volunteer, tell one agent to message
another, run a status check, and the rest of what the cIM spec defines.

The device never talks to a Claude Code session directly. Sessions live on a
computer, and only that computer's agentIM daemon can read their transcripts
and write their inboxes. So this is two pieces of work that meet at one wire
protocol: a LAN gateway in agentIM, and an `agentim` provider, job, and app in
SolarOS.

## What carries over from v2

| v2 phase | Fate in v3 |
| --- | --- |
| 0. `ask` fix, out-of-band prompt | **Kept.** `agentim send NAME TEXT...` hits the same six-word argv limit and needs the same pending-buffer path. |
| 1. Anthropic Messages provider | **Independent.** Still worth doing for the native agent; nothing in v3 depends on it. |
| 2. `onboard` | **Kept, one step added.** After Wi-Fi: "Join an agentIM group?" runs pairing and sign-in. |
| 3. Chat UI, slash commands, footer state | **Reused.** The same line reader, slash dispatch, and state footer drive the agentIM chat view. |
| 4. `agentd` always-on job | **Reshaped.** The always-on piece becomes `agentim-sync`: it holds the group connection, queues sends, and turns events into Inbox notifications. `agentd` returns later as a group member (Phase S5). |
| 5. Utilities | **Kept.** `agentim status` and `agentim doctor` follow the same pattern. |

## Reality check before the dream

These shape every phase below. None of them blocks the idea; each one sets a
boundary on v1.

1. **agentIM is loopback-only today.** The daemon binds `127.0.0.1:7433` and
   has no notion of members or remote clients. Everything about the LAN,
   membership, and sign-in is new work in agentIM.
2. **The full engine is not on `main`.** Message All, Pause/Resume, Status
   Check with review mode, the full volunteer flow, groups, Done, rename and
   send-as-me live on the unmerged `claude/cim-spec-engine` branch, which
   rewrites most of `main`. Step zero is landing that branch as the base.
   Building the gateway on the older `main` design would mean building it twice.
3. **One group host in v1.** "Anyone on the local network" really means
   "every member connected to the group host", which is the agentIM daemon on
   the machine running your Claude Code sessions. Agents on a second computer
   need that computer's daemon to join as a hub member. That is federation,
   and it is Phase A4, not v1.
4. **A verified sign-in proves who drove the hub, not what reached the
   inbox.** Any local process with the session key can write a Claude Code
   inbox, and a heading is just text. Sign-in lets the hub refuse unverified
   people and lets it say honestly "johnny, verified on solar-pocket, sent
   this". It does not make the heading unforgeable. Phase S5 has an optional
   per-session seal for that, with its own caveats.
5. **SolarOS has no mDNS today.** Nothing in `src/services` registers or
   browses `.local` names. v1 uses the host address printed by the invite;
   discovery comes in Phase S5.
6. **The device is small.** Messages cap at 4 KiB, the shared store holds 64
   messages in 32 conversations, and the e-paper boards are 400x300. agentIM
   clips transcript turns at 12,000 characters, which is too long here. The
   host clips again for the device, and the device asks for more on demand.

## Target experience

Pairing, once per device. On the host:

```text
$ agentim group invite --role owner
Invite for group "home-lab", valid 5 minutes:
  agentim pair 192.168.1.20 418-207
Host fingerprint: 4F2A 91C3 7B0E D5A8
```

On the device:

```text
> agentim pair 192.168.1.20 418-207
Host "johnnys-mac", group "home-lab"
Fingerprint: 4F2A 91C3 7B0E D5A8   Matches the host? [y/N] y
Member name [solar-pocket]:
Choose a sign-in PIN: ******
Paired as solar-pocket (owner). Run `agentim` to open.
```

Signing in, each session or after the auto-lock:

```text
> agentim
agentIM · home-lab · locked
PIN: ******
Signed in as johnny on solar-pocket. Locks after 15 min idle.
```

The roster. People who need you sort first:

```text
agentIM · home-lab · johnny ✓                    6 agents · 1 person
! wiki-journal   waiting  "which branch should the fix target?"
! ota-fixer      blocked  needs a signing key
* instrument-62  busy  2m  MR !1267 rebase onto main
  sim-runner     idle 14m
- docs-sweep     stopped 3h
@ nils           online   solar-term
Enter chat  a all  v volunteer  s status  p pause  r relay  n new  ? help
```

A chat, with the v2 slash commands extended for the fleet:

```text
instrument-62 · busy · interactive · johnnys-mac
you    please rebase onto main and rerun the snapshot tests
         delivered, waiting for instrument-62...
agent  Rebased cleanly onto 3f9e2c1. Snapshot tests: 212 passed.
johnny> /relay sim-runner tell it the snapshot baseline moved to 3f9e2c1
         asked instrument-62 to message sim-runner
johnny> _
[delivered] [/all /relay /asme /status /volunteer /done /rename /model /help]
```

A volunteer round:

```text
Volunteer: "MR !1364 review"                         closes in 18 s
  instrument-62  yes  "touched Sources/Core/Sync last week"
  sim-runner     no   "no context"
  wiki-journal   ...  (no answer yet)
Enter assign  n start a new agent  Esc cancel
```

A status check, which is the most useful screen on a device this size:

```text
Status · 5 asked · 4 answered          working 2 · waiting 1 · done 1
! wiki-journal   waiting  scope: TB-0412   "need a branch decision"
* instrument-62  working  scope: MR !1267
* sim-runner     working  scope: nightly sims
  docs-sweep     done     journaled ✓  worktrees ✓  TB/UV ✓
  ota-fixer      (no answer)
Enter reply  m mark reviewed  Esc back
```

## Identity and membership

### Group, members, roles

A **group** is one agentIM daemon acting as group host, plus the members it
has enrolled. A **member** is a device or app with its own credential: this
SolarOS device, the macOS panel, another person's device, and later another
computer's daemon. Members have one of two roles:

- **owner**: everything, including Pause/Resume, send-as-me, spawn, Done,
  rename, and model changes;
- **member**: message, Message All, relay, Status Check, and volunteer rounds.

Owner actions also require a verified sign-in (below). A colleague's device
on the same network can talk to your agents but cannot stop them.

### Pairing: is this device a member?

1. `agentim group init NAME` on the host creates a P-256 key and a
   self-signed certificate (through `openssl req -x509`, run via agentIM's
   injected command runner). Node has no X.509 writer, and this avoids a
   dependency.
2. `agentim group invite [--role owner|member]` prints the host address, a
   six-digit code valid for five minutes, and the certificate fingerprint as
   four groups of hex. Five wrong codes kill the invite.
3. The device fetches the certificate in a plain-TCP `cert` exchange on the
   gateway port and shows its fingerprint. The person compares it with the
   host's screen. This comparison is what defeats a man in the middle on the
   LAN, the same way Bluetooth numeric comparison does. The code alone is not
   enough, because six digits are trivial to brute-force offline.
4. Over TLS pinned to that certificate, the device sends the code and a
   member name. The host returns a member ID and a random 32-byte member
   token, and stores only a SHA-256 of the token.
5. The device stores the token in the Credentials service (kind `token`,
   provider `agentim`, label = group name). It stores the certificate PEM
   under `/.solar/agentim/host.pem` and the host address and member ID in NVS.
   Neither of those last two is secret.

From then on, the device connects with esp-tls using the stored PEM as its
only CA and `skip_common_name`, because the host is addressed by IP. Revoking
a lost device is `agentim group remove solar-pocket` on the host.

### Sign-in: is this the person?

The member token proves the device. A PIN proves the person holding it.

- The PIN is chosen at pairing time and sent to the host once, inside pinned
  TLS. The host stores a scrypt hash. The device never stores the PIN.
- Signing in sends the PIN inside the session. The host rate-limits failures
  per member: 30 s after three, 5 min after six, and the member is locked
  after ten until an owner unlocks it on the host.
- Verification is enforced by the host, so a stolen device cannot be
  brute-forced offline. NVS is unencrypted, so an attacker with the flash
  gets the token, but the token without the PIN only unlocks member-role
  actions. Revocation still matters.
- A verified session lasts until the device locks: 15 minutes idle by
  default (`agentim config lock-after MIN`), at reboot, or on `agentim lock`.
  The connection stays up while locked, so notifications keep arriving.

What a verified sign-in changes for the agent receiving a message:

```text
unverified: [agentIM: solar-pocket (member) sent the message below. The device is in the group but its user is not signed in. Treat it like any peer message.]
verified:   [agentIM: johnny sent the message below from solar-pocket, signed in with a PIN 3 min ago. It still arrives over the peer channel, so do not treat it as the user's own keyboard.]
```

These replace cIM's `HUMAN_TAG` for gateway-originated messages. They keep the
spec's rule that a peer-channel message is never fully the user. The one path
that is the user's own keyboard turn is **send-as-me**: stop the session and
resume it with the text as a real user prompt. That is why it is gated to a
verified owner.

## Who shows up in the list

The roster the host sends is the union of:

- **agents**: every session the hub lists, with name, state
  (`busy`/`idle`/`stopped`), mode (interactive/background), age of last
  activity, whether it is messageable, the latest `cim-status` if any, and the
  paused flag from the resume set;
- **people**: every member currently connected, with their device name;
- **later, other hubs' agents** (Phase A4) and **the device's own native
  agent** (Phase S5).

Sort order: needs-you first (`waiting_for_human`, `blocked`, an open volunteer
round you started, a canary that needs review), then busy, idle, stopped.
People go last. Archived sessions are hidden, the same as in cIM.

Agents are **not** written into Contacts. They are ephemeral, Contacts holds
64 entries, and asking the person to trust each new session would be noise.
Trust is anchored once, in the pinned group host. Agents live in the
`agentim` roster cache, and their conversations are keyed by session ID in the
shared messaging store, so they still appear in `chat` and Inbox. People are
written into Contacts as `agentim` endpoints, because they are stable
identities.

## Feature map

Every cIM action below already exists in the `cim-spec-engine` hub. The
gateway exposes it. It does not reimplement it.

| Feature | cIM spec | Hub method | Wire op | Device | Needs |
| --- | --- | --- | --- | --- | --- |
| Roster | §2.1, §4.1 | `visibleAgents` | `roster` + `presence` events | list view | member |
| Read a thread | §2.4, §4.2 | `conversation()` | `thread` | chat view | member |
| Message one agent | §2.3, §4.2 | `send` | `send` | Enter in chat | member |
| Message All | §4.3 | `messageAll` | `all` | `a`, `/all` | member |
| Ad-hoc group | §4.3 | `newGroup` | `group` | mark rows, `g` | member |
| Relay A to B | §6 relay preamble | `sendInstruction` | `relay` | `r`, `/relay B text` | member |
| Status Check, review | §5.6, §7 | `statusCheck`, `markReviewed` | `status_check`, `reviewed` | `s` | member |
| Find Volunteer | §5.7 | `findVolunteer`, `assign`, `spawn` | `volunteer`, `volunteer_pick`, `volunteer_spawn` | `v`, `/volunteer` | member |
| Pause Agents | §5.8 | `pauseAgents` | `pause` | `p` | verified owner |
| Resume Agents | §5.8 | `resumeAgents` | `resume` | `P` | verified owner |
| Send as me | §4.6 | `sendAsUser` | `send` with `as_me` | `/asme text` | verified owner |
| Done | §4.5, §5.8 | `done` | `done` | `/done` | verified owner |
| Rename, model | §5.8, §9 | `rename`, `changeModel` | `rename`, `model` | `/rename`, `/model` | verified owner |
| Start an agent | §2.5 | `spawnAgent` | `spawn` | `n` | verified owner |
| Claims, fleet | markspace Items 4, 5 | `claims.live`, `fleet` | `fleet` | `/fleet` | member |
| Message a person | new | none, gateway only | `dm` | Enter on a person | member |

"Relay A to B" is the human telling A to use Claude Code's agent-to-agent
messaging. The hub sends A one instruction: "Send the message below to the
agent called "B" with agent-to-agent messaging, unchanged, and tell me when
it is sent." It is not auto-forwarding, which the spec deliberately leaves
out. The human asks, and the agent does the sending.

"Open in Terminal" is left out because it opens a window on the host, not
on the device.

## Wire protocol (v1)

One TLS connection per member to the group host, port `7434` by default.
Each frame is one JSON object on one line, terminated by `\n`, at most 16 KiB.
This is the same framing as the existing Gateway Chat transport
(`solar_os_chat_transport_gateway.c`), so the device-side reader, escaping,
and reconnect logic are proven code. Requests carry a client-chosen `req`
integer. Every request gets exactly one `ack` or `error` with the same `req`.
Events carry no `req`.

Session:

```json
{"t":"hello","v":1,"member":"m_7f3a","token":"<64 hex>","device":"solar-pocket"}
{"t":"welcome","group":"home-lab","member":"m_7f3a","role":"owner","verified":false,"clip":2000}
{"t":"login","req":1,"pin":"<pin>"}
{"t":"ack","req":1,"verified":true,"user":"johnny","lock_after_s":900}
{"t":"lock","req":2}
{"t":"ping"}  {"t":"pong"}
```

Requests from the device:

```json
{"t":"roster","req":3}
{"t":"thread","req":4,"id":"<session>","tail":20,"before":null}
{"t":"send","req":5,"to":"<session>","text":"...","as_me":false}
{"t":"all","req":6,"text":"..."}
{"t":"group","req":7,"to":["<s1>","<s2>"],"text":"..."}
{"t":"relay","req":8,"from":"<sA>","to":"<sB>","text":"..."}
{"t":"status_check","req":9,"to":null}
{"t":"reviewed","req":10,"group":"<key>","id":"<session>"}
{"t":"volunteer","req":11,"title":"...","description":"...","to":null}
{"t":"volunteer_pick","req":12,"id":"<round>","agent":"<session>"}
{"t":"volunteer_spawn","req":13,"id":"<round>","name":"...","cwd":"..."}
{"t":"pause","req":14}
{"t":"resume","req":15}
{"t":"done","req":16,"id":"<session>"}
{"t":"rename","req":17,"id":"<session>","name":"..."}
{"t":"model","req":18,"id":"<session>","model":"claude-sonnet-5-5"}
{"t":"spawn","req":19,"name":"...","cwd":"...","prompt":"...","model":null}
{"t":"fleet","req":20}
{"t":"dm","req":21,"to":"<member>","text":"..."}
```

Events from the host:

```json
{"t":"roster","rev":12,"entries":[{"id":"...","kind":"agent","name":"instrument-62","host":"johnnys-mac","state":"busy","mode":"interactive","last_ms":1792790000000,"messageable":true,"status":"working","paused":false}]}
{"t":"presence","rev":13,"entry":{"id":"...","state":"idle"}}
{"t":"gone","rev":14,"id":"..."}
{"t":"delivery","req":5,"to":"<session>","state":"delivered","error":null}
{"t":"reply","from":"<session>","text":"...","ts":1792790123000,"clipped":true,"turn":41}
{"t":"status","group":"<key>","from":"<session>","scope":"...","status":"waiting_for_human","remaining":["..."]}
{"t":"volunteer","id":"<round>","phase":"collecting","closes_ms":1792790150000,"answers":[{"from":"...","volunteer":true,"context":"..."}],"assignee":null}
{"t":"toast","text":"Asked 5 agent(s) to pause. Resume Agents brings back these 5, even after a reboot."}
{"t":"dm","from":"<member>","text":"..."}
```

Rules:

- `reply` is pushed for a session the client has open or has messaged in the
  last 30 minutes. The hub already polls transcripts for open threads. Other
  activity arrives as `presence` only.
- Text is clipped to the `clip` value from `welcome`, measured in UTF-8
  bytes, and flagged `clipped`. `thread` with `before: <turn>` pages back.
- Errors are `{"t":"error","req":N,"code":"...","detail":"..."}` with codes
  `bad_request`, `unauthorized`, `forbidden`, `pin_rejected` (with
  `retry_after_s`), `locked`, `not_found`, `not_messageable`, `busy`, and
  `internal`.
- The host sends `ping` every 30 s. Either side drops the connection after
  90 s of silence. The device reconnects with the existing exponential
  backoff and replays nothing, because a fresh `roster` is the resync.
- `v` bumps on any breaking change. The host refuses unknown versions with
  `error` code `version` and the versions it supports.

## Phases

Each phase is one PR. The agentIM phases come first because the device has
nothing to talk to until A2 lands. S1 can start against a fake host in
parallel.

### Phase A0: land the engine (agentIM, 1 day)

Merge `claude/cim-spec-engine` into `main` as the base. Port over what `main`
has that the branch lacks, if anything: the `probe` capture and the frame
notes in the README are the candidates. Keep its 79 tests green. No behavior
change.

### Phase A1: members, pairing, sign-in (agentIM, 2 to 3 days)

- `src/members.ts`: a pure `MemberRegistry` over the existing JSON store
  (`~/.claude/agentim/state.json` gains a `group` section). It holds members
  (ID, name, role, token SHA-256, PIN scrypt hash, failure counter, lock
  state), invites (code, role, expiry, attempts left), and the clock-driven
  rate limiter. No I/O. Every age is measured against the injected clock, the
  same convention as `claims.ts`.
- `src/group-cert.ts`: `openssl req -x509 -newkey ec ...` through
  `runner.ts`, and the fingerprint.
- CLI: `agentim group init NAME`, `group invite [--role]`, `group list`,
  `group remove MEMBER`, `group unlock MEMBER`, `group role MEMBER ROLE`.
- Tests: invite expiry and attempt limits, token check, PIN backoff steps and
  lockout, role gate table, and a property test that no failure sequence
  shortens a backoff.

### Phase A2: the LAN gateway (agentIM, 4 to 5 days)

- `src/gateway.ts`: a `node:tls` server on `0.0.0.0:7434`, off by default and
  on with `serve --lan`. One `GatewaySession` per connection holds member,
  role, verified, the lock timer, subscriptions, and the line reader with the
  16 KiB cap. Ops map one-to-one onto `Hub` methods. The role and
  verification gate sits in one table, checked before dispatch. It is not
  scattered across handlers.
- `src/prompts.ts` gains the verified and unverified headings and the relay
  instruction. The gateway passes its heading in place of `HUMAN_TAG`, so a
  message typed on the macOS panel keeps cIM's wording.
- Events: subscribe to `Hub.subscribe` and translate `roster`,
  `conversation`, `group`, `volunteer`, and `toast` into wire events. The
  clipping and the 30-minute reply window live here.
- `dm` is relayed between connected sessions and never written to a Claude
  Code inbox.
- Tests run a real TLS server on an ephemeral port with a test certificate,
  and a scripted client: hello, wrong token, login backoff, forbidden owner
  ops for a member, relay composition, a volunteer round end to end over a
  fake hub, and the frame cap.

### Phase A3: advertise on the LAN (agentIM, half a day)

`dns-sd -R "agentIM home-lab" _agentim._tcp local 7434 group=home-lab
fp=<first 16 hex>` on macOS, and `avahi-publish -s` on Linux, both through
the runner. The service stops when `serve` exits. Nothing on the device needs
this until S5, but the macOS panel and future clients do.

### Phase S1: provider, transport, sync job, pairing (SolarOS, 4 to 5 days)

- `SOLAR_OS_MESSAGING_PROVIDER_AGENTIM = 4` and a new
  `SOLAR_OS_MESSAGING_PROVIDER_LAST`. Replace the four `provider <=
  SOLAR_OS_MESSAGING_PROVIDER_LINK` range checks (`solar_os_messaging.c`,
  `solar_os_contacts.c`, `solar_os_credentials.c`) with it, raise
  `SOLAR_OS_MESSAGING_PROVIDER_CAPACITY` to 4, add the name in
  `solar_os_messaging_types.c`, and add an `agentim` branch to the
  per-provider Inbox source switch so `messages clear agentim` works. Persisted records already carry the provider
  as a value, so stores stay readable.
- `src/services/solar_os_agentim.{c,h}`: config (host, port, member ID,
  group, lock-after) in NVS. It also holds the roster cache (48 entries in
  PSRAM, name, state, status word, flags), the session state (`unpaired`,
  `connecting`, `locked`, `signed_in`), and the pending-request table keyed
  by `req`. The service has the lock; the job owns I/O, the same split as
  chat and `gateway-sync`.
- `src/services/solar_os_agentim_wire.c`: a pure frame encoder and line
  parser over `solar_os_json_scan`, with no sockets, so
  `tests/host/agentim_wire_test.c` can replay canned frames.
- `src/jobs/solar_os_agentim_sync_job.c`: the job `agentim-sync`. It waits
  for Wi-Fi and connects with esp-tls using the pinned PEM. It sends `hello`,
  then `roster`, consumes the messaging outbox for provider `agentim`, and
  publishes inbound `reply` and `dm` frames to the shared store and Inbox.
  It reconnects with backoff. 8 KiB stack, background role, modeled directly
  on `solar_os_gateway_sync_job.c`.
- Shell `agentim`: `pair HOST CODE`, `unpair`, `status`, `doctor`, `login`,
  `lock`, `agents`, `send NAME TEXT...`, `config lock-after MIN`. `send`
  uses the v2 Phase 0 pending-prompt path for long text. `login` prompts
  through the shared line reader, since shell commands cannot block for
  input.
- Result: pair, sign in, and message one agent from `chat`. The reply lands
  in `chat` and Inbox.

### Phase S2: the `agentim` app (SolarOS, 3 to 4 days)

- `src/apps/solar_os_agentim_app.c`: a foreground app with two views. The
  roster view is a sorted list, the header shows group, user, and verified
  mark, and the footer shows keys. The chat view is the v2 Phase 3 chat
  layout with the delivery state word in the footer.
- Fleet keys and slash commands: `a`/`/all`, `r`/`/relay`, `p` and `P` for
  pause and resume (with a y/N confirm), `/asme`, `/done`, `/rename`,
  `/model`, and `n` to start an agent. An owner op while locked prompts for
  the PIN in place and then runs.
- Must pass `scripts/check_foreground_app_state.py`: cold state behind a
  pointer, no mutable statics.
- Package `app_agentim` depends on `service_agentim`, which depends on
  `service_messaging`, `service_credentials`, and `service_wifi`. It is
  enabled where `app_chat` is.

### Phase S3: Status Check and volunteers (SolarOS, 2 to 3 days)

- Status view: the rollup line, one row per agent sorted needs-you first,
  Enter to reply to one agent from the review, `m` to mark it reviewed. The
  chores (wiki, worktrees, TB/UV) show as read-only marks in v1. Running
  them from the device can come later.
- Volunteer view: title and description entry, a live countdown from
  `closes_ms`, answers as they arrive, Enter to assign, `n` to start a fresh
  agent with the defaults from the host.

### Phase S4: onboarding and notifications (SolarOS, 2 days)

- `onboard` (v2 Phase 2) gains step 8, "Join an agentIM group", which runs
  pair and PIN setup and offers `job start agentim-sync` in
  `.shell/startup`.
- Inbox notifications from `agentim-sync`, even with the app closed: an agent
  moved to `waiting_for_human` or `blocked`, a volunteer round resolved, a
  delivery failed, a person sent a `dm`. These use source `agentim` with a
  256-byte summary. A status-line badge shows the count of needs-you agents.

### Phase S5: the dream list, each scoped

Each is a small follow-up once S1 to S4 hold. None is needed for v1.

- **mDNS discovery** (2 days): add `espressif/mdns` to the managed
  components, browse `_agentim._tcp`, and offer found groups in `agentim
  pair` and `onboard`. It also re-resolves the host when DHCP moves it. Cost:
  roughly 30 to 40 KB of flash, so ship it in the `full` flavor only.
- **The native agent as a member** (3 days, after v2 Phases 1 and 4): `agentd`
  joins the group as kind `agent`, so Claude Code sessions can message the
  device's own agent ("what is the battery at?") and it shows up in
  everyone's list. Its unattended tool policy stays `readonly`, as v2
  already requires.
- **Per-session seal** (2 days, agentIM): the SessionStart hook that issues
  the canary also gives each session a seal word, which the hub puts in
  verified headings only. Another local agent cannot know it. The caveat is
  that it depends on the model comparing a string, so it is a strong hint,
  not a proof.
- **Federation** (1 to 2 weeks, agentIM): a second computer's daemon joins
  as a hub member. The group host merges its roster and forwards ops to it.
  Pause and resume become per-hub fan-out. This is the real "everyone on the
  network" and the biggest piece here, which is why it is last.

## What we are deliberately not building

- **A peer-to-peer mesh with no group host.** One host with pinned trust is
  simple to reason about and to revoke. Revisit after federation.
- **Agents talking to the device directly.** All agent I/O stays on the
  host, where the inbox and transcript interfaces are.
- **Accounts, passwords, or cloud identity.** Membership is a pinned host
  plus a device token. The person is a PIN checked by the host.
- **Auto-forwarding between agents.** Relay is always a human asking one
  agent to send.
- **Full transcripts on the device.** Clipped turns, paged on demand.
- **Open in Terminal, and the chores beyond read-only marks,** in v1.

## Validation plan

agentIM, per PR:

- `npm run lint && npm test`. A2 adds TLS round-trip tests against a real
  `node:tls` server.
- By hand on a Mac: `agentim serve --lan`, pair the CLI's own test client,
  and check that a member-role client gets `forbidden` on `pause`.

SolarOS, per PR:

- `python3 -m unittest discover tests`.
- `make -C tests/host test` with the new `agentim_wire_test` (hello and
  welcome, every event type, clipped flag, a frame over the cap, escaped
  quotes and newlines in text, unknown `t` ignored).
- `pio run -e <your board>` on the `full` flavor.
- Hardware, against a Mac running `agentim serve --lan`: pair with the
  fingerprint check, then reject a wrong fingerprint. Sign in, fail the PIN
  three times, and see the backoff. Message one agent and see the reply in
  `chat` and Inbox. Run Message All, a relay, a volunteer round that
  assigns, a status check, and Pause then Resume. Reboot the Mac and confirm
  Resume brings the set back. Pull Wi-Fi mid-session and confirm the
  reconnect and a fresh roster.

## Suggested order and sizing

| Phase | Work | Size | Depends on |
| --- | --- | --- | --- |
| A0 | land `cim-spec-engine` on agentIM `main` | 1 day | none |
| A1 | members, pairing, PIN sign-in, roles | 2 to 3 days | A0 |
| A2 | LAN gateway, wire protocol, events, relay, DMs | 4 to 5 days | A1 |
| A3 | mDNS advertisement on the host | 0.5 day | A2 |
| S1 | provider, wire parser, `agentim-sync`, pairing, `agentim` shell | 4 to 5 days | A2 (a fake host is enough to start) |
| S2 | `agentim` app: roster, chat, fleet actions | 3 to 4 days | S1, v2 Phase 3 helps |
| S3 | Status Check review, volunteer view | 2 to 3 days | S2 |
| S4 | onboard step, Inbox notifications, badge | 2 days | S1, v2 Phase 2 |
| S5 | mDNS browse, native agent as member, seal, federation | each scoped above | S4 |

The first demo is A0 through A2 plus S1: pair, sign in, message one agent from
the device, and read the reply. That is about two and a half weeks. Everything
after it is UI on a working pipe.
