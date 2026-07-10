# JSON contract — protocol v1

The standardized contract systems use to interact with the plugin, in both
directions: a backend/frontend sends **commands** to the plugin; the plugin
answers each command with a **response** and pushes unsolicited **events**
(flight updated, flight removed, position updated).

It is transport-independent: today it can be exercised through the
`.wsc json <message>` and `.wsc events` commands; phase 2 carries exactly
these messages over WebSocket. Implemented in
[`src/JsonApi.cpp`](../src/JsonApi.cpp) — keep code and this spec in sync.

- Encoding: UTF-8 JSON, one JSON object per message.
  **Stick to ASCII for anything that reaches the VATSIM network**
  (messages, scratch pad, routes): the protocol does not carry Unicode.
- Messages never crash the plugin: malformed input yields an `ok:false`
  response.
- Protocol version: **1** (reported by `ping`). Backwards-incompatible
  changes bump it; additive changes don't (consumers must ignore unknown
  fields).

## Envelope — every message, both directions

```json
{
  "type": "command",
  "id": "req-42",
  "callsign": "ABC1234",
  "action": "set_squawk",
  "payload": { "code": "2354" }
}
```

| Field | Type | Required | Meaning |
|-------|------|----------|---------|
| `type` | string | yes | `command` (→ plugin), `response` (plugin →, one per command), `event` (plugin →, unsolicited) |
| `id` | any | no | Correlation id: set it on a `command`, the matching `response` echoes it verbatim. Events carry no `id` |
| `callsign` | string | flight-scoped messages | Target/subject aircraft (case-insensitive). Omitted on session-level messages (`ping`, `list_flights`) |
| `action` | string | yes | The verb (`set_squawk`, …) on commands/responses; the event name (`flight_updated`, …) on events |
| `payload` | object | per action | Action/event-specific data. Named fields (not positional) so they can be added without breaking consumers |
| `ok` | bool | responses only | Whether the plugin/EuroScope accepted the command |
| `error` | string | responses with `ok:false` | Human-readable reason |

Direction summary:

```
backend/frontend ──command──▶ plugin
plugin ──response──▶ backend/frontend     (exactly one per command)
plugin ──event────▶ backend/frontend      (unsolicited, when enabled/connected)
```

## Commands

### Session-level

| Action | Payload | Response payload |
|--------|---------|------------------|
| `ping` | — | `{ "plugin": "...", "protocolVersion": 1 }` |
| `list_flights` | `filter` (string, optional): callsign prefix or exact origin/destination ICAO | `{ "count": N, "flights": [ FlightObject, ... ] }` |

### Flight-scoped read

| Action | Payload | Response payload |
|--------|---------|------------------|
| `get_flight` | — | FlightObject |

### Flight-scoped writes

Write responses carry `{ "message": "<human-readable outcome>" }`; failures
(EuroScope refusing because you are not connected / not tracking the
aircraft) come back as `ok:false` with the reason in `error`.

| Action | Payload | Notes |
|--------|---------|-------|
| `set_cleared_altitude` | `feet` (int) **or** `special` (`"ils"` \| `"visual"` \| `"clear"`) | CFL. `clear` removes the cleared level (RFL applies) |
| `set_final_altitude` | `feet` (int) | RFL / requested cruise |
| `set_heading` | `degrees` (int, 1–360; 0 clears) | |
| `set_speed` | `knots` (int; 0 clears) | Assigned IAS |
| `set_mach` | `mach` (number: `0.78` or hundredths `78`; 0 clears) | |
| `set_rate` | `feetPerMinute` (int; 0 clears) | Climb/descent rate |
| `set_squawk` | `code` (string, 4 octal digits) | |
| `set_direct` | `point` (string) | Direct-to waypoint |
| `set_scratchpad` | `text` (string; empty clears) | Raw scratch pad content |
| `set_ground_state` | `state` (string): `NSTS` `STUP` `PUSH` `TAXI` `DEPA` `TXIN` `PARK` `CLEA` `NOTC` `ARR` | Magic-token broadcast; previous scratch pad content is preserved. `CLEA`/`NOTC` drive the clearance-received flag |
| `set_sid` | `sid` (string, optional `/RWY` suffix) | Route rewrite + amend; name must exist in the sector file |
| `set_star` | `star` (string) | Route rewrite + amend |
| `send_private_message` | `message` (string) — recipient is `callsign` | **Experimental** (command-line injection; see [COMMANDS.md](COMMANDS.md) §4) |
| `send_frequency_message` | `message` (string) — no `callsign` needed | **Experimental**; primary frequency only (see [COMMANDS.md](COMMANDS.md) §3) |

`ok:true` on the two `send_*` actions means the message was handed to
EuroScope's command line, not that it was delivered on the network.

## Events (plugin → backend/frontend)

| Action | Fires when | `callsign` | Payload |
|--------|------------|------------|---------|
| `flight_updated` | A flight plan appears or changes — filed data **or** controller-assigned data (consumers get the full fresh object either way) | subject | FlightObject |
| `flight_removed` | The flight plan leaves the session (pilot disconnect / out of range) | subject | `{}` |
| `position_updated` | A radar target gets a new position (every few seconds per target; also for targets without a flight plan) | subject | `{ "latitude": 48.35, "longitude": 11.78, "flightLevel": 12000, "groundSpeed": 250, "squawk": "1000" }` |

