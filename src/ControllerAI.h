#ifndef _CPPTESTAI_CPPTESTAI_H
#define _CPPTESTAI_CPPTESTAI_H

#include "OOAICallback.h"
#include "nlohmann/json.hpp"

#include "ControllerAIServer.h"

#include "Game.h"
#include "Map.h"
#include "Log.h"
#include "SkirmishAI.h"
#include "Economy.h"
#include "Lua.h"
#include "Mod.h"

#include <memory>
#include <string>
#include <tuple>
#include <vector>

namespace controllerai {

using json = nlohmann::json;

class CControllerAI {
private:
    springai::OOAICallback* callback;
    int skirmishAIId;

    // Engine Objects
    std::unique_ptr<springai::Game> game;
    std::unique_ptr<springai::Map> map;
    std::unique_ptr<springai::Log> log;
    std::unique_ptr<springai::SkirmishAI> skirmishAI;
    std::unique_ptr<springai::Economy> economy;
    std::unique_ptr<springai::Lua> lua;
    std::unique_ptr<springai::Mod> mod;

    // HTTP Server
    std::string bindAddress;
    int port;
    std::unique_ptr<CControllerAIServer> server;

    // State management
    json eventBuffer; // Stores events since last observation poll
    bool running;
    bool synchronousMode;
    bool setupComplete;
    bool canChooseStartPos;
    bool frameFinished;

    // Helper for event serialization
    json EventToJson(int topic, const void* data);
    
    // Internal processing
    void UpdateObservation();
    void ProcessCommands();
    void WaitForResume();
    
    void CacheStaticData();
    std::tuple<int, json> ParseUnit(springai::Unit* unit);
    std::tuple<int, json> ParseRadarBlip(springai::Unit* unit);

    bool IsSpawnPosValid(const springai::AIFloat3& pos);

    std::string SafeCString(const char* value) const;
    std::string Base64Encode(const unsigned char* data, size_t len);

public:
    CControllerAI(springai::OOAICallback* callback);
    ~CControllerAI();

    int HandleEvent(int topic, const void* data);
};

} // namespace controllerai

#endif
