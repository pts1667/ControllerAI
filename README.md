# ControllerAI: External AI Communication Layer

ControllerAI is a specialized AI module for the Recoil Engine (SpringRTS) that acts as a bridge between the game engine and external services. It exposes the game state via a RESTful API and accepts commands in JSON format, allowing you to write RTS AIs in any language (Python, Node.js, Rust, etc.) without compiling C++ code.

## How it Works

1.  **Engine Side**: ControllerAI runs as a standard Skirmish AI. It starts a background HTTP server on `localhost:3017`.
2.  **Observation**: Every game frame (or on specific events), the AI updates its internal state. External services can poll this state.
3.  **Commands**: External services POST JSON commands to the AI. These commands are queued and executed on the next engine update.

## Configuration

You can configure the binding address and port through the engine's AI options (typically in the game setup or `AIOptions.lua`):

- **Binding Address (`ip`)**: The IP the server binds to. Default: `0.0.0.0`.
- **Server Port (`port`)**: The port the server listens on. Default: `3017`.
- **Synchronous Mode (`sync`)**: If `true`, the engine thread blocks at the end of every update until a `finish_frame` command is received. Default: `false`.

## API Endpoints

The server runs on `http://localhost:3017`.

### 1. `GET /observation`
Returns the current snapshot of the game state and events since the last poll.

**Example Response:**
```json
{
  "frame": 450,
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
  "enemies": {
    "2048": {
      "pos": [900.0, 0.0, 850.0],
      "defId": 60,
      "health": 500.0
    }
  },
  "economy": {
    "metal": { "current": 500.0, "storage": 1000.0, "income": 10.5, "usage": 5.0 },
    "energy": { "current": 2000.0, "storage": 2000.0, "income": 50.0, "usage": 20.0 }
  },
  "events": [
    { "topic": 2, "unitId": 1025, "builderId": 1024 },
    { "topic": 5, "unitId": 1020, "attackerId": 2048 }
  ]
}
```

### 2. `GET /metadata`
Returns static data about all unit types available in the current game/mod.

### 3. `GET /map_features`
Returns positions of resource spots (metal/energy) and map features (trees, rocks, wrecks).

### 4. `GET /heightmap`
Returns the current dynamic heightmap of the map.
- **width**, **height**: Dimensions of the heightmap.
- **data_b64**: Base64 encoded string of raw `float32` values. 

### 5. `POST /command`
Sends one or more commands to the game engine.

**Standard Command Fields:**
- `unitId` (integer): The unit to receive the command.
- `options` (integer, optional): Bitmask (1=Shift/Queue, 2=RightClick, 4=Alt, 8=Ctrl).
- `pos` (array of 3 floats, optional): Target position `[x, y, z]`.

**Command Types:**
- `move`, `patrol`, `fight`: Requires `pos`.
- `attack`, `guard`, `repair`, `capture`: Requires `targetId`.
- `stop`, `wait`, `self_destruct`: No extra fields required.
- `build`: Requires `defId`, `pos`, and optional `facing` (0-3).
- `reclaim`: Requires either `targetId` or `featureId`.
- `resurrect`: Requires `featureId`.
- **`custom`**: Directly trigger an engine `cmdId`. Requires `cmdId` (int) and `params` (array of floats).
- **`lua`**: Execute a raw string in `LuaRules`. Requires `data` (string).
- **`finish_frame`**: Signals the engine to proceed to the next frame (when in Synchronous mode).

## Getting Started (Python Example, non-synchronous)

Note: Synchronous mode is the default; without changing AIOptions.lua, this example will hang.

```python
import requests
import time

URL = "http://localhost:3017"

def main():
    while True:
        # 1. Get State
        obs = requests.get(f"{URL}/observation").json()
        print(f"Frame: {obs['frame']}")

        # 2. Logic (Example: Move first unit to center)
        if obs['units']:
            first_id = list(obs['units'].keys())[0]
            requests.post(f"{URL}/command", json={
                "type": "move",
                "unitId": int(first_id),
                "pos": [1000, 0, 1000]
            })

        time.sleep(1) # Poll every second

if __name__ == "__main__":
    main()
```

## Synchronous Mode Example (Step-by-Step)

When `sync` is enabled (which is the default) in `AIOptions.lua`, the engine thread blocks at the end of every update until a `finish_frame` command is received. This is ideal for Reinforcement Learning or precise debugging.

```python
import requests

URL = "http://localhost:3017"

def run_step():
    # 1. Get current state (Engine is waiting for us)
    obs = requests.get(f"{URL}/observation").json()
    print(f"Processing Frame: {obs['frame']}")

    # 2. Issue multiple commands
    if obs['units']:
        for uid in obs['units']:
            requests.post(f"{URL}/command", json={
                "type": "move", 
                "unitId": int(uid), 
                "pos": [1000, 0, 1000]
            })

    # 3. Tell the engine we are done with this frame
    # Engine will process commands and advance to next frame
    requests.post(f"{URL}/command", json={"type": "finish_frame"})

if __name__ == "__main__":
    while True:
        run_step()
```

## Performance Note
ControllerAI is designed for flexibility. For ultra-high-performance AIs requiring sub-millisecond latency, consider implementing your logic directly in C++ using the `CircuitAI` or `BARb` patterns. For most RTS automation and experimental AI, the HTTP overhead is negligible compared to the 30 FPS game simulation.
