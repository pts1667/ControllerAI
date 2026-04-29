#include "ControllerAI.h"
#include "ControllerAICommon.h"
#include "ControllerAIServer.h"

#include "Feature.h"
#include "FeatureDef.h"
#include "Game.h"
#include "Log.h"
#include "Map.h"
#include "Mod.h"
#include "OOAICallback.h"
#include "OptionValues.h"
#include "Resource.h"
#include "SkirmishAI.h"
#include "UnitDef.h"
#include "Economy.h"
#include "Lua.h"

#include "OOAICallback.h"
#include <utility>
#include <vector>

namespace controllerai {

namespace {

constexpr int kMapSquareSize = 8;

}

CControllerAI::CControllerAI(springai::OOAICallback* callback, std::string bindAddress, int port, int masterPort) :
    callback(callback),
    skirmishAIId(callback != nullptr ? callback->GetSkirmishAIId() : -1),
    bindAddress(std::move(bindAddress)),
    port(port),
    masterPort(masterPort),
    server(nullptr),
    eventBuffer(json::array()),
    running(true),
    synchronousMode(true),
    setupComplete(true),
    canChooseStartPos(false),
    frameFinished(true),
    startupBlocking(true),
    released(false),
    blockNFrames(1),
    losMapWidth(0),
    losMapHeight(0),
    losMapResolution(0),
    losMapFrame(-1)
{
    server = std::make_unique<CControllerAIServer>(
        this->bindAddress,
        this->port,
        [this](const std::string& message) {
            LogToInfolog(message);
        }
    );

    server->Start();
    server->PublishSettings(GetSettings());
}

CControllerAI::~CControllerAI() {
    Release(0);
}

void CControllerAI::EnsureInterfacesInitialized() {
    if (!callback) {
        return;
    }

    if (!game) {
        game = std::unique_ptr<springai::Game>(callback->GetGame());
    }
    if (!map) {
        map = std::unique_ptr<springai::Map>(callback->GetMap());
    }
    if (!log) {
        log = std::unique_ptr<springai::Log>(callback->GetLog());
    }
    if (!skirmishAI) {
        skirmishAI = std::unique_ptr<springai::SkirmishAI>(callback->GetSkirmishAI());
        if (skirmishAI) {
            springai::OptionValues* options = skirmishAI->GetOptionValues();
            if (options) {
                const char* syncOpt = options->GetValueByKey("sync");
                if (syncOpt != nullptr) {
                    synchronousMode = (std::string(syncOpt) == "true");
                }
            }
        }
    }
    if (!economy) {
        economy = std::unique_ptr<springai::Economy>(callback->GetEconomy());
    }
    if (!lua) {
        lua = std::unique_ptr<springai::Lua>(callback->GetLua());
    }
    if (!mod) {
        mod = std::unique_ptr<springai::Mod>(callback->GetMod());
    }
}

void CControllerAI::RefreshLosMapCache() {
    if (!map || !mod) {
        losMapCache.clear();
        losMapWidth = 0;
        losMapHeight = 0;
        losMapResolution = 0;
        losMapFrame = -1;
        return;
    }

    const int currentFrame = game ? game->GetCurrentFrame() : -1;
    if (!losMapCache.empty() && losMapFrame == currentFrame && losMapWidth > 0 && losMapHeight > 0 && losMapResolution > 0) {
        return;
    }

    const int losMipLevel = mod->GetLosMipLevel();
    losMapCache = map->GetLosMap();
    losMapWidth = map->GetWidth() >> losMipLevel;
    losMapHeight = map->GetHeight() >> losMipLevel;
    losMapResolution = kMapSquareSize << losMipLevel;
    losMapFrame = currentFrame;
}

bool CControllerAI::IsPositionInLos(const springai::AIFloat3& pos) {
    RefreshLosMapCache();
    if (losMapCache.empty() || losMapWidth <= 0 || losMapHeight <= 0 || losMapResolution <= 0) {
        return false;
    }

    const int x = static_cast<int>(pos.x) / losMapResolution;
    const int z = static_cast<int>(pos.z) / losMapResolution;
    if (x < 0 || z < 0 || x >= losMapWidth || z >= losMapHeight) {
        return false;
    }

    const std::size_t index = static_cast<std::size_t>(z * losMapWidth + x);
    if (index >= losMapCache.size()) {
        return false;
    }

    return losMapCache[index] > 0;
}

void CControllerAI::LogToInfolog(const std::string& message) {
    if (!callback) {
        return;
    }

    if (!log) {
        log = std::unique_ptr<springai::Log>(callback->GetLog());
    }

    if (log) {
        log->DoLog(message.c_str());
    }
}

void CControllerAI::CacheStaticData() {
    if (!server) {
        return;
    }

    json gameInfoCache = json::object();
    json spawnBoxesCache = json::object();
    json unitDefsCache = json::object();
    json mapFeaturesCache = json::object();
    json heightmapCache = json::object();

    if (mod) {
        gameInfoCache["modName"] = detail::SafeCString(mod->GetHumanName());
        gameInfoCache["gametype"] = detail::SafeCString(mod->GetShortName());
    }

    if (map) {
        gameInfoCache["mapName"] = detail::SafeCString(map->GetHumanName());
        gameInfoCache["mapWidth"] = map->GetWidth();
        gameInfoCache["mapHeight"] = map->GetHeight();
        gameInfoCache["mapWidthElmos"] = map->GetWidth() * 8;
        gameInfoCache["mapHeightElmos"] = map->GetHeight() * 8;
    }

    std::string script;
    if (game) {
        gameInfoCache["gameMode"] = -1;
        gameInfoCache["isPaused"] = game->IsPaused();
        gameInfoCache["teamId"] = game->GetMyTeam();
        gameInfoCache["allyTeamId"] = game->GetMyAllyTeam();

        script = detail::SafeCString(game->GetSetupScript());
        canChooseStartPos = (script.find("startpostype=1") != std::string::npos);
        setupComplete = !canChooseStartPos;
        startupBlocking = true;
    }
    gameInfoCache["serverAddress"] = bindAddress;
    gameInfoCache["serverPort"] = port;
    gameInfoCache["masterServerAddress"] = bindAddress;
    gameInfoCache["masterServerPort"] = masterPort;
    gameInfoCache["masterListPath"] = "/list";
    gameInfoCache["canChooseStartPos"] = canChooseStartPos;
    gameInfoCache["supportsWebsocketApi"] = true;
    gameInfoCache["websocketPath"] = "/ws";
    gameInfoCache["supportsWebsocketObservation"] = true;
    gameInfoCache["websocketObservationPath"] = "/ws";

    if (game) {
        detail::TryParseZkSpawnBoxes(game.get(), spawnBoxesCache);
    }
    if (spawnBoxesCache.empty() && map && !script.empty()) {
        detail::TryParseClassicSpawnBoxes(script, map.get(), spawnBoxesCache);
    }

    if (callback) {
        std::vector<springai::UnitDef*> defs = callback->GetUnitDefs();
        for (springai::UnitDef* def : defs) {
            if (!def) continue;

            json d;
            d["name"] = detail::SafeCString(def->GetName());
            d["humanName"] = detail::SafeCString(def->GetHumanName());
            d["buildTime"] = def->GetBuildTime();
            d["health"] = def->GetHealth();
            d["speed"] = def->GetSpeed();
            d["range"] = def->GetMaxWeaponRange();
            unitDefsCache[std::to_string(def->GetUnitDefId())] = d;
        }
    }

    mapFeaturesCache["spots"] = json::array();
    mapFeaturesCache["features"] = json::array();
    if (callback && map) {
        const float waterDamage = map->GetWaterDamage();
        mapFeaturesCache["waterDamage"] = waterDamage;
        mapFeaturesCache["waterIsHarmful"] = waterDamage > 0.0f;

        std::vector<springai::Resource*> resources = callback->GetResources();
        for (springai::Resource* res_ptr : resources) {
            if (!res_ptr) continue;

            json resourceSpots = detail::GetResourceSpotsData(game.get(), map.get(), res_ptr);
            if (resourceSpots.contains("averageIncome")) {
                mapFeaturesCache["averageIncomeByResource"][detail::SafeCString(res_ptr->GetName())] = resourceSpots["averageIncome"];
            }

            if (!resourceSpots.contains("spots") || !resourceSpots["spots"].is_array()) {
                continue;
            }

            for (const json& spot : resourceSpots["spots"]) {
                json s = spot;
                s["resource"] = detail::SafeCString(res_ptr->GetName());
                mapFeaturesCache["spots"].push_back(std::move(s));
            }
        }

        std::vector<springai::Feature*> features = callback->GetFeatures();
        for (springai::Feature* f : features) {
            if (!f) continue;

            json fj;
            fj["id"] = f->GetFeatureId();
            springai::AIFloat3 f_pos = f->GetPosition();
            fj["pos"] = json::array({f_pos.x, f_pos.y, f_pos.z});
            fj["health"] = f->GetHealth();
            springai::FeatureDef* fdef = f->GetDef();
            if (fdef) {
                fj["name"] = detail::SafeCString(fdef->GetName());
            }
            mapFeaturesCache["features"].push_back(fj);
        }
    }

    if (map) {
        int h_width = map->GetWidth();
        int h_height = map->GetHeight();
        std::vector<float> heights = map->GetHeightMap();
        heightmapCache["width"] = h_width;
        heightmapCache["height"] = h_height;
        heightmapCache["data_b64"] = detail::Base64Encode(
            reinterpret_cast<const unsigned char*>(heights.data()),
            heights.size() * sizeof(float)
        );
    }

    server->PublishGameInfo(std::move(gameInfoCache));
    server->PublishSpawnBoxes(std::move(spawnBoxesCache));
    server->PublishMetadata(std::move(unitDefsCache));
    server->PublishMapFeatures(std::move(mapFeaturesCache));
    server->PublishHeightmap(std::move(heightmapCache));
}

void CControllerAI::Release(int /*reason*/) {
    if (released) {
        return;
    }

    released = true;
    running = false;
    frameFinished = true;
    setupComplete = true;
    startupBlocking = false;
    eventBuffer = json::array();

    if (server) {
        server->Stop();
    }
}

} // namespace controllerai
