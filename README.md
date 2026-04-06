# ControllerAI: External AI Communication Layer

ControllerAI is a specialized AI module for the Recoil Engine (SpringRTS) that acts as a bridge between the game engine and external services. It exposes the game state via a RESTful API, streams observations over WebSocket, and accepts commands in JSON format, allowing you to write RTS AIs in any language (Python, Node.js, Rust, etc.) without compiling C++ code.

## How it Works

1.  **Engine Side**: ControllerAI runs as a standard Skirmish AI. It starts a background HTTP/WebSocket server on `localhost:3017`.
2.  **Observation**: During the pre-match phase the AI always updates its internal state immediately. After frame 0, observations are published every `block_n_frames` frames, so external services can either poll `/observation` or use `ws://localhost:3017/ws` as a bidirectional batched frame loop.
3.  **WebSocket Bootstrap**: On connect, the WebSocket sends the cached startup data you would otherwise fetch from `/game_info`, `/spawn_boxes`, `/metadata`, `/map_features`, `/heightmap`, `/settings`, and then the latest observation if available.
4.  **Commands**: External services POST JSON commands to `/command` or send the same JSON payloads on `/ws`. These are executed on the next engine update.
5.  **Queries**: External services can call `GET /query` or send `type="query"` messages on `/ws` to execute engine-thread lookups such as full `UnitDef` serialization, resource spot lookups, and build-site checks without changing frame synchronization.
6.  **Runtime Settings**: External services can call `GET /settings`, `POST /settings`, or use the matching WebSocket messages to change runtime behavior such as `block_n_frames`.
7. **Synchronous Control**: By default, the engine can already block during startup handling so setup commands such as `set_commander`, `set_side`, and `set_start_pos` can be processed before the match proceeds. Before the first `EVENT_UPDATE`, the AI stays in a startup command loop. If the game requires choosing a start position, a valid `set_start_pos` unblocks startup. Otherwise, send `finish_frame` after your startup commands to let the match proceed. Once the game starts, it pauses at the end of each published observation batch until you send a `finish_frame` command.

## Configuration

You can configure the binding address and port through the engine's AI options (typically in the game setup or `AIOptions.lua`):

- **Binding Address (`ip`)**: The IP the server binds to. Default: `127.0.0.1`.
- **Server Port (`port`)**: The port the server listens on. Default: `3017`.
- **Synchronous Mode (`sync`)**: If `true`, the engine thread blocks at the end of every update until a `finish_frame` command is received. **Default: `true`**. Note that setup-phase blocking (frame -1) is mandatory regardless of this setting if the map requires choosing a start position.


## API Endpoints

The server runs on `http://localhost:3017` and `ws://localhost:3017/ws`.

### 1. `GET /observation`
Returns the current snapshot of the game state and events accumulated since the last published observation.

For simple integrations, polling this endpoint is still supported and unchanged.

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

**Python WebSocket example:**
```python
import json

from websocket import create_connection

WS_URL = "ws://127.0.0.1:3017/ws"


def send(ws, payload):
    ws.send(json.dumps(payload))


def main():
    ws = create_connection(WS_URL)
    state = {
        "game_info": None,
        "spawn_boxes": None,
        "metadata": None,
        "map_features": None,
        "heightmap": None,
        "startup_complete": False,
    }

    try:
        while True:
            message = json.loads(ws.recv())
            msg_type = message["type"]
            data = message["data"]

            if msg_type in {"game_info", "spawn_boxes", "metadata", "map_features", "heightmap"}:
                state[msg_type] = data
                continue

            if msg_type == "error":
                print("Server error:", data)
                continue

            if msg_type == "query_result":
                print("Query result:", data)
                continue

            if msg_type == "settings":
                print("Current settings:", data)
                continue

            if msg_type == "settings_result":
                print("Settings updated:", data)
                continue

            if msg_type != "observation":
                continue

            frame = data["frame"]
            print("frame", frame)

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
                        raise RuntimeError("Missing spawn box for ally team")
                else:
                    send(ws, {"type": "finish_frame"})

                state["startup_complete"] = True
                continue

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
                    "requestId": f"unitdef-{first_unit_id}",
                    "query": {
                        "type": "unit_def_by_unit_id",
                        "unitId": first_unit_id,
                    },
                })

            send(ws, {"type": "finish_frame"})
    finally:
        ws.close()


if __name__ == "__main__":
    main()
```

