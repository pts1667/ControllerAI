#ifndef _CPPTESTAI_CPPTESTAI_H
#define _CPPTESTAI_CPPTESTAI_H

#include "OOAICallback.h"
#include "httplib.h"
#include "nlohmann/json.hpp"

#include "Game.h"
#include "Map.h"
#include "Log.h"
#include "SkirmishAI.h"
#include "Economy.h"
#include "Lua.h"
#include "Mod.h"

#include <thread>
#include <mutex>
#include <vector>
#include <string>
#include <queue>
#include <map>
#include <atomic>
#include <condition_variable>
#include <memory>

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
    httplib::Server svr;
    std::thread serverThread;
    std::string bindAddress;
    int port;
    
    // State management
    std::mutex stateMutex;
    json lastObservation;
    json eventBuffer; // Stores events since last observation poll
    bool running;
    bool synchronousMode;
    bool setupComplete;
    bool canChooseStartPos;

    // Synchronous Mode control
    std::condition_variable cv;
    std::mutex cvMutex;
    bool frameFinished;

    // Command Queue
    std::mutex commandQueueMutex;
    std::queue<json> commandQueue;

    // Helper for event serialization
    json EventToJson(int topic, const void* data);
    
    // Internal processing
    void ServerThread();
    void UpdateObservation();
    void ProcessCommands();
    
    // Metadata and static-ish caches
    json unitDefsCache;
    json gameInfoCache;
    json spawnBoxesCache;
    json mapFeaturesCache;
    json heightmapCache;
    void CacheStaticData();

    bool IsSpawnPosValid(const springai::AIFloat3& pos);

    std::string Base64Encode(const unsigned char* data, size_t len);

public:
    CControllerAI(springai::OOAICallback* callback);
    ~CControllerAI();

    int HandleEvent(int topic, const void* data);
};

} // namespace controllerai

#endif
