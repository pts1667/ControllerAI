#ifndef CONTROLLERAI_SERVER_H
#define CONTROLLERAI_SERVER_H

#include "httplib.h"
#include "nlohmann/json.hpp"

#include <memory>
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
    struct WebSocketSession {
        std::mutex mutex;
        std::condition_variable cv;
        std::queue<std::string> outgoingMessages;
        bool closed = false;
    };

    void ConfigureRoutes();
    void Run();
    std::string BuildWebSocketMessage(const std::string& type, const json& data) const;
    void EnqueueCommand(json command);
    void EnqueueWebSocketMessage(const std::shared_ptr<WebSocketSession>& session, const std::string& message);
    void BroadcastWebSocketMessage(const std::string& message);
    void SendCurrentStateToWebSocketSession(const std::shared_ptr<WebSocketSession>& session);
    void HandleWebSocketPayload(const std::shared_ptr<WebSocketSession>& session, json payload);
    void HandleWebSocketCommandOrRequest(const std::shared_ptr<WebSocketSession>& session, json payload);
    void RemoveWebSocketSession(const std::shared_ptr<WebSocketSession>& session);

    std::string bindAddress;
    int port;

    httplib::Server svr;
    std::thread serverThread;
    std::atomic<bool> running;

    mutable std::mutex snapshotMutex;
    json lastObservation;
    std::string lastObservationSerialized;
    json unitDefsCache;
    json gameInfoCache;
    json spawnBoxesCache;
    json mapFeaturesCache;
    json heightmapCache;

    std::mutex websocketMutex;
    std::vector<std::shared_ptr<WebSocketSession>> websocketSessions;

    std::mutex commandMutex;
    std::condition_variable commandCv;
    std::queue<json> commandQueue;
};

} // namespace controllerai

#endif