### 3. `GET /spawn_boxes`
Returns the defined start boxes for each ally team.
- **Keys**: Ally Team IDs.
- **Values**: `{ "left", "top", "right", "bottom" }` in map coordinates (elmos).

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
  "mapName": "TitanDuel 2.2",
  "mapWidth": 1024,
  "mapHeight": 1024,
  "mapWidthElmos": 8192,
  "mapHeightElmos": 8192,
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
- `unit_def_id_by_name`: requires `name`.
- `unit_def_by_id`: requires `unitDefId`. Returns the full serialized `UnitDef` table.
- `unit_def_by_name`: requires `name`. Returns the full serialized `UnitDef` table.
- `unit_def_id_by_unit_id`: requires `unitId`.
- `unit_def_by_unit_id`: requires `unitId`. Returns the full serialized `UnitDef` table for that unit's current definition.
- `unit_by_id`: requires `unitId`. Returns an expanded unit snapshot including its full serialized `definition`.
- `feature_by_id`: requires `featureId`.
- `feature_def_by_id`: requires `featureDefId`.
- `feature_def_by_name`: requires `name`.
- `resource_by_name`: requires `name`.
- `resource_spots_by_name`: requires `name`.
- `nearest_resource_spot`: requires `name` plus `pos` or `x`/`z`.
- `elevation_at`: requires `pos` or `x`/`z`.
- `start_position`: no extra parameters.
- `can_build_at`: requires `unitDefId` plus `pos` or `x`/`z`; optional `facing`.
- `closest_build_site`: requires `unitDefId`, `searchRadius`, and `pos` or `x`/`z`; optional `minDist` and `facing`.

**HTTP examples:**

```text
GET /query?type=unit_def_id_by_name&name=cloakraid
GET /query?type=unit_def_by_id&unitDefId=52
GET /query?type=unit_def_by_unit_id&unitId=1024
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

URL = "http://localhost:3017"


def post_command(payload):
    requests.post(f"{URL}/command", json=payload).raise_for_status()


def main():
    print("Waiting for match initialization...")

    # 1. Initialization Phase
    while True:
        try:
            obs = requests.get(f"{URL}/observation")
            obs.raise_for_status()
            obs = obs.json()
            break
        except requests.RequestException:
            time.sleep(0.5)

    print(f"Match Loaded. My AllyTeam: {obs['allyTeamId']}")

    # 2. Match Start: Read startup metadata and choose setup
    info = requests.get(f"{URL}/game_info")
    info.raise_for_status()
    info = info.json()
    print(f"Map: {info['mapName']} | Mode: {info['gameMode']} | CanChooseStartPos: {info['canChooseStartPos']}")

    # Startup commands can be sent before the first frame update.
    post_command({
        "type": "set_commander",
        "name": "dyntrainer_strike_base"
    })

    if info["canChooseStartPos"]:
        boxes = requests.get(f"{URL}/spawn_boxes")
        boxes.raise_for_status()
        boxes = boxes.json()
        my_box = boxes.get(str(obs['allyTeamId']))

        if my_box:
            cx = (my_box['left'] + my_box['right']) / 2
            cz = (my_box['top'] + my_box['bottom']) / 2
            print(f"Setting start position: {cx}, {cz}")
            post_command({
                "type": "set_start_pos",
                "pos": [cx, 0, cz]
            })
        else:
            raise RuntimeError("Missing spawn box for ally team")
    else:
        post_command({
            "type": "finish_frame"
        })

    # 3. Game Loop (Synchronous)
    while True:
        obs = requests.get(f"{URL}/observation")
        obs.raise_for_status()
        obs = obs.json()
        frame = obs['frame']

        # --- YOUR AI LOGIC HERE ---
        if frame % 100 == 0:
            print(f"Processing Frame {frame}...")

        if obs['units']:
            first_unit_id = int(next(iter(obs['units'])))
            unit_def = requests.get(
                f"{URL}/query",
                params={"type": "unit_def_by_unit_id", "unitId": first_unit_id},
            )
            unit_def.raise_for_status()
            unit_def = unit_def.json()["result"]
            print(f"First unit def: {unit_def['name']}")

        # Move units if any exist
        for uid in obs['units']:
            post_command({
                "type": "move", "unitId": int(uid), "pos": [1000, 0, 1000]
            })

        # 4. Advance to next frame
        post_command({"type": "finish_frame"})

if __name__ == "__main__":
    main()
```

## Performance Note
ControllerAI defaults to Synchronous Mode (`sync=true`) to ensure external services never miss a frame. For non-blocking "real-time" behavior, set `sync = false` in `AIOptions.lua`.

When using `/ws`, the expected loop is: receive an observation, send one or more commands for that frame, send `finish_frame`, then wait for the next observation.
