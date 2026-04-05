# ControllerAI: External AI Communication Layer

ControllerAI is a specialized AI module for the Recoil Engine (SpringRTS) that acts as a bridge between the game engine and external services. It exposes the game state via a RESTful API, streams observations over WebSocket, and accepts commands in JSON format, allowing you to write RTS AIs in any language (Python, Node.js, Rust, etc.) without compiling C++ code.

## How it Works

1.  **Engine Side**: ControllerAI runs as a standard Skirmish AI. It starts a background HTTP/WebSocket server on `localhost:3017`.
2.  **Observation**: Every game frame (and during the pre-match phase), the AI updates its internal state. External services can either poll `/observation` or use `ws://localhost:3017/ws` as a bidirectional frame loop.
3.  **WebSocket Bootstrap**: On connect, the WebSocket sends the cached startup data you would otherwise fetch from `/game_info`, `/spawn_boxes`, `/metadata`, `/map_features`, `/heightmap`, and then the latest observation if available.
4.  **Commands**: External services POST JSON commands to `/command` or send the same JSON payloads on `/ws`. These are executed on the next engine update.
5. **Synchronous Control**: By default, the engine blocks the main thread during the setup phase (frame -1) until a start position is chosen. Once the game starts, it pauses at the end of every frame until you send a `finish_frame` command.

## Configuration

You can configure the binding address and port through the engine's AI options (typically in the game setup or `AIOptions.lua`):

- **Binding Address (`ip`)**: The IP the server binds to. Default: `127.0.0.1`.
- **Server Port (`port`)**: The port the server listens on. Default: `3017`.
- **Synchronous Mode (`sync`)**: If `true`, the engine thread blocks at the end of every update until a `finish_frame` command is received. **Default: `true`**. Note that setup-phase blocking (frame -1) is mandatory regardless of this setting if the map requires choosing a start position.


## API Endpoints

The server runs on `http://localhost:3017` and `ws://localhost:3017/ws`.

### 1. `GET /observation`
Returns the current snapshot of the game state and events since the last poll.

For simple integrations, polling this endpoint is still supported and unchanged.

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
  "economy": { ... },
  "events": [
    { "topic": 2, "unitId": 1025, "builderId": 1024 }
  ]
}
```

### 2. `GET /metadata`
Returns static data about all unit types available in the current game/mod.

### 1b. `GET /ws`
Runs the full synchronous observation/command loop on a single WebSocket connection.

- Sends cached `game_info`, `spawn_boxes`, `metadata`, `map_features`, `heightmap`, and then the latest observation immediately after connect when available.
- Sends each subsequent observation when the engine publishes the next frame.
- Accepts the same JSON command payloads as `POST /command`.
- Supports either one command per message or an array of command objects in a single message.
- Supports websocket request messages for the HTTP resources: `get_all`, `get_observation`, `get_game_info`, `get_spawn_boxes`, `get_metadata`, `get_map_features`, and `get_heightmap`.
- In synchronous mode, send commands for the current frame and end the batch with `finish_frame`. The next observation is sent after the engine resumes and publishes it.

Server-to-client WebSocket messages are wrapped as:

```json
{
  "type": "observation",
  "data": { ... }
}
```

The `type` field will be one of `game_info`, `spawn_boxes`, `metadata`, `map_features`, `heightmap`, `observation`, or `error`.

**JavaScript example:**
```javascript
const ws = new WebSocket("ws://127.0.0.1:3017/ws");

const state = {
  gameInfo: null,
  spawnBoxes: null,
  metadata: null,
  mapFeatures: null,
  heightmap: null,
};