Reserved for phase 2 (not emitted yet): `controller_updated`,
`controller_removed`, `session_snapshot`, `metar`.

Today the event stream can be inspected with `.wsc events on` (flight
events) and `.wsc events pos on` (position events — noisy) — the plugin
prints each event JSON to the WSC chat tab exactly as it will be sent over
the socket in phase 2.

## FlightObject

Returned by `get_flight`, `list_flights` and carried by `flight_updated`.
Numeric fields use `0` for "unassigned" (EuroScope convention); strings are
empty when absent.

```json
{
  "callsign": "DLH4TX",
  "planType": "I",
  "aircraftType": "A320",
  "wtc": "M",
  "origin": "EDDM",
  "destination": "EDDF",
  "alternate": "EDDK",
  "departureRunway": "26L",
  "arrivalRunway": "25R",
  "sid": "GIVMI1N",
  "star": "ROKIL3A",
  "route": "GIVMI1N GIVMI Y101 ERNAS T161 ROKIL3A",
  "remarks": "/v/",
  "finalAltitude": 24000,
  "clearedAltitude": 12000,
  "assignedHeading": 0,
  "assignedSpeed": 0,
  "assignedMach": 0,
  "assignedRate": 0,
  "directTo": "",
  "assignedSquawk": "1000",
  "scratchPad": "",
  "groundState": "TAXI",
  "clearanceFlag": true,
  "trackingController": "EDDM_TWR",
  "trackedByMe": true,
  "communicationType": "v",
  "correlated": true,
  "transponderSquawk": "1000",
  "flightLevel": 1200,
  "groundSpeed": 18,
  "latitude": 48.3538,
  "longitude": 11.7861
}
```

Field notes:

- `clearedAltitude`: feet, with special values `0` = none (final applies),
  `1` = cleared ILS approach, `2` = cleared visual approach.
- `assignedMach`: hundredths (`78` = M0.78).
- `groundState`: as EuroScope reports it (`ST-UP`, `PUSH`, `TAXI`, `DEPA`,
  … or empty). Note the *read* form `ST-UP` differs from the *write* token
  `STUP` — that asymmetry is EuroScope's, not ours.
- `communicationType`: `v` voice, `r` receive-only, `t` text (as filed /
  assigned).
- `transponderSquawk`, `flightLevel` (feet), `groundSpeed` (knots),
  `latitude`, `longitude` are only present when `correlated` is `true`
  (the plan is matched to a radar target).
- `list_flights` returns flights known to *this controller's session*
  (EuroScope's visibility range) — not the whole network.

## Worked examples

Command and its response:

```json
{ "type": "command", "id": 7, "callsign": "ABC1234",
  "action": "set_ground_state", "payload": { "state": "PUSH" } }

{ "type": "response", "id": 7, "ok": true, "callsign": "ABC1234",
  "action": "set_ground_state",
  "payload": { "message": "ABC1234: broadcast 'PUSH' (scratch pad restored to '')" } }
```

A failed command:

```json
{ "type": "command", "id": 8, "callsign": "ABC1234",
  "action": "set_squawk", "payload": { "code": "8888" } }

{ "type": "response", "id": 8, "ok": false, "callsign": "ABC1234",
  "action": "set_squawk",
  "error": "'8888' is not a valid squawk (4 octal digits, e.g. 2354)" }
```

Events as they arrive:

```json
{ "type": "event", "callsign": "ABC1234", "action": "flight_updated",
  "payload": { "callsign": "ABC1234", "origin": "EDDM", "...": "..." } }

{ "type": "event", "callsign": "ABC1234", "action": "position_updated",
  "payload": { "latitude": 48.3538, "longitude": 11.7861,
               "flightLevel": 12000, "groundSpeed": 250, "squawk": "1000" } }

{ "type": "event", "callsign": "ABC1234", "action": "flight_removed",
  "payload": {} }
```

## Error model

Every command failure is a `type:"response"`, `ok:false` message with
`error` set. Classes of failure, in the order they are checked:

1. **Malformed JSON / not an object** → `"message is not a JSON object"`
   (no `id` echo possible if unparseable).
2. **Envelope errors** — missing/unsupported `type` (the plugin only
   accepts `command`), missing/unknown `action`, missing `callsign` on a
   flight-scoped action.
3. **Payload validation** — missing/wrong-typed/out-of-range fields; the
   message names the offending field.
4. **Unknown flight** — callsign not in the session.
5. **EuroScope refusal** — the API returned `false` (not connected, no
   permission to modify the flight, …). The message explains the likely
   cause.

## Testing without a WebSocket

Type commands directly into EuroScope (everything after `.wsc json` is
passed through verbatim), and enable the event stream:

```
.wsc json {"type":"command","action":"ping"}
.wsc json {"type":"command","callsign":"DLH4TX","action":"get_flight"}
.wsc json {"type":"command","callsign":"DLH4TX","action":"set_cleared_altitude","payload":{"feet":12000}}
.wsc events on
.wsc events pos on
```

The printed messages are byte-for-byte what a WebSocket peer will exchange
in phase 2.

## Versioning rules

- Additive changes (new actions, new events, new optional payload fields,
  new FlightObject fields) do **not** bump the version — consumers must
  ignore unknown fields and unknown event actions.
- Renames, type changes, semantic changes bump `kProtocolVersion`
  (`src/JsonApi.h`) and this document's title.
