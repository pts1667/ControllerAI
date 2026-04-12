# ControllerAI: External AI Communication Layer

ControllerAI is an AI for Zero-K/BAR that starts a server from which you can controller your AI.
Each AI gets its own server. First poll the master server to get the actual list of ControllerAIs.
You will need to know the team ID list ahead of time, but you can get this from the start script when the match is started.

## AI Disclaimer

Made with Codex (GPT-5.4 and GPT-5.4-Mini).
Would've probably been faster to code this manually... this will be my last "vibe code" project, I think.

## Example

```python
import json
import time

import requests
from websocket import (
    WebSocketConnectionClosedException,
    WebSocketTimeoutException,
    create_connection,
)

MASTER_URL = "http://127.0.0.1:3017"
DISCOVERY_INTERVAL_SEC = 1.0
SOCKET_TIMEOUT_SEC = 0.1


def discover_instances():
    response = requests.get(f"{MASTER_URL}/list")
    response.raise_for_status()
    return response.json()


def make_state():
    return {
        "game_info": None,
        "spawn_boxes": None,
        "metadata": None,
        "map_features": None,
        "heightmap": None,
        "startup_complete": False,
    }


def send(ws, payload):
    ws.send(json.dumps(payload))


def connect_new_instances(clients):
    for team_id, instance in discover_instances().items():
        if team_id in clients or not instance.get("reachable", True):
            continue

        try:
            ws = create_connection(instance["wsUrl"], timeout=SOCKET_TIMEOUT_SEC)
            ws.settimeout(SOCKET_TIMEOUT_SEC)
        except OSError as exc:
            print(f"team {team_id}: connect failed: {exc}")
            continue

        clients[team_id] = {
            "ws": ws,
            "state": make_state(),
        }
        print(f"Connected to team {team_id} at {instance['wsUrl']}")


def handle_message(team_id, client, message):
    ws = client["ws"]
    state = client["state"]
    msg_type = message["type"]
    data = message["data"]

    if msg_type in {"game_info", "spawn_boxes", "metadata", "map_features", "heightmap"}:
        state[msg_type] = data
        return

    if msg_type == "error":
        print(f"team {team_id}: server error: {data}")
        return

    if msg_type == "query_result":
        print(f"team {team_id}: query result: {data}")
        return

    if msg_type == "settings":
        print(f"team {team_id}: current settings: {data}")
        return

    if msg_type == "settings_result":
        print(f"team {team_id}: settings updated: {data}")
        return

    if msg_type != "observation":
        return

    frame = data["frame"]
    print(f"team {team_id}: frame {frame}")

    if not state["startup_complete"] and frame < 0:
        send(ws, {"type": "set_commander", "name": "dyntrainer_strike_base"})

        game_info = state["game_info"] or {}
        if game_info.get("canChooseStartPos"):
            boxes = state["spawn_boxes"] or {}
            ally_box = boxes.get(str(data["allyTeamId"]))
            if ally_box:
                cx = (ally_box["left"] + ally_box["right"]) / 2
                cz = (ally_box["top"] + ally_box["bottom"]) / 2
                send(ws, {"type": "set_start_pos", "pos": [cx, 0, cz]})
            else:
                raise RuntimeError(f"team {team_id}: missing spawn box for ally team")
        else:
            send(ws, {"type": "finish_frame"})

        state["startup_complete"] = True
        return

    for unit_id in data["units"]:
        send(ws, {
            "type": "move",
            "unitId": int(unit_id),
            "pos": [1000, 0, 1000],
        })

    if data["units"]:
        first_unit_id = int(next(iter(data["units"])))
        send(ws, {
            "type": "query",
            "requestId": f"unitdef-{team_id}-{first_unit_id}",
            "query": {
                "type": "unit_def_by_unit_id",
                "unitId": first_unit_id,
            },
        })

    send(ws, {"type": "finish_frame"})


def main():
    clients = {}
    next_discovery_at = 0.0

    try:
        while True:
            now = time.time()
            if now >= next_discovery_at:
                try:
                    connect_new_instances(clients)
                except requests.RequestException as exc:
                    print(f"master discovery failed: {exc}")
                next_discovery_at = now + DISCOVERY_INTERVAL_SEC

            for team_id in list(clients):
                client = clients[team_id]
                ws = client["ws"]
                try:
                    raw_message = ws.recv()
                except WebSocketTimeoutException:
                    continue
                except WebSocketConnectionClosedException:
                    print(f"team {team_id}: disconnected")
                    ws.close()
                    del clients[team_id]
                    continue

                handle_message(team_id, client, json.loads(raw_message))
    finally:
        for client in clients.values():
            client["ws"].close()


if __name__ == "__main__":
    main()
```

