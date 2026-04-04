# AI Agent Guide: ControllerAI & Recoil Engine

This document maps the relevant codebases and files for navigating, understanding, and extending the **ControllerAI** communication layer.

## 1. ControllerAI (Current Repository)
The primary implementation of the HTTP forwarding layer.

- **Core Logic**:
    - `src/ControllerAI.h`: Class definition, state management, and HTTP server members.
    - `src/ControllerAI.cpp`: Implementation of observations, command processing, and the HTTP endpoints.
- **Engine Interface**:
    - `src/AIExport.h` / `src/AIExport.cpp`: Standard Spring Skirmish AI entry points (`init`, `release`, `handleEvent`).
- **Configuration**:
    - `data/AIInfo.lua`: AI metadata (name, version, description).
    - `data/AIOptions.lua`: User-visible options (IP, Port, Synchronous Mode).
- **Build**:
    - `CMakeLists.txt`: Fetches `nlohmann/json` and `cpp-httplib`, handles cross-platform linking (e.g., `ws2_32` on Windows).

## 2. Recoil Engine (Parent Base)
Location: `D:/sources/RecoilEngine`

### Critical Interfaces (C-Level)
Located in `rts/ExternalAI/Interface/`:
- `SSkirmishAICallback.h`: The main interface from the engine to the AI (querying units, map, economy).
- `AISEvents.h`: Definitions of all `EVENT_*` topics and their data structures.
- `AISCommands.h`: Definitions of all `COMMAND_*` topics and their data structures.

### C++ OO Wrapper (Generated)
Located in `build/AI/Wrappers/Cpp/src-generated/`:
- `OOAICallback.h`: Object-oriented interface used by ControllerAI.
- `Unit.h` / `WrappUnit.h`: Unit properties and order methods.
- `Map.h` / `WrappMap.h`: Heightmap, resource spots, and build-site functions.
- `Game.h` / `WrappGame.h`: Global game state and start position controls.

### Peer AIs (Reference)
Located in `AI/Skirmish/`:
- `CircuitAI/`: A high-level complex AI used as a reference for comprehensive event/command handling.
- `BARb/`: Reference for advanced combat and strategic logic.

## 3. Communication Protocol
ControllerAI implements a REST API on port `3017` (default). 
- **Observations**: JSON snapshots of the engine state.
- **Commands**: JSON representations of engine commands.
- **Synchronization**: Thread-safe synchronization between the Engine's main simulation thread and the AI's HTTP server thread.
