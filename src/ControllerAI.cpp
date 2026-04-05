#include "ControllerAI.h"
#include "ExternalAI/Interface/AISEvents.h"
#include "ExternalAI/Interface/AISCommands.h"
#include "ControllerAIServer.h"

#include "Game.h"
#include "Map.h"
#include "Unit.h"
#include "WrappUnit.h"
#include "UnitDef.h"
#include "WrappUnitDef.h"
#include "Feature.h"
#include "WrappFeature.h"
#include "FeatureDef.h"
#include "Resource.h"
#include "Economy.h"
#include "Cheats.h"
#include "Lua.h"
#include "Mod.h"
#include "SkirmishAI.h"
#include "OptionValues.h"
#include "Log.h"

#include <iostream>
#include <chrono>
#include <algorithm>
#include <cstdint>
#include <regex>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace controllerai {

CControllerAI::CControllerAI(springai::OOAICallback* callback) :
    callback(callback),
    skirmishAIId(callback != nullptr ? callback->GetSkirmishAIId() : -1),
    bindAddress("127.0.0.1"),
    port(3017),
    server(nullptr),
    eventBuffer(json::array()),
    running(true),
    synchronousMode(true),
    setupComplete(true),
    canChooseStartPos(false),
    frameFinished(true),
    released(false)
{
    if (callback) {
        game = std::unique_ptr<springai::Game>(callback->GetGame());
        map = std::unique_ptr<springai::Map>(callback->GetMap());
        log = std::unique_ptr<springai::Log>(callback->GetLog());
        skirmishAI = std::unique_ptr<springai::SkirmishAI>(callback->GetSkirmishAI());
        economy = std::unique_ptr<springai::Economy>(callback->GetEconomy());
        lua = std::unique_ptr<springai::Lua>(callback->GetLua());
        mod = std::unique_ptr<springai::Mod>(callback->GetMod());
    }

    if (skirmishAI) {
        springai::OptionValues* options = skirmishAI->GetOptionValues();
        if (options) {
            const char* ip_opt = options->GetValueByKey("ip");
            if (ip_opt != nullptr) {
                bindAddress = ip_opt;
            }

            const char* port_opt = options->GetValueByKey("port");
            if (port_opt != nullptr) {
                port = std::stoi(port_opt);
            }

            const char* sync_opt = options->GetValueByKey("sync");
            if (sync_opt != nullptr) {
                synchronousMode = (std::string(sync_opt) == "true");
            }
        }
    }

    server = std::make_unique<CControllerAIServer>(bindAddress, port);

    server->Start();
}