This example polls the master `/list` endpoint once per second, opens one WebSocket per reachable team, and keeps checking for newly listed AIs while the match is running.

## How it Works

1.  **Engine Side**: ControllerAI runs as a standard Skirmish AI. In matches with one or more ControllerAI instances, it starts a shared master HTTP server on the configured port and one per-instance HTTP/WebSocket server on a derived port for each controlled team.
2.  **Observation**: During the pre-match phase the AI always updates its internal state immediately. After frame 0, observations are published every `block_n_frames` frames, so external services can either poll the per-instance `/observation` endpoint or use the per-instance `/ws` endpoint as a bidirectional batched frame loop.
3.  **WebSocket Bootstrap**: On connect, the WebSocket sends the cached startup data you would otherwise fetch from `/game_info`, `/spawn_boxes`, `/metadata`, `/map_features`, `/heightmap`, `/settings`, and then the latest observation if available.
4.  **Commands**: External services POST JSON commands to `/command` or send the same JSON payloads on `/ws`. These are executed on the next engine update.
5.  **Queries**: External services can call `GET /query` or send `type="query"` messages on `/ws` to execute engine-thread lookups such as full `UnitDef` serialization, resource spot lookups, and build-site checks without changing frame synchronization.
6.  **Runtime Settings**: External services can call `GET /settings`, `POST /settings`, or use the matching WebSocket messages to change runtime behavior such as `block_n_frames`.
7. **Synchronous Control**: By default, the engine can already block during startup handling so setup commands such as `set_commander`, `set_side`, and `set_start_pos` can be processed before the match proceeds. Before the first `EVENT_UPDATE`, the AI stays in a startup command loop. If the game requires choosing a start position, a valid `set_start_pos` unblocks startup. Otherwise, send `finish_frame` after your startup commands to let the match proceed. Once the game starts, it pauses at the end of each published observation batch until you send a `finish_frame` command.

## Configuration

You can configure the binding address and port through the engine's AI options (typically in the game setup or `AIOptions.lua`):

- **Binding Address (`ip`)**: The IP the server binds to. Default: `127.0.0.1`.
- **Server Port (`port`)**: The master discovery port. ControllerAI serves `/list` on this port and allocates per-instance servers from the next available ports above it. Default: `3017`.
- **Synchronous Mode (`sync`)**: If `true`, the engine thread blocks at the end of every update until a `finish_frame` command is received. **Default: `true`**. Note that setup-phase blocking (frame -1) is mandatory regardless of this setting if the map requires choosing a start position.


## API Endpoints

The configured `port` hosts the master discovery endpoint `http://localhost:<port>/list`.

Each actual ControllerAI instance then runs its own HTTP/WebSocket server on a derived port, typically `port + 1`, `port + 2`, and so on. Use `/list` to discover the endpoint for the team you want to control, then use that returned per-instance base URL for `/observation`, `/ws`, `/settings`, `/query`, and `/command`.

### 0. `GET /list`
Returns the live team ID to instance endpoint table for all active ControllerAI instances in the current match.

