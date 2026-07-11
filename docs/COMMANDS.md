# Command reference

All commands are typed into the EuroScope command line (the edit box at the
top of the main window) and start with `.wsc`. Callsigns are
case-insensitive. Output appears in a chat tab named **WSC**.

Everything the plugin does here can also be triggered by the backend over
the HTTPS gateway — the commands are a 1:1 manual front end for `Actions`
(see [DEVELOPER-GUIDE.md](DEVELOPER-GUIDE.md)).

---

## `.wsc help`

Prints a one-line summary of every command.

---

## 1. Listing flights

### `.wsc list [filter]`

Lists every flight plan EuroScope currently knows in this session, one line
per flight:

```
DLH4TX  A320  EDDM>EDDF  RFL240  CFL120  SQ1000  TAXI  GS18  [me]
```

Fields, in order: callsign, aircraft type, origin>destination, final
(requested) flight level, cleared level (`CFL:ILS` / `CFL:VIS` for approach
clearances), assigned squawk, ground state + `CLEA` when the clearance flag
is set, ground speed (only when the plan is correlated to a radar target),
and the tracking controller in brackets (`[me]` when it's you).

- `filter` — optional; keeps flights whose **callsign starts with** the
  filter, or whose **origin or destination equals** it:
  `.wsc list DLH`, `.wsc list EDDF`.
- Output is capped at 25 lines (the total match count is always shown) —
  use a filter in busy sessions.

**Scope:** EuroScope only knows flight plans within your visibility range /
facility; "all flights" means all flights *available to your session*, not
the whole network.

### `.wsc show <callsign>`

Multi-line detail for one flight: plan type, aircraft type + WTC, routing
with runways and alternate, **SID and STAR** (capability 6, read side),
RFL/CFL, all controller-assigned values (heading, speed, mach, rate,
direct), assigned squawk vs. actual transponder code, current FL and ground
speed, **ground state and clearance flag**, tracking controller,
communication type (`v`/`r`/`t`), **scratch pad**, full route, remarks.

---

## 2. Modifying flight-plan parameters

### `.wsc set <callsign> <field> <value>`

| Field | Value | Effect (EuroScope terminology) |
|-------|-------|--------------------------------|
| `calt` (or `cfl`) | `FL240`, `24000`, `ils`, `visual`, `clear` | Cleared (temporary) altitude. `ils`/`visual` are EuroScope's special values 1/2 = cleared for ILS/visual approach; `clear`/`0` removes the cleared level so the final altitude applies again |
| `rfl` (or `final`) | `FL340`, `34000` | Final / requested cruise altitude |
| `hdg` | `1`–`360`, `0` clears | Assigned heading |
| `spd` | knots, `0` clears | Assigned IAS |
| `mach` | `0.78`, `.78`, `78`; `0` clears | Assigned Mach (stored as hundredths) |
| `rate` | feet/minute, `0` clears | Assigned climb/descent rate |
| `sqk` | 4 octal digits, e.g. `2354` | Assigned squawk |
| `dct` | waypoint name | Direct-to point |

Examples:

```
.wsc set DLH4TX calt FL120
.wsc set DLH4TX hdg 250
.wsc set DLH4TX mach 0.78
.wsc set BAW23K calt ils
.wsc set BAW23K sqk 2354
```

**Permissions:** these are *controller-assigned data*. EuroScope only
accepts (and distributes to other controllers) changes to flights you are
allowed to modify — in practice: you must be connected, and for most values
you should be **tracking** the aircraft. When EuroScope refuses, the plugin
tells you.

---

## 3. Text message to the primary frequency — *experimental*

### `.wsc freq <message text>`

Sends free text to your **primary frequency** chat.

**How it works & caveats — please read:** the plugin API has *no* function
for sending chat messages, so the plugin types the text into EuroScope's
command line and presses the **FREQ key** for you. Consequences:

- It assumes the FREQ key is on its **default binding (numeric pad `*`)**.
  If you remapped it in EuroScope's key settings, this command won't send.
- EuroScope may prefix the message with the currently **selected (ASEL)
  aircraft's callsign** — that is EuroScope's own behaviour for the FREQ key.
- It always goes to the *primary* frequency; there is no way to target
  another frequency.
- If EuroScope doesn't consume the text, the plugin restores whatever you
  had typed in the command line and reports failure.

## 4. Private message to a user — *experimental*

### `.wsc msg <callsign> <message text>`

Sends a private message using EuroScope's own documented `.msg` command
(`.msg <callsign> <free text>`), typed into the command line on your
behalf. Same injection mechanism and caveats as `.wsc freq` (minus the
FREQ-key remapping issue — this one uses plain ENTER).

```
.wsc msg DLH4TX please check your assigned squawk
.wsc msg EDDF_TWR handoff coming in 2 minutes
```

---

## 5. Scratch pad

### `.wsc pad <callsign> <text|clear>`

Sets (or clears) the scratch pad string persistently. Multi-word text is
allowed.

```
.wsc pad DLH4TX RWY CHANGE
.wsc pad DLH4TX clear
```

Note: the scratch pad is synchronised to other controllers, but keep it
short — EuroScope documents that only the first characters are guaranteed
to be published to neighbouring clients (the built-in `.qs` command
documents 5 characters).

### `.wsc state <callsign> <token>`

Sets **ground states and flags** using EuroScope's magic scratch-pad
tokens. The API has no `SetGroundState()`; instead, writing one of these
values into the scratch pad makes EuroScope interpret, apply and distribute
it. The plugin uses the standard broadcast pattern (write token → restore
previous scratch-pad content), so your scratch pad text is preserved.

| Token | Meaning |
|-------|---------|
| `NSTS` | clear ground state (not started) |
| `STUP` | start-up approved |
| `PUSH` | push-back approved |
| `TAXI` | taxi |
| `DEPA` | departure / take-off clearance |
| `TXIN` | taxi in (after landing) |
| `PARK` | parked at gate |
| `CLEA` | clearance received flag ON |
| `NOTC` | clearance received flag OFF |
| `ARR`  | mark as arrival traffic |

```
.wsc state DLH4TX STUP
.wsc state DLH4TX PUSH
.wsc state DLH4TX TAXI
.wsc state DLH4TX CLEA
```

These are the values EuroScope shows in the departure list ground-state
column (displayed as `ST-UP`, `PUSH`, `TAXI`, `DEPA`, …). Source: [EuroScope
“Non-Standard Extensions”](https://www.euroscope.hu/wp/non-standard-extensions/).

---

## 6. SID / STAR

*Reading* SID/STAR: `.wsc show <callsign>` (fields `SID:` / `STAR:`).

### `.wsc sid <callsign> <SID[/RWY]>`
### `.wsc star <callsign> <STAR>`

```
.wsc sid DLH4TX CINDY1S
.wsc sid DLH4TX CINDY1S/25R     (also assigns the departure runway)
.wsc star BAW23K ROKIL3A
```

**How it works:** the API has no SID/STAR setter. The plugin rewrites the
**route** — removing the currently extracted SID (first route token) or
STAR (last route token) if present, inserting the new one — and then calls
`AmendFlightPlan()` to publish the modified flight plan.

**Caveats:**

- The SID/STAR name must exist in the loaded sector file, and the procedure
  must connect to the route (share a fix), otherwise EuroScope will not
  extract it and the tag/lists won't show it.
- Amending a flight plan is a network operation: you must be connected, and
  the amended plan is visible to everyone.
- If a flight has an unusual route where the first/last token is *not* the
  procedure, review the route with `.wsc show` first — the rewrite is
  intentionally conservative and only removes a token that matches the
  currently extracted procedure name.

---

## 7. JSON contract (for external systems)

### `.wsc json <message>`

Feeds one message of the standardized JSON contract
([PROTOCOL.md](PROTOCOL.md)) through the plugin and prints the response —
byte-for-byte what the backend receives over the HTTPS gateway. Everything
after `.wsc json` is passed through verbatim.

```
.wsc json {"type":"command","action":"ping"}
.wsc json {"type":"command","callsign":"DLH4TX","action":"get_flight"}
.wsc json {"type":"command","callsign":"DLH4TX","action":"set_ground_state","payload":{"state":"PUSH"}}
```

Every capability on this page is also reachable through the contract; see
the action table in [PROTOCOL.md](PROTOCOL.md).

### `.wsc events <on|off|pos on|pos off|status>`

Prints the contract's **event stream** (`type:"event"` messages) to the
WSC tab as events happen:

- `on` / `off` — `flight_updated` (flight plan or controller-assigned data
  changed) and `flight_removed` events.
- `pos on` / `pos off` — additionally `position_updated` events.
  **Warning:** one per radar target every few seconds; very noisy in busy
  sessions.
- `status` — show current toggles.

While a gateway is connected, the same messages also go to the backend —
the print toggles and the gateway are independent consumers.

---

## 8. Gateway connection

All `gateway` settings persist in the EuroScope settings file. The wire
model — `POST {base}/messages` to send, long-poll `GET {base}/poll` to
receive — is specified in [PROTOCOL.md](PROTOCOL.md) *Transport*.

### `.wsc gateway url <https://host[:port]/base-path>`

Sets the backend base URL; the plugin talks to `{base}/messages` and
`{base}/poll`. `https://` in production (TLS and certificate validation
are done by Windows/WinHTTP); `http://` is accepted for local
development. Changing the URL while connected reconnects.

### `.wsc gateway token <bearer-token>`

The token sent as `Authorization: Bearer <token>` on every request — how
the backend authenticates the plugin and tells sessions apart.
**Stored in plain text in the EuroScope settings file: use a dedicated,
revocable token** (e.g. a Laravel Sanctum token), not a personal password.

### `.wsc gateway connect` / `.wsc gateway disconnect`

Starts/stops the transport. While enabled, failures retry with
exponential backoff (2 s → 60 s); auth failures (401/403) park at the
maximum until the token is fixed. After every recovery the plugin
re-sends a `session_snapshot` so the backend is consistent again.

### `.wsc gateway auto <on|off>`

Persisted: connect automatically when the plugin loads (requires a saved
URL and token).

### `.wsc gateway pos <on|off>`

Persisted: forward `position_updated` events to the backend (default on).
Turn off to cut traffic massively in busy airspace; flight events and
snapshots are always sent.

### `.wsc gateway status`

Shows connection state, URL, whether a token is set, sent/received/
dropped counters and the last error.

```
.wsc gateway url https://api.example.com/euroscope
.wsc gateway token 3|x7Jd...
.wsc gateway connect
.wsc gateway status
```
