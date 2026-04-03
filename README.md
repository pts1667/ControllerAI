# ControllerAI: External AI Communication Layer

ControllerAI is a specialized AI module for the Recoil Engine (SpringRTS) that acts as a bridge between the game engine and external services. It exposes the game state via a RESTful API and accepts commands in JSON format, allowing you to write RTS AIs in any language (Python, Node.js, Rust, etc.) without compiling C++ code.

## How it Works

1.  **Engine Side**: ControllerAI runs as a standard Skirmish AI. It starts a background HTTP server on `localhost:3017`.
2.  **Observation**: Every game frame (or on specific events), the AI updates its internal state. External services can poll this state.
3.  **Commands**: External services POST JSON commands to the AI. These commands are queued and executed on the next engine update.

## API Endpoints

The server runs on `http://localhost:3017`.

### 1. `GET /observation`
Returns the current snapshot of the game state.

**Example Response:**
```json
{
  "frame": 450,
  "units": {
    "1024": {
      "defId": 52,
      "health": 1200.0,
      "pos": [150.5, 12.0, 300.2]
    }
  },
  "enemies": {
    "2048": {
      "pos": [900.0, 0.0, 850.0]
    }
  },
  "economy": {
    "metal": {
      "current": 500.0,
      "storage": 1000.0,
      "income": 10.5,
      "usage": 5.0
    },
    "energy": { "current": 2000.0, "storage": 2000.0, "income": 50.0, "usage": 20.0 }
  }
}
```

### 2. `GET /metadata`
Returns static data about all unit types available in the current game/mod. Useful for mapping `defId` to human-readable names and stats.

### 3. `POST /command`
Sends a command to the game engine.

**Move Command:**
```json
{
  "type": "move",
  "unitId": 1024,
  "pos": [500.0, 0.0, 500.0]
}
```

**Attack Command:**
```json
{
  "type": "attack",
  "unitId": 1024,
  "targetId": 2048
}
```

**Build Command:**
```json
{
  "type": "build",
  "unitId": 1024,
  "defId": 60,
  "pos": [200.0, 0.0, 200.0]
}
```

**Lua/Custom Command:**
Directly call `LuaRules` (Gadgets) with a raw string.
```json
{
  "type": "lua",
  "data": "custom_gadget_event:target=1024"
}
```

## Getting Started (Python Example)

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

## Requirements for Compilation
To compile ControllerAI from source, ensure the following headers are in your include path:
- `httplib.h` from [cpp-httplib](https://github.com/yhirose/cpp-httplib)
- `nlohmann/json.hpp` from [nlohmann/json](https://github.com/nlohmann/json)

## Performance Note
ControllerAI is designed for flexibility. For ultra-high-performance AIs requiring sub-millisecond latency, consider implementing your logic directly in C++ using the `CircuitAI` or `BARb` patterns. For most RTS automation and experimental AI, the HTTP overhead is negligible compared to the 30 FPS game simulation.
