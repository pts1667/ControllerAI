#include "ControllerAIServer.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace controllerai {

CControllerAIServer::CControllerAIServer(std::string bindAddress, int port) :
    bindAddress(std::move(bindAddress)),
    port(port),
    running(false),
    lastObservation(json::object()),
    lastObservationSerialized(),
    unitDefsCache(json::object()),
    gameInfoCache(json::object()),
    spawnBoxesCache(json::object()),
    mapFeaturesCache(json::object()),
    heightmapCache(json::object())
{
}

CControllerAIServer::~CControllerAIServer() {
    Stop();
}

void CControllerAIServer::Start() {
    bool expected = false;
    if (!running.compare_exchange_strong(expected, true)) {
        return;
    }

    ConfigureRoutes();
    serverThread = std::thread(&CControllerAIServer::Run, this);
}

void CControllerAIServer::Stop() {
    bool wasRunning = running.exchange(false);
    commandCv.notify_all();

    std::vector<std::shared_ptr<WebSocketSession>> sessions;
    {
        std::lock_guard<std::mutex> lock(websocketMutex);
        sessions = websocketSessions;
    }

    for (const auto& session : sessions) {
        std::lock_guard<std::mutex> lock(session->mutex);
        session->closed = true;
        session->cv.notify_all();
    }

    if (wasRunning) {
        svr.stop();
    }
    if (serverThread.joinable()) {
        serverThread.join();
    }
}

void CControllerAIServer::PublishObservation(json observation) {
    std::string serialized;
    {
        std::lock_guard<std::mutex> lock(snapshotMutex);
        lastObservation = std::move(observation);
        lastObservationSerialized = lastObservation.dump();
        serialized = lastObservationSerialized;
    }

    EnqueueWebSocketObservation(serialized);
}

void CControllerAIServer::PublishMetadata(json metadata) {
    std::lock_guard<std::mutex> lock(snapshotMutex);
    unitDefsCache = std::move(metadata);
}

void CControllerAIServer::PublishGameInfo(json gameInfo) {
    std::lock_guard<std::mutex> lock(snapshotMutex);
    gameInfoCache = std::move(gameInfo);
}

void CControllerAIServer::PublishSpawnBoxes(json spawnBoxes) {
    std::lock_guard<std::mutex> lock(snapshotMutex);
    spawnBoxesCache = std::move(spawnBoxes);
}

void CControllerAIServer::PublishMapFeatures(json mapFeatures) {
    std::lock_guard<std::mutex> lock(snapshotMutex);
    mapFeaturesCache = std::move(mapFeatures);
}

void CControllerAIServer::PublishHeightmap(json heightmap) {
    std::lock_guard<std::mutex> lock(snapshotMutex);
    heightmapCache = std::move(heightmap);
}

json CControllerAIServer::GetSpawnBoxes() const {
    std::lock_guard<std::mutex> lock(snapshotMutex);
    return spawnBoxesCache;
}

std::vector<json> CControllerAIServer::DrainCommands() {
    std::lock_guard<std::mutex> lock(commandMutex);
    std::vector<json> commands;
    commands.reserve(commandQueue.size());
    while (!commandQueue.empty()) {
        commands.push_back(std::move(commandQueue.front()));
        commandQueue.pop();
    }
    return commands;
}

bool CControllerAIServer::WaitForCommands() {
    std::unique_lock<std::mutex> lock(commandMutex);
    commandCv.wait(lock, [this]() {
        return !running.load() || !commandQueue.empty();
    });
    return running.load() || !commandQueue.empty();
}

void CControllerAIServer::EnqueueCommand(json command) {
    {
        std::lock_guard<std::mutex> lock(commandMutex);
        commandQueue.push(std::move(command));
    }
    commandCv.notify_one();
}

void CControllerAIServer::EnqueueWebSocketObservation(const std::string& observation) {
    std::vector<std::shared_ptr<WebSocketSession>> sessions;
    {
        std::lock_guard<std::mutex> lock(websocketMutex);
        sessions = websocketSessions;
    }

    for (const auto& session : sessions) {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (session->closed) {
            continue;
        }

        session->outgoingMessages.push(observation);
        session->cv.notify_one();
    }
}