**Example Response:**
```json
{
    "0": {
        "address": "127.0.0.1",
        "port": 3018,
        "reachable": true,
        "endpoint": "127.0.0.1:3018",
        "httpUrl": "http://127.0.0.1:3018",
        "wsUrl": "ws://127.0.0.1:3018/ws",
        "skirmishAIId": 3
    },
    "1": {
        "address": "127.0.0.1",
        "port": 3019,
        "reachable": true,
        "endpoint": "127.0.0.1:3019",
        "httpUrl": "http://127.0.0.1:3019",
        "wsUrl": "ws://127.0.0.1:3019/ws",
        "skirmishAIId": 4
    }
}
```

The top-level keys are team IDs, which are the identifiers external AI services typically already know.

`reachable` reports whether the master server's probe of the per-instance `/observation` endpoint succeeded when `/list` was generated. Entries remain visible even when `reachable` is `false`, so startup clients can still discover the instance and retry the per-instance connection.

### 1. `GET /observation`
Returns the current snapshot of the game state and events accumulated since the last published observation.
For simple integrations, polling this endpoint is supported.

If `block_n_frames` is greater than `1`, the observation is refreshed every N post-start frames instead of every single frame. Startup observations are still published immediately.

**Example Response:**
```json
{
  "frame": 450,
  "aiId": 1,
  "teamId": 0,
  "allyTeamId": 0,
  "units": {
    "1024": {
      "defId": 52,
      "health": 1200.0,
      "maxHealth": 1200.0,
      "pos": [150.5, 12.0, 300.2],
      "vel": [0.0, 0.0, 0.0],
      "experience": 0.05,
      "buildProgress": 1.0,
      "isBeingBuilt": false
    }
  },
  "enemies": { ... },
  "radarBlips": {
    "2048": {
      "allyTeam": 1,
      "pos": [4200.0, 85.0, 3900.0],
      "vel": [0.0, 0.0, 1.5],
      "inLos": false,
      "isRadarBlip": true
    }
  },
  "economy": { ... },
  "events": [
    { "topic": 2, "unitId": 1025, "builderId": 1024 }
  ]
}
```

`enemies` contains units currently in LOS. `radarBlips` contains enemy contacts available through radar/sensor coverage but not currently in LOS.

### 2. `GET /metadata`
Returns static data about all unit types available in the current game/mod.

### 1b. `GET /ws`
Runs the full synchronous observation/command loop on a single WebSocket connection.

- Sends cached `game_info`, `spawn_boxes`, `metadata`, `map_features`, `heightmap`, `settings`, and then the latest observation immediately after connect when available.
- Sends each subsequent observation when the engine publishes the next configured batch.
- Accepts the same JSON command payloads as `POST /command`.
- Supports either one command per message or an array of command objects in a single message.
- Supports websocket request messages for the HTTP resources: `get_all`, `get_observation`, `get_game_info`, `get_spawn_boxes`, `get_metadata`, `get_map_features`, `get_heightmap`, and `get_settings`.
- Supports websocket settings updates with `{"type": "set_settings", "settings": {...}}` and replies with `settings_result` or `settings_error`.
- Supports websocket query messages with `{"type": "query", "query": {...}}` and replies with `query_result` or `query_error`.
- In synchronous mode, send commands for the current published frame and end the batch with `finish_frame`. The next observation is sent after the engine resumes and reaches the next configured publish frame.

Server-to-client WebSocket messages are wrapped as:

```json
{
  "type": "observation",
  "data": { ... }
}
```

The `type` field will be one of `game_info`, `spawn_boxes`, `metadata`, `map_features`, `heightmap`, `settings`, `settings_result`, `settings_error`, `observation`, `query_result`, `query_error`, or `error`.

### 3. `GET /spawn_boxes`
Returns the defined start boxes for each ally team.
- **Keys**: Ally Team IDs.
- **Values**: `{ "left", "top", "right", "bottom" }` in map coordinates (elmos).
- Zero-K polygon boxes also include `polygons`, an array of polygons where each polygon is an array of `[x, z]` vertices in elmos. In that case `left/top/right/bottom` remain the polygon bounding box for compatibility.

### 4. `GET /game_info`
Returns startup metadata for the current match.