ws.onmessage = (event) => {
  const message = JSON.parse(event.data);

  if (message.type === "game_info") {
    state.gameInfo = message.data;
    console.log("map size", state.gameInfo.mapWidthElmos, state.gameInfo.mapHeightElmos);
    return;
  }

  if (message.type === "spawn_boxes") {
    state.spawnBoxes = message.data;
    return;
  }

  if (message.type === "metadata") {
    state.metadata = message.data;
    return;
  }

  if (message.type === "map_features") {
    state.mapFeatures = message.data;
    return;
  }

  if (message.type === "heightmap") {
    state.heightmap = message.data;
    return;
  }

  if (message.type !== "observation") {
    console.error(message);
    return;
  }

  const obs = message.data;
  console.log("frame", obs.frame);

  for (const unitId of Object.keys(obs.units)) {
    ws.send(JSON.stringify({
      type: "move",
      unitId: Number(unitId),
      pos: [1000, 0, 1000]
    }));
  }

  ws.send(JSON.stringify({ type: "finish_frame" }));
};
```

### 3. `GET /spawn_boxes`
Returns the defined start boxes for each ally team.
- **Keys**: Ally Team IDs.
- **Values**: `{ "left", "top", "right", "bottom" }` in map coordinates (elmos).

### 4. `GET /game_info`
Returns startup metadata for the current match.

Typical startup flow:
1. Poll `/observation` until the AI is initialized.
2. Read `/game_info` to check whether startup setup is still available.
3. If the game allows it, send `set_commander` before choosing a start position.
4. If `canChooseStartPos` is true, read `/spawn_boxes` and send `set_start_pos`. **This unblocks the engine setup phase.**
5. Once the match reaches frame 0, the engine will block again (if `sync=true`). Call `finish_frame` to proceed with the game loop.

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

### 7. `POST /command`
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

This example shows the HTTP fallback flow. With `/ws`, you can connect once, receive the startup data over WebSocket, choose a start position, and run the frame loop without touching the REST endpoints.

```python
import requests
import time

URL = "http://localhost:3017"

def main():
    print("Waiting for match initialization...")
    
    # 1. Initialization Phase
    while True:
        try:
            obs = requests.get(f"{URL}/observation").json()
            break
        except:
            time.sleep(0.5)
    
    print(f"Match Loaded. My AllyTeam: {obs['allyTeamId']}")

    # 2. Match Start: Read startup metadata and choose setup
    info = requests.get(f"{URL}/game_info").json()
    print(f"Map: {info['mapName']} | Mode: {info['gameMode']} | CanChooseStartPos: {info['canChooseStartPos']}")

    boxes = requests.get(f"{URL}/spawn_boxes").json()
    my_box = boxes.get(str(obs['allyTeamId']))
    
    if my_box:
        # Pick center of your start box
        cx = (my_box['left'] + my_box['right']) / 2
        cz = (my_box['top'] + my_box['bottom']) / 2
        print(f"Setting start position: {cx}, {cz}")
        requests.post(f"{URL}/command", json={
            "type": "set_start_pos",
            "pos": [cx, 0, cz]
        })

    # Commander selection can be sent during startup as well.
    requests.post(f"{URL}/command", json={
        "type": "set_commander",
        "name": "dyntrainer_strike_base"
    })
    
    # 3. Game Loop (Synchronous)
    while True:
        obs = requests.get(f"{URL}/observation").json()
        frame = obs['frame']
        
        # --- YOUR AI LOGIC HERE ---
        if frame % 100 == 0:
            print(f"Processing Frame {frame}...")

        # Move units if any exist
        for uid in obs['units']:
            requests.post(f"{URL}/command", json={
                "type": "move", "unitId": int(uid), "pos": [1000, 0, 1000]
            })
        
        # 4. Advance to next frame
        requests.post(f"{URL}/command", json={"type": "finish_frame"})

if __name__ == "__main__":
    main()
```

## Performance Note
ControllerAI defaults to Synchronous Mode (`sync=true`) to ensure external services never miss a frame. For non-blocking "real-time" behavior, set `sync = false` in `AIOptions.lua`.

When using `/ws`, the expected loop is: receive an observation, send one or more commands for that frame, send `finish_frame`, then wait for the next observation.
