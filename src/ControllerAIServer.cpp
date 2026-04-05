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

std::string CControllerAIServer::BuildWebSocketMessage(const std::string& type, const json& data) const {
    return json({{"type", type}, {"data", data}}).dump();
}

json CControllerAIServer::BuildHttpQuery(const httplib::Request& req) const {
    json query = json::object();
    for (const auto& param : req.params) {
        query[param.first] = param.second;
    }
    return query;
}

void CControllerAIServer::CompleteQuery(const std::shared_ptr<QueryRequest>& request, json response, const std::string& error) {
    {
        std::lock_guard<std::mutex> lock(request->mutex);
        request->response = std::move(response);
        request->error = error;
        request->completed = true;
    }
    request->cv.notify_one();
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

    std::vector<std::shared_ptr<QueryRequest>> pendingQueries;
    {
        std::lock_guard<std::mutex> lock(queryMutex);
        while (!queryQueue.empty()) {
            pendingQueries.push_back(queryQueue.front());
            queryQueue.pop();
        }
    }

    for (const auto& request : pendingQueries) {
        CompleteQuery(request, json(), "server stopped");
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
        serialized = BuildWebSocketMessage("observation", lastObservation);
    }

    BroadcastWebSocketMessage(serialized);
}

void CControllerAIServer::PublishMetadata(json metadata) {
    std::string serialized;
    {
        std::lock_guard<std::mutex> lock(snapshotMutex);
        unitDefsCache = std::move(metadata);
        serialized = BuildWebSocketMessage("metadata", unitDefsCache);
    }
    BroadcastWebSocketMessage(serialized);
}

void CControllerAIServer::PublishGameInfo(json gameInfo) {
    std::string serialized;
    {
        std::lock_guard<std::mutex> lock(snapshotMutex);
        gameInfoCache = std::move(gameInfo);
        serialized = BuildWebSocketMessage("game_info", gameInfoCache);
    }
    BroadcastWebSocketMessage(serialized);
}

void CControllerAIServer::PublishSpawnBoxes(json spawnBoxes) {
    std::string serialized;
    {
        std::lock_guard<std::mutex> lock(snapshotMutex);
        spawnBoxesCache = std::move(spawnBoxes);
        serialized = BuildWebSocketMessage("spawn_boxes", spawnBoxesCache);
    }
    BroadcastWebSocketMessage(serialized);
}

void CControllerAIServer::PublishMapFeatures(json mapFeatures) {
    std::string serialized;
    {
        std::lock_guard<std::mutex> lock(snapshotMutex);
        mapFeaturesCache = std::move(mapFeatures);
        serialized = BuildWebSocketMessage("map_features", mapFeaturesCache);
    }
    BroadcastWebSocketMessage(serialized);
}

void CControllerAIServer::PublishHeightmap(json heightmap) {
    std::string serialized;
    {
        std::lock_guard<std::mutex> lock(snapshotMutex);
        heightmapCache = std::move(heightmap);
        serialized = BuildWebSocketMessage("heightmap", heightmapCache);
    }
    BroadcastWebSocketMessage(serialized);
}

json CControllerAIServer::GetSpawnBoxes() const {
    std::lock_guard<std::mutex> lock(snapshotMutex);
    return spawnBoxesCache;
}

json CControllerAIServer::ExecuteQuery(json query) {
    if (!running.load()) {
        throw std::runtime_error("Server is not running.");
    }

    auto request = std::make_shared<QueryRequest>();
    request->query = std::move(query);

    {
        std::lock_guard<std::mutex> lock(queryMutex);
        queryQueue.push(request);
    }
    commandCv.notify_all();

    std::unique_lock<std::mutex> lock(request->mutex);
    request->cv.wait(lock, [&]() {
        return request->completed || !running.load();
    });

    if (!request->completed) {
        throw std::runtime_error("Server stopped before the query completed.");
    }

    if (!request->error.empty()) {
        throw std::runtime_error(request->error);
    }

    return request->response;
}