Typical startup flow:
1. Poll `/observation` or connect to `/ws` until the AI is initialized.
2. Read `/game_info` to check whether startup setup is still available.
3. Send any startup commands you need, such as `set_commander` or `set_side`.
4. If `canChooseStartPos` is true, read `/spawn_boxes` and send `set_start_pos`. **This unblocks the engine setup phase.**
5. If no start position choice is required, send `finish_frame` to end the startup command loop.
6. Once the match reaches frame 0, the engine will block again (if `sync=true`). Call `finish_frame` to proceed with the game loop.

**Example Response:**
```json
{
    "modName": "Balanced Annihilation",
    "gametype": "BA",
    "mapName": "TitanDuel 2.2",
    "mapWidth": 1024,
    "mapHeight": 1024,
    "mapWidthElmos": 8192,
    "mapHeightElmos": 8192,
    "teamId": 0,
    "allyTeamId": 0,
    "serverAddress": "127.0.0.1",
    "serverPort": 3018,
    "masterServerAddress": "127.0.0.1",
    "masterServerPort": 3017,
    "masterListPath": "/list",
    "gameMode": 0,
    "isPaused": true,
    "canChooseStartPos": true,
    "supportsWebsocketApi": true,
    "websocketPath": "/ws",
    "supportsWebsocketObservation": true,
    "websocketObservationPath": "/ws"
}
```

### 5. `GET /map_features`
Returns resource spots and map features (trees, rocks, etc.).

The payload also includes:
- `waterDamage`: raw `map->GetWaterDamage()` value.
- `waterIsHarmful`: `true` when `waterDamage > 0`.
- `spots[*].pos`: world position `[x, y, z]`.
- `spots[*].income`: extractor income for that spot.
- `averageIncomeByResource`: average spot income keyed by resource name.

### 6. `GET /heightmap`
Returns the dynamic heightmap as a Base64 encoded string of `float32` values.

### 7. `GET /settings`
Returns the current runtime settings.

**Example Response:**
```json
{
    "block_n_frames": 1
}
```

### 8. `POST /settings`
Updates runtime settings.

Supported settings:
- `block_n_frames`: integer `>= 1`, default `1`. After startup, ControllerAI publishes an observation and blocks for commands every N frames instead of every single frame. Startup handling before frame 0 is unchanged.

**Example Request:**
```json
{
    "block_n_frames": 5
}
```

**WebSocket equivalents:**
- `{"type": "get_settings"}` returns a `settings` message.
- `{"type": "set_settings", "settings": {"block_n_frames": 5}}` updates the settings and returns `settings_result`. All connected WebSocket clients also receive the updated broadcast `settings` message.

### 9. `GET /query`
Runs an engine-thread lookup and returns the result as JSON.

This is the endpoint to use for data that is not already present in the observation or the cached startup endpoints. If the engine is currently blocked in startup sync or frame sync, the query wakes the AI just long enough to answer it and then immediately resumes the blocked state.

Responses are wrapped as:

```json
{
  "query": { "type": "unit_def_by_id", "unitDefId": 52 },
  "result": { "id": 52, "name": "cloakraid", "humanName": "Glaive", "buildOptions": [], "customParams": {}, "weaponMounts": [], "moveData": { ... } }
}
```

If the query is invalid, the endpoint returns HTTP `400` with an `error` field.

