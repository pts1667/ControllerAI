#ifndef CONTROLLERAI_SERVER_H
#define CONTROLLERAI_SERVER_H

// hack needed for cross-compile with mingw
#pragma push_macro("_WIN32_WINNT")
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00 // Windows 10 or later
#include "httplib.h"
#pragma pop_macro("_WIN32_WINNT")

#include "nlohmann/json.hpp"

#include <memory>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace controllerai {

using json = nlohmann::json;

class CControllerAIServer {
public:
    struct PendingWorkState {
        bool hasCommands = false;
        bool hasQueries = false;
        bool hasSettings = false;

        bool HasAny() const {
            return hasCommands || hasQueries || hasSettings;
        }
    };

    CControllerAIServer(std::string bindAddress, int port, std::function<void(const std::string&)> infologSink = {});
    ~CControllerAIServer();

    void Start();
    void Stop();

    void PublishObservation(json observation);
    void PublishMetadata(json metadata);
    void PublishGameInfo(json gameInfo);
    void PublishSpawnBoxes(json spawnBoxes);
    void PublishMapFeatures(json mapFeatures);
    void PublishHeightmap(json heightmap);
    void PublishSettings(json settings);

    json GetSpawnBoxes() const;
    json GetSettings() const;

    json ExecuteQuery(json query);
    void ProcessQueries(const std::function<json(const json&)>& handler);
    json ExecuteSettings(json settings);
    void ProcessSettings(const std::function<json(const json&)>& handler);
    std::vector<json> DrainCommands();
    bool WaitForWork();
    bool WaitForWorkFor(int timeoutMs);
    PendingWorkState GetPendingWorkState();

private:
    struct WebSocketSession {
        std::mutex mutex;
        std::condition_variable cv;
        std::queue<std::string> outgoingMessages;
        bool closed = false;
    };

    struct QueryRequest {
        std::mutex mutex;
        std::condition_variable cv;
        json query;
        json response;
        std::string error;
        bool completed = false;
    };

    struct SettingsRequest {
        std::mutex mutex;
        std::condition_variable cv;
        json settings;
        json response;
        std::string error;
        bool completed = false;
    };

    void ConfigureRoutes();
    void Run();
    std::string BuildWebSocketMessage(const std::string& type, const json& data) const;
    json BuildHttpQuery(const httplib::Request& req) const;
    json BuildHttpSettingsPayload(const httplib::Request& req) const;
    void CompleteQuery(const std::shared_ptr<QueryRequest>& request, json response, const std::string& error = std::string());
    void CompleteSettings(const std::shared_ptr<SettingsRequest>& request, json response, const std::string& error = std::string());
    void EnqueueCommand(json command);
    void EnqueueWebSocketMessage(const std::shared_ptr<WebSocketSession>& session, const std::string& message);
    void BroadcastWebSocketMessage(const std::string& message);
    void SendCurrentStateToWebSocketSession(const std::shared_ptr<WebSocketSession>& session);
    void HandleWebSocketPayload(const std::shared_ptr<WebSocketSession>& session, json payload);
    void HandleWebSocketCommandOrRequest(const std::shared_ptr<WebSocketSession>& session, json payload);
    void RemoveWebSocketSession(const std::shared_ptr<WebSocketSession>& session);
    void LogToInfolog(const std::string& message) const;
    void ReportWebSocketError(const std::shared_ptr<WebSocketSession>& session, const std::string& message, bool enqueueClientError = true);

    std::function<void(const std::string&)> infologSink;
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
    json settingsCache;

    std::mutex websocketMutex;
    std::vector<std::shared_ptr<WebSocketSession>> websocketSessions;

    std::mutex commandMutex;
    std::condition_variable commandCv;
    std::queue<json> commandQueue;

    std::mutex queryMutex;
    std::queue<std::shared_ptr<QueryRequest>> queryQueue;

    std::mutex settingsMutex;
    std::queue<std::shared_ptr<SettingsRequest>> settingsQueue;
};

} // namespace controllerai

#endif