CControllerAI::~CControllerAI() {
    Release(0);
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
        gameInfoCache["modName"] = SafeCString(mod->GetHumanName());
    }

    if (map) {
        gameInfoCache["mapName"] = SafeCString(map->GetHumanName());
        gameInfoCache["mapWidth"] = map->GetWidth();
        gameInfoCache["mapHeight"] = map->GetHeight();
        gameInfoCache["mapWidthElmos"] = map->GetWidth() * 8;
        gameInfoCache["mapHeightElmos"] = map->GetHeight() * 8;
    }

    std::string script;
    if (game) {
        gameInfoCache["gameMode"] = -1;
        gameInfoCache["isPaused"] = game->IsPaused();

        script = SafeCString(game->GetSetupScript());
        canChooseStartPos = (script.find("startpostype=1") != std::string::npos);
        setupComplete = !canChooseStartPos;
    }
    gameInfoCache["canChooseStartPos"] = canChooseStartPos;
    gameInfoCache["supportsWebsocketApi"] = true;
    gameInfoCache["websocketPath"] = "/ws";
    gameInfoCache["supportsWebsocketObservation"] = true;
    gameInfoCache["websocketObservationPath"] = "/ws";

    if (map && !script.empty()) {
        int width_elmos = map->GetWidth() * 8;
        int height_elmos = map->GetHeight() * 8;

        std::regex zkPattern("\\[(\\d+)\\]\\s*=\\s*\\{\\s*([\\d.]+),\\s*([\\d.]+),\\s*([\\d.]+),\\s*([\\d.]+)\\s*\\}");
        auto zk_it = std::sregex_iterator(script.begin(), script.end(), zkPattern);
        auto zk_end = std::sregex_iterator();
        for (std::sregex_iterator i = zk_it; i != zk_end; ++i) {
            std::smatch match = *i;
            int allyId = std::stoi(match[1]);
            json box;
            box["left"] = std::stof(match[2]) * width_elmos;
            box["top"] = std::stof(match[3]) * height_elmos;
            box["right"] = std::stof(match[4]) * width_elmos;
            box["bottom"] = std::stof(match[5]) * height_elmos;
            spawnBoxesCache[std::to_string(allyId)] = box;
        }

        if (spawnBoxesCache.empty()) {
            std::regex stdPattern("allyteam(\\d+)\\s*\\{[^}]*rect\\s*=\\s*(\\d+)\\s+(\\d+)\\s+(\\d+)\\s+(\\d+)");
            auto std_it = std::sregex_iterator(script.begin(), script.end(), stdPattern);
            auto std_end = std::sregex_iterator();
            for (std::sregex_iterator i = std_it; i != std_end; ++i) {
                std::smatch match = *i;
                int allyId = std::stoi(match[1]);
                json box;
                box["left"] = std::stof(match[2]);
                box["top"] = std::stof(match[3]);
                box["right"] = std::stof(match[4]);
                box["bottom"] = std::stof(match[5]);
                spawnBoxesCache[std::to_string(allyId)] = box;
            }
        }
    }

    if (callback) {
        std::vector<springai::UnitDef*> defs = callback->GetUnitDefs();
        for (springai::UnitDef* def : defs) {
            if (!def) continue;

            json d;
            d["name"] = SafeCString(def->GetName());
            d["humanName"] = SafeCString(def->GetHumanName());
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
        std::vector<springai::Resource*> resources = callback->GetResources();
        for (springai::Resource* res_ptr : resources) {
            if (!res_ptr) continue;

            std::vector<springai::AIFloat3> spots = map->GetResourceMapSpotsPositions(res_ptr);
            for (const auto& spot_pos : spots) {
                json s;
                s["resource"] = SafeCString(res_ptr->GetName());
                s["pos"] = json::array({spot_pos.x, spot_pos.y, spot_pos.z});
                mapFeaturesCache["spots"].push_back(s);
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
                fj["name"] = SafeCString(fdef->GetName());
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
        heightmapCache["data_b64"] = Base64Encode(
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

std::string CControllerAI::SafeCString(const char* value) const {
    return value != nullptr ? std::string(value) : std::string();
}

bool CControllerAI::IsSpawnPosValid(const springai::AIFloat3& pos) {
    if (!game || !lua || !server) return false;
    int myAlly = game->GetMyAllyTeam();
    
    std::string allyStr = std::to_string(myAlly);
    json spawnBoxesCache = server->GetSpawnBoxes();
    if (spawnBoxesCache.contains(allyStr)) {
        auto box = spawnBoxesCache[allyStr];
        if (pos.x < box["left"] || pos.x > box["right"] || pos.z < box["top"] || pos.z > box["bottom"]) {
            return false;
        }
    }

    // Engine validation via LuaRules.
    std::string formats[] = { ",", "/" };
    for (const std::string& sep : formats) {
        std::stringstream ss;
        ss << "ai_is_valid_startpos:" << (int)pos.x << sep << (int)pos.z;
        std::string result = lua->CallRules(ss.str().c_str(), (int)ss.str().size());
        if (result == "not_valid" || result == "0") return false;
    }

    return true;
}

std::string CControllerAI::Base64Encode(const unsigned char* data, size_t len) {
    static const char* base64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string ret;
    int i = 0;
    int j = 0;
    unsigned char char_array_3[3];
    unsigned char char_array_4[4];

    while (len--) {
        char_array_3[i++] = *(data++);
        if (i == 3) {
            char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
            char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
            char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
            char_array_4[3] = char_array_3[2] & 0x3f;

            for (i = 0; i < 4; i++) ret += base64_chars[char_array_4[i]];
            i = 0;
        }
    }

    if (i) {
        for (j = i; j < 3; j++) char_array_3[j] = '\0';
        char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
        char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
        char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
        char_array_4[3] = char_array_3[2] & 0x3f;

        for (j = 0; j < i + 1; j++) ret += base64_chars[char_array_4[j]];
        while ((i++ < 3)) ret += '=';
    }
    return ret;
}

void CControllerAI::Release(int /*reason*/) {
    if (released) {
        return;
    }

    released = true;
    running = false;
    frameFinished = true;
    setupComplete = true;
    eventBuffer = json::array();

    if (server) {
        server->Stop();
    }
}

std::tuple<int, json> CControllerAI::ParseUnit(springai::Unit* unit) {
    if (!unit) return std::make_tuple(-1, json());

    json u;
    springai::UnitDef* def = unit->GetDef();
    if (def) {
        u["defId"] = def->GetUnitDefId();
        u["name"] = SafeCString(def->GetName());
    }

    springai::AIFloat3 pos = unit->GetPos();
    springai::AIFloat3 vel = unit->GetVel();

    u["allyTeam"] = unit->GetAllyTeam();
    u["pos"] = json::array({pos.x, pos.y, pos.z});
    u["vel"] = json::array({vel.x, vel.y, vel.z});
    u["health"] = unit->GetHealth();
    u["maxHealth"] = unit->GetMaxHealth();
    u["experience"] = unit->GetExperience();
    u["buildProgress"] = unit->GetBuildProgress();
    u["isBeingBuilt"] = unit->IsBeingBuilt();
    u["isCloaked"] = unit->IsCloaked();
    u["isParalyzed"] = unit->IsParalyzed();

    return std::make_tuple(unit->GetUnitId(), u);
}

std::tuple<int, json> CControllerAI::ParseRadarBlip(springai::Unit* unit) {
    if (!unit) return std::make_tuple(-1, json());

    json blip;
    springai::AIFloat3 pos = unit->GetPos();
    springai::AIFloat3 vel = unit->GetVel();

    blip["allyTeam"] = unit->GetAllyTeam();
    blip["pos"] = json::array({pos.x, pos.y, pos.z});
    blip["vel"] = json::array({vel.x, vel.y, vel.z});
    blip["inLos"] = false;
    blip["isRadarBlip"] = true;

    springai::UnitDef* def = unit->GetDef();
    if (def) {
        blip["defId"] = def->GetUnitDefId();
        blip["name"] = SafeCString(def->GetName());
    }

    return std::make_tuple(unit->GetUnitId(), blip);
}

void CControllerAI::UpdateObservation() {
    if (!callback || !game || !economy || !server) return;

    json obs;
    obs["frame"] = game->GetCurrentFrame();
    obs["aiId"] = skirmishAIId;
    obs["teamId"] = game->GetMyTeam();
    obs["allyTeamId"] = game->GetMyAllyTeam();

    // Friendly units
    obs["units"] = json::object();
    std::vector<springai::Unit*> myUnits = callback->GetFriendlyUnits();
    for (springai::Unit* unit : myUnits) {
        if (!unit) continue;
        
        auto [unitId, u] = ParseUnit(unit);
        if (unitId == -1) continue;
        obs["units"][std::to_string(unitId)] = u;
    }

    // Enemy units
    obs["enemies"] = json::object();
    std::unordered_set<int> enemyIds;
    std::vector<springai::Unit*> enemies = callback->GetEnemyUnits();
    for (springai::Unit* unit : enemies) {
        if (!unit) continue;

        auto [unitId, e] = ParseUnit(unit);
        if (unitId == -1) continue;
        enemyIds.insert(unitId);
        e["inLos"] = true;
        obs["enemies"][std::to_string(unitId)] = e;
    }

    obs["radarBlips"] = json::object();
    std::vector<springai::Unit*> radarContacts = callback->GetEnemyUnitsInRadarAndLos();
    for (springai::Unit* unit : radarContacts) {
        if (!unit) continue;

        auto [unitId, blip] = ParseRadarBlip(unit);
        if (unitId == -1 || enemyIds.find(unitId) != enemyIds.end()) {
            continue;
        }

        obs["radarBlips"][std::to_string(unitId)] = blip;
    }

    // Economy
    std::vector<springai::Resource*> resources = callback->GetResources();
    obs["economy"] = json::object();
    for (springai::Resource* res_ptr : resources) {
        if (!res_ptr) continue;
        
        json r;
        r["current"] = economy->GetCurrent(res_ptr);
        r["storage"] = economy->GetStorage(res_ptr);
        r["income"] = economy->GetIncome(res_ptr);
        r["usage"] = economy->GetUsage(res_ptr);
        obs["economy"][SafeCString(res_ptr->GetName())] = r;
    }

    obs["events"] = eventBuffer;
    eventBuffer = json::array(); // Clear buffer after poll
    server->PublishObservation(std::move(obs));
}

void CControllerAI::ProcessCommands() {
    if (!server) return;

    std::vector<json> commands = server->DrainCommands();
    for (json& cmd : commands) {

        try {
            std::string type = cmd["type"];
            int unitId = cmd.value("unitId", -1);
            springai::Unit* unit = (unitId != -1) ? springai::WrappUnit::GetInstance(skirmishAIId, unitId) : nullptr;
            short opts = cmd.value("options", 0);

            if (type == "move" && unit) {
                springai::AIFloat3 pos;
                pos.x = cmd["pos"][0];
                pos.y = cmd["pos"][1];
                pos.z = cmd["pos"][2];
                unit->MoveTo(pos, opts, 100000);
            } else if (type == "attack" && unit) {
                int targetId = cmd["targetId"];
                springai::Unit* target = springai::WrappUnit::GetInstance(skirmishAIId, targetId);
                if (target) unit->Attack(target, opts, 100000);
            } else if (type == "build" && unit) {
                int defId = cmd["defId"];
                springai::UnitDef* def = springai::WrappUnitDef::GetInstance(skirmishAIId, defId);
                if (def) {
                    springai::AIFloat3 pos;
                    pos.x = cmd["pos"][0];
                    pos.y = cmd["pos"][1];
                    pos.z = cmd["pos"][2];
                    unit->Build(def, pos, cmd.value("facing", 0), opts, 100000);
                }
            } else if (type == "stop" && unit) {
                unit->Stop(opts, 100000);
            } else if (type == "wait" && unit) {
                unit->Wait(opts, 100000);
            } else if (type == "patrol" && unit) {
                springai::AIFloat3 pos;
                pos.x = cmd["pos"][0];
                pos.y = cmd["pos"][1];
                pos.z = cmd["pos"][2];
                unit->PatrolTo(pos, opts, 100000);
            } else if (type == "fight" && unit) {
                springai::AIFloat3 pos;
                pos.x = cmd["pos"][0];
                pos.y = cmd["pos"][1];
                pos.z = cmd["pos"][2];
                unit->Fight(pos, opts, 100000);
            } else if (type == "guard" && unit) {
                int targetId = cmd["targetId"];
                springai::Unit* target = springai::WrappUnit::GetInstance(skirmishAIId, targetId);
                if (target) unit->Guard(target, opts, 100000);
            } else if (type == "repair" && unit) {
                int targetId = cmd["targetId"];
                springai::Unit* target = springai::WrappUnit::GetInstance(skirmishAIId, targetId);
                if (target) unit->Repair(target, opts, 100000);
            } else if (type == "reclaim" && unit) {
                if (cmd.contains("targetId")) {
                    springai::Unit* target = springai::WrappUnit::GetInstance(skirmishAIId, cmd["targetId"]);
                    if (target) unit->ReclaimUnit(target, opts, 100000);
                } else if (cmd.contains("featureId")) {
                    springai::Feature* feature = springai::WrappFeature::GetInstance(skirmishAIId, cmd["featureId"]);
                    if (feature) unit->ReclaimFeature(feature, opts, 100000);
                }
            } else if (type == "resurrect" && unit) {
                springai::Feature* feature = springai::WrappFeature::GetInstance(skirmishAIId, cmd["featureId"]);
                if (feature) unit->Resurrect(feature, opts, 100000);
            } else if (type == "capture" && unit) {
                int targetId = cmd["targetId"];
                springai::Unit* target = springai::WrappUnit::GetInstance(skirmishAIId, targetId);
                if (target) unit->Capture(target, opts, 100000);
            } else if (type == "self_destruct" && unit) {
                unit->SelfDestruct(opts, 100000);
            } else if (type == "custom" && unit) {
                int cmdId = cmd["cmdId"];
                std::vector<float> params = cmd.value("params", std::vector<float>());
                unit->ExecuteCustomCommand(cmdId, params, opts, 100000);
            } else if (type == "set_start_pos") {
                springai::AIFloat3 pos = {cmd["pos"][0], cmd["pos"][1], cmd["pos"][2]};
                bool ready = cmd.value("ready", true);
                if (IsSpawnPosValid(pos)) {
                    if (game) game->SendStartPosition(ready, pos);
                    if (ready) {
                        setupComplete = true;
                    }
                } else {
                    // Logic to inform user could be via text message or a status code if we wait
                    if (game) game->SendTextMessage("ControllerAI: Invalid start position received!", 0);
                }
            } else if (type == "set_commander") {
                std::string name = cmd["name"];
                if (lua) {
                    std::string msg = "ai_commander:" + name;
                    lua->CallRules(msg.c_str(), (int)msg.size());
                }
            } else if (type == "set_side") {
                std::string name = cmd["name"];
                if (lua) {
                    std::string msg = "ai_side:" + name;
                    lua->CallRules(msg.c_str(), (int)msg.size());
                }
            } else if (type == "lua") {
                if (lua) {
                    std::string data = cmd["data"];
                    lua->CallRules(data.c_str(), (int)data.size());
                }
            } else if (type == "finish_frame") {
                frameFinished = true;
            }
        } catch (...) {}
    }
}

void CControllerAI::WaitForResume() {
    if (!server) return;

    while (running) {
        if (game && canChooseStartPos && !setupComplete) {
            if (!server->WaitForCommands()) return;
            ProcessCommands();
            continue;
        }

        if (!synchronousMode || !game || game->GetCurrentFrame() < 0 || frameFinished) {
            return;
        }

        if (!server->WaitForCommands()) return;
        ProcessCommands();
    }
}

json CControllerAI::EventToJson(int topic, const void* data) {
    json e;
    e["topic"] = topic;
    if (!data) return e;

    auto addDirection = [&e](const float* dir_posF3) {
        if (!dir_posF3) return;
        e["dirPos"] = json::array({dir_posF3[0], dir_posF3[1], dir_posF3[2]});
    };

    switch (topic) {
        case EVENT_INIT: {
            struct SInitEvent* ev = (struct SInitEvent*)data;
            e["name"] = "Init";
            e["isSavegame"] = ev->savedGame;
            break;
        }
        case EVENT_UNIT_CREATED: {
            struct SUnitCreatedEvent* ev = (struct SUnitCreatedEvent*)data;
            e["unitId"] = ev->unit;
            e["builderId"] = ev->builder;
            break;
        }
        case EVENT_UNIT_FINISHED: {
            struct SUnitFinishedEvent* ev = (struct SUnitFinishedEvent*)data;
            e["unitId"] = ev->unit;
            break;
        }
        case EVENT_UNIT_IDLE: {
            struct SUnitIdleEvent* ev = (struct SUnitIdleEvent*)data;
            e["unitId"] = ev->unit;
            break;
        }
        case EVENT_UNIT_DESTROYED: {
            struct SUnitDestroyedEvent* ev = (struct SUnitDestroyedEvent*)data;
            e["unitId"] = ev->unit;
            e["attackerId"] = ev->attacker;
            break;
        }
        case EVENT_UNIT_DAMAGED: {
            struct SUnitDamagedEvent* ev = (struct SUnitDamagedEvent*)data;
            e["unitId"] = ev->unit;
            e["attackerId"] = ev->attacker;
            e["damage"] = ev->damage;
            e["weaponDefId"] = ev->weaponDefId;
            e["paralyzer"] = ev->paralyzer;
            addDirection(ev->dir_posF3);
            break;
        }
        case EVENT_ENEMY_ENTER_LOS: {
            struct SEnemyEnterLOSEvent* ev = (struct SEnemyEnterLOSEvent*)data;
            e["enemyId"] = ev->enemy;
            break;
        }
        case EVENT_ENEMY_DAMAGED: {
            struct SEnemyDamagedEvent* ev = (struct SEnemyDamagedEvent*)data;
            e["enemyId"] = ev->enemy;
            e["attackerId"] = ev->attacker;
            e["damage"] = ev->damage;
            e["weaponDefId"] = ev->weaponDefId;
            e["paralyzer"] = ev->paralyzer;
            addDirection(ev->dir_posF3);
            break;
        }
        case EVENT_LUA_MESSAGE: {
            struct SLuaMessageEvent* ev = (struct SLuaMessageEvent*)data;
            e["data"] = SafeCString(ev->inData);
            break;
        }
        default: break;
    }
    return e;
}

int CControllerAI::HandleEvent(int topic, const void* data) {
    if (!callback || skirmishAIId == -1) return 0;

    if (released && topic != EVENT_RELEASE) {
        return 0;
    }

    if (topic == EVENT_RELEASE) {
        const int reason = data ? static_cast<const SReleaseEvent*>(data)->reason : 0;
        Release(reason);
        return 0;
    }

    if (topic == EVENT_INIT) {
        CacheStaticData();
        UpdateObservation();
    }

    // Capture events
    json ev = EventToJson(topic, data);
    if (!ev.is_null() && ev.contains("topic")) {
        eventBuffer.push_back(ev);
    }

    if (topic == EVENT_UPDATE) {
        const int frame = data ? static_cast<const SUpdateEvent*>(data)->frame : -1;
        if (frame >= 0) {
            frameFinished = false;
        }

        ProcessCommands();
        UpdateObservation();
        WaitForResume();
    }
    
    return 0;
}

} // namespace controllerai