Supported query types:
- `game_rules_param`: requires `key`; optional `defaultFloat` and `defaultString`. Returns both `floatValue` and `stringValue` for the requested game rules param.
- `game_rules_params`: requires `keys` (JSON array or comma-separated string); optional `defaultFloat` and `defaultString`. Returns a `values` object keyed by param name.
- `team_rules_param`: requires `key`; optional `teamId`, `defaultFloat`, and `defaultString`. Defaults to the current AI team.
- `team_rules_params`: requires `keys` (JSON array or comma-separated string); optional `teamId`, `defaultFloat`, and `defaultString`.
- `unit_def_id_by_name`: requires `name`.
- `unit_def_by_id`: requires `unitDefId`. Returns the full serialized `UnitDef` table.
- `unit_def_by_name`: requires `name`. Returns the full serialized `UnitDef` table.
- `unit_def_id_by_unit_id`: requires `unitId`.
- `unit_def_by_unit_id`: requires `unitId`. Returns the full serialized `UnitDef` table for that unit's current definition.
- `unit_by_id`: requires `unitId`. Returns an expanded unit snapshot including its full serialized `definition`.
- `unit_rules_param`: requires `unitId` and `key`; optional `defaultFloat` and `defaultString`. Returns both `floatValue` and `stringValue`.
- `unit_rules_params`: requires `unitId` and `keys` (JSON array or comma-separated string); optional `defaultFloat` and `defaultString`.
- `feature_by_id`: requires `featureId`.
- `feature_def_by_id`: requires `featureDefId`.
- `feature_def_by_name`: requires `name`.
- `resource_by_name`: requires `name`.
- `resource_spots_by_name`: requires `name`. Returns normalized spot entries as `{ pos, income }` and prefers `mex_count` / `mex_*` game rules params for metal when present, matching CircuitAI.
- `nearest_resource_spot`: requires `name` plus `pos` or `x`/`z`. Returns the nearest normalized spot with `pos` and `income`.
- `elevation_at`: requires `pos` or `x`/`z`.
- `height_map`: no extra parameters. Returns the height map as Base64 encoded `float32` data with `width` and `height`.
- `water_damage`: no extra parameters. Returns `waterDamage` plus `waterIsHarmful`.
- `slope_map`: no extra parameters. Returns the slope map as Base64 encoded `float32` data with `width` and `height`.
- `ai_option_by_key`: requires `key`.
- `ai_options`: optional `keys` (JSON array or comma-separated string). Without `keys`, returns all AI option values.
- `ai_info_by_key`: requires `key`.
- `ai_info`: optional `keys` (JSON array or comma-separated string). Without `keys`, returns all AI info values.
- `start_position`: no extra parameters.
- `can_build_at`: requires `unitDefId` plus `pos` or `x`/`z`; optional `facing`.
- `closest_build_site`: requires `unitDefId`, `searchRadius`, and `pos` or `x`/`z`; optional `minDist` and `facing`.

**HTTP examples:**

```text
GET /query?type=unit_def_id_by_name&name=cloakraid
GET /query?type=game_rules_param&key=mex_count
GET /query?type=team_rules_param&key=start_box_id
GET /query?type=unit_rules_params&unitId=1024&keys=disarmed,noammo,comm_level
GET /query?type=unit_def_by_id&unitDefId=52
GET /query?type=unit_def_by_unit_id&unitId=1024
GET /query?type=height_map
GET /query?type=water_damage
GET /query?type=slope_map
GET /query?type=ai_options
GET /query?type=closest_build_site&unitDefId=52&x=1200&z=3400&searchRadius=600&minDist=2&facing=0
```

**WebSocket query example:**

```json
{
  "type": "query",
  "requestId": "build-check-1",
  "query": {
    "type": "can_build_at",
    "unitDefId": 52,
    "pos": [1200, 0, 3400],
    "facing": 0
  }
}
```

Successful WebSocket query responses look like this:

```json
{
  "type": "query_result",
  "data": {
    "requestId": "build-check-1",
    "query": {
      "type": "can_build_at",
      "unitDefId": 52,
      "pos": [1200, 0, 3400],
      "facing": 0
    },
    "result": {
      "possible": true,
      "pos": [1200, 0, 3400],
      "facing": 0
    }
  }
}
```

### 10. `POST /command`
Sends a command to the engine. Standard fields: `unitId`, `type`, `pos`, `targetId`, `options`.

This endpoint remains available for simple integrations or mixed HTTP/WebSocket clients. WebSocket clients can send the same command JSON messages on `/ws` instead and can fetch the same cached resources over WebSocket without touching the HTTP endpoints.

