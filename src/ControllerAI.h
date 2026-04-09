#ifndef _CPPTESTAI_CPPTESTAI_H
#define _CPPTESTAI_CPPTESTAI_H

#include "nlohmann/json.hpp"

#include <memory>
#include <string>

namespace springai {
class OOAICallback;
class Game;
class Map;
class Log;
class SkirmishAI;
class Economy;
class Lua;
class Mod;
struct AIFloat3;
}

namespace controllerai {

using json = nlohmann::json;

class CControllerAIServer;

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
    int masterPort;
    std::unique_ptr<CControllerAIServer> server;

    // State management
    json eventBuffer; // Stores events since last observation poll
    bool running;
    bool synchronousMode;
    bool setupComplete;
    bool canChooseStartPos;
    bool frameFinished;
    bool startupBlocking;
    bool released;
    int blockNFrames;

    void EnsureInterfacesInitialized();

    // Internal processing
    void UpdateObservation();
    void ProcessCommands();
    void ProcessQueries();
    void ProcessSettings();
    void WaitForResume();
    void Release(int reason);
    json HandleQuery(const json& query);
    json HandleSettings(const json& settingsPatch);
    json GetSettings() const;
    bool ShouldPublishObservationForFrame(int frame) const;

    void CacheStaticData();

    bool IsSpawnPosValid(const springai::AIFloat3& pos);

public:
    CControllerAI(springai::OOAICallback* callback, std::string bindAddress, int port, int masterPort);
    ~CControllerAI();

    int HandleEvent(int topic, const void* data);
};

} // namespace controllerai

#endif
