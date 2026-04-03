#ifndef _CPPTESTAI_CPPTESTAI_H
#define _CPPTESTAI_CPPTESTAI_H

#include "OOAICallback.h"
#include "httplib.h"
#include "nlohmann/json.hpp"

#include <thread>
#include <mutex>
#include <vector>
#include <string>
#include <queue>
#include <map>
#include <atomic>

namespace controllerai {

using json = nlohmann::json;

class CControllerAI {
private:
    springai::OOAICallback* callback;
    int skirmishAIId;

    // HTTP Server
    httplib::Server svr;
    std::thread serverThread;
    
    // State management
    std::mutex stateMutex;
    json lastObservation;
    std::atomic<bool> running;

    // Command Queue
    std::mutex commandQueueMutex;
    std::queue<json> commandQueue;

    // Helper for event serialization
    json EventToJson(int topic, const void* data);
    
    // Internal processing
    void ServerThread();
    void UpdateObservation();
    void ProcessCommands();
    
    // Metadata cache
    json unitDefsCache;
    void CacheMetadata();

public:
    CControllerAI(springai::OOAICallback* callback);
    ~CControllerAI();

    int HandleEvent(int topic, const void* data);
};

} // namespace controllerai

#endif
