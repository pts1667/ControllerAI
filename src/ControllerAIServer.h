#ifndef CONTROLLERAI_SERVER_H
#define CONTROLLERAI_SERVER_H

#include "httplib.h"
#include "nlohmann/json.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace controllerai {

using json = nlohmann::json;

class CControllerAIServer {
public:
    CControllerAIServer(std::string bindAddress, int port);
    ~CControllerAIServer();

    void Start();
    void Stop();

    void PublishObservation(json observation);
    void PublishMetadata(json metadata);
    void PublishGameInfo(json gameInfo);
    void PublishSpawnBoxes(json spawnBoxes);
    void PublishMapFeatures(json mapFeatures);
    void PublishHeightmap(json heightmap);

    json GetSpawnBoxes() const;

    std::vector<json> DrainCommands();
    bool WaitForCommands();

private:
    void ConfigureRoutes();
    void Run();

    std::string bindAddress;
    int port;

    httplib::Server svr;
    std::thread serverThread;
    std::atomic<bool> running;

    mutable std::mutex snapshotMutex;
    json lastObservation;
    json unitDefsCache;
    json gameInfoCache;
    json spawnBoxesCache;
    json mapFeaturesCache;
    json heightmapCache;

    std::mutex commandMutex;
    std::condition_variable commandCv;
    std::queue<json> commandQueue;
};

} // namespace controllerai

#endif