void CControllerAIServer::ProcessQueries(const std::function<json(const json&)>& handler) {
    std::vector<std::shared_ptr<QueryRequest>> pendingQueries;
    {
        std::lock_guard<std::mutex> lock(queryMutex);
        while (!queryQueue.empty()) {
            pendingQueries.push_back(queryQueue.front());
            queryQueue.pop();
        }
    }

    for (const auto& request : pendingQueries) {
        try {
            CompleteQuery(request, handler(request->query));
        } catch (const std::exception& e) {
            CompleteQuery(request, json(), e.what());
        } catch (...) {
            CompleteQuery(request, json(), "Unknown query error.");
        }
    }
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

bool CControllerAIServer::WaitForWork() {
    std::unique_lock<std::mutex> lock(commandMutex);
    commandCv.wait(lock, [this]() {
        if (!running.load() || !commandQueue.empty()) {
            return true;
        }

        std::lock_guard<std::mutex> queryLock(queryMutex);
        return !queryQueue.empty();
    });

    bool hasQueries = false;
    {
        std::lock_guard<std::mutex> queryLock(queryMutex);
        hasQueries = !queryQueue.empty();
    }

    return running.load() || !commandQueue.empty() || hasQueries;
}

void CControllerAIServer::EnqueueCommand(json command) {
    {
        std::lock_guard<std::mutex> lock(commandMutex);
        commandQueue.push(std::move(command));
    }
    commandCv.notify_one();
}

void CControllerAIServer::EnqueueWebSocketMessage(const std::shared_ptr<WebSocketSession>& session, const std::string& message) {
    std::lock_guard<std::mutex> lock(session->mutex);
    if (session->closed) {
        return;
    }

    session->outgoingMessages.push(message);
    session->cv.notify_one();
}

void CControllerAIServer::BroadcastWebSocketMessage(const std::string& message) {
    std::vector<std::shared_ptr<WebSocketSession>> sessions;
    {
        std::lock_guard<std::mutex> lock(websocketMutex);
        sessions = websocketSessions;
    }

    for (const auto& session : sessions) {
        EnqueueWebSocketMessage(session, message);
    }
}

void CControllerAIServer::SendCurrentStateToWebSocketSession(const std::shared_ptr<WebSocketSession>& session) {
    std::vector<std::string> messages;
    {
        std::lock_guard<std::mutex> lock(snapshotMutex);
        if (!gameInfoCache.empty()) {
            messages.push_back(BuildWebSocketMessage("game_info", gameInfoCache));
        }
        if (!spawnBoxesCache.empty()) {
            messages.push_back(BuildWebSocketMessage("spawn_boxes", spawnBoxesCache));
        }
        if (!unitDefsCache.empty()) {
            messages.push_back(BuildWebSocketMessage("metadata", unitDefsCache));
        }
        if (!mapFeaturesCache.empty()) {
            messages.push_back(BuildWebSocketMessage("map_features", mapFeaturesCache));
        }
        if (!heightmapCache.empty()) {
            messages.push_back(BuildWebSocketMessage("heightmap", heightmapCache));
        }
        if (!lastObservationSerialized.empty()) {
            messages.push_back(BuildWebSocketMessage("observation", lastObservation));
        }
    }

    for (const auto& message : messages) {
        EnqueueWebSocketMessage(session, message);
    }
}

void CControllerAIServer::HandleWebSocketPayload(const std::shared_ptr<WebSocketSession>& session, json payload) {
    if (payload.is_array()) {
        for (auto& item : payload) {
            HandleWebSocketCommandOrRequest(session, std::move(item));
        }
        return;
    }

    HandleWebSocketCommandOrRequest(session, std::move(payload));
}

void CControllerAIServer::HandleWebSocketCommandOrRequest(const std::shared_ptr<WebSocketSession>& session, json payload) {
    if (!payload.is_object()) {
        EnqueueWebSocketMessage(session, BuildWebSocketMessage("error", json({{"message", "WebSocket payload must be a JSON object or array of objects."}})));
        return;
    }

    const std::string type = payload.value("type", std::string());
    if (type.empty()) {
        EnqueueWebSocketMessage(session, BuildWebSocketMessage("error", json({{"message", "Missing message type."}})));
        return;
    }

    json response;
    bool handledRequest = true;
    {
        std::lock_guard<std::mutex> lock(snapshotMutex);
        if (type == "get_observation") {
            response = lastObservation;
        } else if (type == "get_game_info") {
            response = gameInfoCache;
        } else if (type == "get_spawn_boxes") {
            response = spawnBoxesCache;
        } else if (type == "get_metadata") {
            response = unitDefsCache;
        } else if (type == "get_map_features") {
            response = mapFeaturesCache;
        } else if (type == "get_heightmap") {
            response = heightmapCache;
        } else {
            handledRequest = false;
        }
    }

    if (type == "get_all") {
        SendCurrentStateToWebSocketSession(session);
        return;
    }

    if (type == "query") {
        json query = json::object();
        if (payload.contains("query") && payload["query"].is_object()) {
            query = payload["query"];
        } else {
            query = payload;
            query.erase("type");
            if (query.contains("queryType") && !query.contains("type")) {
                query["type"] = query["queryType"];
            }
        }

        const json requestId = payload.contains("requestId") ? payload["requestId"] : json();
        query.erase("requestId");
        query.erase("queryType");

        try {
            json response = {
                {"query", query},
                {"result", ExecuteQuery(query)}
            };
            if (!requestId.is_null()) {
                response["requestId"] = requestId;
            }
            EnqueueWebSocketMessage(session, BuildWebSocketMessage("query_result", response));
        } catch (const std::exception& e) {
            json error = {
                {"query", query},
                {"message", e.what()}
            };
            if (!requestId.is_null()) {
                error["requestId"] = requestId;
            }
            EnqueueWebSocketMessage(session, BuildWebSocketMessage("query_error", error));
        }
        return;
    }

    if (handledRequest) {
        EnqueueWebSocketMessage(session, BuildWebSocketMessage(type.substr(4), response));
        return;
    }

    EnqueueCommand(std::move(payload));
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

        SendCurrentStateToWebSocketSession(session);

        std::thread sender([session, &ws]() {
            while (true) {
                std::string payload;
                bool shouldCloseSocket = false;
                {
                    std::unique_lock<std::mutex> lock(session->mutex);
                    session->cv.wait(lock, [&]() {
                        return session->closed || !session->outgoingMessages.empty();
                    });

                    if (session->closed) {
                        shouldCloseSocket = true;
                    } else {
                        payload = std::move(session->outgoingMessages.front());
                        session->outgoingMessages.pop();
                    }
                }

                if (shouldCloseSocket) {
                    if (ws.is_open()) {
                        ws.close(httplib::ws::CloseStatus::GoingAway, "server shutdown");
                    }
                    break;
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
                try {
                    HandleWebSocketPayload(session, json::parse(message));
                } catch (const std::exception& e) {
                    EnqueueWebSocketMessage(session, BuildWebSocketMessage("error", json({{"message", e.what()}})));
                }
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

    svr.Get("/query", [this](const httplib::Request& req, httplib::Response& res) {
        json query = BuildHttpQuery(req);

        try {
            if (!query.contains("type")) {
                throw std::runtime_error("Missing query type.");
            }

            res.set_content(json({
                {"query", query},
                {"result", ExecuteQuery(query)}
            }).dump(), "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json({
                {"query", query},
                {"error", e.what()}
            }).dump(), "application/json");
        }
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
