#include "ControllerAIServer.h"

#include <utility>

namespace controllerai {

CControllerAIServer::CControllerAIServer(std::string bindAddress, int port) :
    bindAddress(std::move(bindAddress)),
    port(port),
    running(false),
    lastObservation(json::object()),
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
    if (wasRunning) {
        svr.stop();
    }
    if (serverThread.joinable()) {
        serverThread.join();
    }
}

void CControllerAIServer::PublishObservation(json observation) {
    std::lock_guard<std::mutex> lock(snapshotMutex);
    lastObservation = std::move(observation);
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

void CControllerAIServer::ConfigureRoutes() {
    svr.Get("/observation", [this](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(snapshotMutex);
        res.set_content(lastObservation.dump(), "application/json");
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
            {
                std::lock_guard<std::mutex> lock(commandMutex);
                commandQueue.push(std::move(cmd));
            }
            commandCv.notify_one();
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