**Match Start Command:**
- `set_start_pos`: Set your initial spawn point. Requires `pos`.
- `set_commander`: Select the initial commander loadout for this match. Requires `name`.

**Unit Commands:**
- `move`, `patrol`, `fight`, `attack`, `guard`, `repair`, `reclaim`, `resurrect`, `capture`, `stop`, `wait`, `self_destruct`.

**Special Commands:**
- `custom`: Trigger raw engine `cmdId`.
- `lua`: Execute a raw string in `LuaRules`.
- `finish_frame`: Resume engine execution (Synchronous Mode).

## Complete Match Lifecycle (Python Example)

This example shows the HTTP fallback flow. The README examples use Python consistently; for WebSocket transport, see the Python example above.

```python
import requests
import time

MASTER_URL = "http://localhost:3017"


def discover_instance(team_id):
    while True:
        listing = requests.get(f"{MASTER_URL}/list")
        listing.raise_for_status()
        listing = listing.json()

        instance = listing.get(str(team_id))
        if instance:
            return instance["httpUrl"]

        time.sleep(0.5)


def post_command(base_url, payload):
    requests.post(f"{base_url}/command", json=payload).raise_for_status()


def main():
    print("Waiting for match initialization...")
    url = discover_instance(team_id=0)

    # 1. Initialization Phase
    while True:
        try:
            obs = requests.get(f"{url}/observation")
            obs.raise_for_status()
            obs = obs.json()
            break
        except requests.RequestException:
            time.sleep(0.5)

    print(f"Match Loaded. My AllyTeam: {obs['allyTeamId']}")

    # 2. Match Start: Read startup metadata and choose setup
    info = requests.get(f"{url}/game_info")
    info.raise_for_status()
    info = info.json()
    print(f"Map: {info['mapName']} | Mode: {info['gameMode']} | CanChooseStartPos: {info['canChooseStartPos']}")

    # Startup commands can be sent before the first frame update.
    post_command(url, {
        "type": "set_commander",
        "name": "dyntrainer_strike_base"
    })

    if info["canChooseStartPos"]:
        boxes = requests.get(f"{url}/spawn_boxes")
        boxes.raise_for_status()
        boxes = boxes.json()
        my_box = boxes.get(str(obs['allyTeamId']))

        if my_box:
            cx = (my_box['left'] + my_box['right']) / 2
            cz = (my_box['top'] + my_box['bottom']) / 2
            print(f"Setting start position: {cx}, {cz}")
            post_command(url, {
                "type": "set_start_pos",
                "pos": [cx, 0, cz]
            })
        else:
            raise RuntimeError("Missing spawn box for ally team")
    else:
        post_command(url, {
            "type": "finish_frame"
        })

    # 3. Game Loop (Synchronous)
    while True:
        obs = requests.get(f"{url}/observation")
        obs.raise_for_status()
        obs = obs.json()
        frame = obs['frame']

        # --- YOUR AI LOGIC HERE ---
        if frame % 100 == 0:
            print(f"Processing Frame {frame}...")

        if obs['units']:
            first_unit_id = int(next(iter(obs['units'])))
            unit_def = requests.get(
                f"{url}/query",
                params={"type": "unit_def_by_unit_id", "unitId": first_unit_id},
            )
            unit_def.raise_for_status()
            unit_def = unit_def.json()["result"]
            print(f"First unit def: {unit_def['name']}")

        # Move units if any exist
        for uid in obs['units']:
            post_command(url, {
                "type": "move", "unitId": int(uid), "pos": [1000, 0, 1000]
            })

        # 4. Advance to next frame
        post_command(url, {"type": "finish_frame"})

if __name__ == "__main__":
    main()
```

## Performance Note
ControllerAI defaults to Synchronous Mode (`sync=true`) to ensure external services never miss a frame. For non-blocking "real-time" behavior, set `sync = false` in `AIOptions.lua`.

When using `/ws`, the expected loop is: receive an observation, send one or more commands for that frame, send `finish_frame`, then wait for the next observation.