void CControllerAIServer::EnqueueWebSocketCommands(const std::string& message) {
    try {
        json payload = json::parse(message);
        if (payload.is_array()) {
            for (auto& command : payload) {
                if (command.is_object()) {
                    EnqueueCommand(std::move(command));
                }
            }
            return;
        }

        if (payload.is_object()) {
            EnqueueCommand(std::move(payload));
        }
    } catch (...) {
    }
}

void CControllerAIServer::RemoveWebSocketSession(const std::shared_ptr<WebSocketSession>& session) {
    std::lock_guard<std::mutex> lock(websocketMutex);
    websocketSessions.erase(
        std::remove(websocketSessions.begin(), websocketSessions.end(), session),
        websocketSessions.end()
    );
}

void CControllerAIServer::ConfigureRoutes() {
    svr.set_websocket_ping_interval(std::chrono::seconds(10));

    svr.WebSocket("/ws", [this](const httplib::Request&, httplib::ws::WebSocket& ws) {
        auto session = std::make_shared<WebSocketSession>();

        {
            std::lock_guard<std::mutex> lock(websocketMutex);
            websocketSessions.push_back(session);
        }

        {
            std::lock_guard<std::mutex> lock(snapshotMutex);
            if (!lastObservationSerialized.empty()) {
                session->outgoingMessages.push(lastObservationSerialized);
            }
        }
        session->cv.notify_one();

        std::thread sender([session, &ws]() {
            while (true) {
                std::string payload;
                {
                    std::unique_lock<std::mutex> lock(session->mutex);
                    session->cv.wait(lock, [&]() {
                        return session->closed || !session->outgoingMessages.empty();
                    });

                    if (session->closed) {
                        break;
                    }

                    payload = std::move(session->outgoingMessages.front());
                    session->outgoingMessages.pop();
                }

                if (!ws.send(payload)) {
                    ws.close();
                    break;
                }
            }

            {
                std::lock_guard<std::mutex> lock(session->mutex);
                session->closed = true;
            }
            session->cv.notify_all();
        });

        std::string message;
        while (running.load() && ws.is_open()) {
            auto readResult = ws.read(message);
            if (!readResult) {
                break;
            }

            if (readResult == httplib::ws::Text) {
                EnqueueWebSocketCommands(message);
            }
        }

        {
            std::lock_guard<std::mutex> lock(session->mutex);
            session->closed = true;
        }
        session->cv.notify_all();

        if (sender.joinable()) {
            sender.join();
        }

        RemoveWebSocketSession(session);
    });

    svr.Get("/observation", [this](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(snapshotMutex);
        res.set_content(lastObservationSerialized.empty() ? lastObservation.dump() : lastObservationSerialized, "application/json");
    });

    svr.Get("/metadata", [this](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(snapshotMutex);
        res.set_content(unitDefsCache.dump(), "application/json");
    });

    svr.Get("/game_info", [this](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(snapshotMutex);
        res.set_content(gameInfoCache.dump(), "application/json");
    });

    svr.Get("/spawn_boxes", [this](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(snapshotMutex);
        res.set_content(spawnBoxesCache.dump(), "application/json");
    });

    svr.Get("/map_features", [this](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(snapshotMutex);
        res.set_content(mapFeaturesCache.dump(), "application/json");
    });

    svr.Get("/heightmap", [this](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(snapshotMutex);
        res.set_content(heightmapCache.dump(), "application/json");
    });

    svr.Post("/command", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            json cmd = json::parse(req.body);
            EnqueueCommand(std::move(cmd));
            res.set_content("{\"status\": \"queued\"}", "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json({{"error", e.what()}}).dump(), "application/json");
        }
    });
}

void CControllerAIServer::Run() {
    svr.listen(bindAddress.c_str(), port);
    running.store(false);
    commandCv.notify_all();
}

} // namespace controllerai
