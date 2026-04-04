# ControllerAI: External AI Communication Layer

ControllerAI is a specialized AI module for the Recoil Engine (SpringRTS) that acts as a bridge between the game engine and external services. It exposes the game state via a RESTful API and accepts commands in JSON format, allowing you to write RTS AIs in any language (Python, Node.js, Rust, etc.) without compiling C++ code.

## How it Works

1.  **Engine Side**: ControllerAI runs as a standard Skirmish AI. It starts a background HTTP server on `localhost:3017`.
2.  **Observation**: Every game frame (and during the pre-match phase), the AI updates its internal state. External services poll this state via `/observation`.
3.  **Commands**: External services POST JSON commands to `/command`. These are executed on the next engine update.
4.  **Synchronous Control**: By default, the engine pauses at the end of every frame until you send a `finish_frame` command.

## Configuration

You can configure the binding address and port through the engine's AI options (typically in the game setup or `AIOptions.lua`):

- **Binding Address (`ip`)**: The IP the server binds to. Default: `127.0.0.1`.
- **Server Port (`port`)**: The port the server listens on. Default: `3017`.
- **Synchronous Mode (`sync`)**: If `true`, the engine thread blocks at the end of every update until a `finish_frame` command is received. **Default: `true`**.

## API Endpoints

The server runs on `http://localhost:3017`.

### 1. `GET /observation`
Returns the current snapshot of the game state and events since the last poll.

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

### 3. `GET /spawn_boxes`
Returns the defined start boxes for each ally team.
- **Keys**: Ally Team IDs.
- **Values**: `{ "left", "top", "right", "bottom" }` in map coordinates (elmos).

### 4. `GET /map_features`
Returns resource spots and map features (trees, rocks, etc.).

### 5. `GET /heightmap`
Returns the dynamic heightmap as a Base64 encoded string of `float32` values.

### 6. `POST /command`
Sends a command to the engine. Standard fields: `unitId`, `type`, `pos`, `targetId`, `options`.

**Match Start Command:**
- `set_start_pos`: Set your initial spawn point. Requires `pos`.

**Unit Commands:**
- `move`, `patrol`, `fight`, `attack`, `guard`, `repair`, `reclaim`, `resurrect`, `capture`, `stop`, `wait`, `self_destruct`.

**Special Commands:**
- `custom`: Trigger raw engine `cmdId`.
- `lua`: Execute a raw string in `LuaRules`.
- `finish_frame`: Resume engine execution (Synchronous Mode).

## Complete Match Lifecycle (Python Example)

This example shows how to wait for the match to initialize, choose a start position within your box, and run a synchronous game loop.

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

    # 2. Match Start: Choose Position
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
