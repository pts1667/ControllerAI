#include "ControllerAI.h"
#include "ExternalAI/Interface/AISEvents.h"
#include "ExternalAI/Interface/AISCommands.h"

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

namespace controllerai {

CControllerAI::CControllerAI(springai::OOAICallback* callback) :
    callback(callback),
    skirmishAIId(callback != nullptr ? callback->GetSkirmishAIId() : -1),
    running(true),
    synchronousMode(true),
    frameFinished(true),
    eventBuffer(json::array())
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

    // Load options from engine
    bindAddress = "0.0.0.0";
    port = 3017;

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
            delete options;
        }
    }

    CacheStaticData();
    UpdateObservation(); // Generate frame -1 observation

    serverThread = std::thread(&CControllerAI::ServerThread, this);
}

CControllerAI::~CControllerAI() {
    running = false;
    svr.stop();
    if (serverThread.joinable()) {
        serverThread.join();
    }
    // unique_ptrs will be destroyed automatically
}

void CControllerAI::ServerThread() {
    svr.Get("/observation", [this](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(stateMutex);
        res.set_content(lastObservation.dump(), "application/json");
    });

    svr.Get("/metadata", [this](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(stateMutex);
        res.set_content(unitDefsCache.dump(), "application/json");
    });

    svr.Get("/game_info", [this](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(stateMutex);
        res.set_content(gameInfoCache.dump(), "application/json");
    });

    svr.Get("/spawn_boxes", [this](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(stateMutex);
        res.set_content(spawnBoxesCache.dump(), "application/json");
    });

    svr.Get("/map_features", [this](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(stateMutex);
        res.set_content(mapFeaturesCache.dump(), "application/json");
    });

    svr.Get("/heightmap", [this](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(stateMutex);
        res.set_content(heightmapCache.dump(), "application/json");
    });

    svr.Post("/command", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            json cmd = json::parse(req.body);
            {
                std::lock_guard<std::mutex> lock(commandQueueMutex);
                commandQueue.push(cmd);
            }
            cv.notify_all(); // Wake up engine thread if it's waiting
            res.set_content("{\"status\": \"queued\"}", "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json({{"error", e.what()}}).dump(), "application/json");
        }
    });

    svr.listen(bindAddress.c_str(), port);
}

void CControllerAI::CacheStaticData() {
    if (!callback || !game || !map || !mod) return;
    std::lock_guard<std::mutex> lock(stateMutex);

    // 1. Game Info
    gameInfoCache = json::object();
    gameInfoCache["modName"] = mod->GetHumanName();
    gameInfoCache["mapName"] = map->GetHumanName();
    gameInfoCache["gameMode"] = game->GetMode();
    gameInfoCache["isPaused"] = game->IsPaused();
    std::string script = game->GetSetupScript();
    canChooseStartPos = (script.find("startpostype=1") != std::string::npos);
    gameInfoCache["canChooseStartPos"] = canChooseStartPos;
    setupComplete = !canChooseStartPos;

    // 2. Spawn Boxes
    spawnBoxesCache = json::object();
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
        for (std::sregex_iterator i = std_it; i != zk_end; ++i) {
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

    // 3. Unit Metadata
    unitDefsCache = json::object();
    std::vector<springai::UnitDef*> defs = callback->GetUnitDefs();
    for (springai::UnitDef* def : defs) {
        json d;
        d["name"] = def->GetName();
        d["humanName"] = def->GetHumanName();
        d["buildTime"] = def->GetBuildTime();
        d["health"] = def->GetHealth();
        d["speed"] = def->GetSpeed();
        d["range"] = def->GetMaxWeaponRange();
        unitDefsCache[std::to_string(def->GetUnitDefId())] = d;
        delete def;
    }

    // 4. Map Features & Resources
    mapFeaturesCache = json::object();
    mapFeaturesCache["spots"] = json::array();
    std::vector<springai::Resource*> resources = callback->GetResources();
    for (springai::Resource* res_ptr : resources) {
        std::vector<springai::AIFloat3> spots = map->GetResourceMapSpotsPositions(res_ptr);
        for (const auto& spot_pos : spots) {
            json s;
            s["resource"] = res_ptr->GetName();
            s["pos"] = json::array({spot_pos.x, spot_pos.y, spot_pos.z});
            mapFeaturesCache["spots"].push_back(s);
        }
        delete res_ptr;
    }
    mapFeaturesCache["features"] = json::array();
    std::vector<springai::Feature*> features = callback->GetFeatures();
    for (springai::Feature* f : features) {
        json fj;
        fj["id"] = f->GetFeatureId();
        springai::AIFloat3 f_pos = f->GetPosition();
        fj["pos"] = json::array({f_pos.x, f_pos.y, f_pos.z});
        fj["health"] = f->GetHealth();
        springai::FeatureDef* fdef = f->GetDef();
        if (fdef) {
            fj["name"] = fdef->GetName();
            delete fdef;
        }
        mapFeaturesCache["features"].push_back(fj);
        delete f;
    }

    // 5. Heightmap
    heightmapCache = json::object();
    int h_width = map->GetWidth();
    int h_height = map->GetHeight();
    std::vector<float> heights = map->GetHeightMap();
    heightmapCache["width"] = h_width;
    heightmapCache["height"] = h_height;
    heightmapCache["data_b64"] = Base64Encode(reinterpret_cast<const unsigned char*>(heights.data()), heights.size() * sizeof(float));
}

bool CControllerAI::IsSpawnPosValid(const springai::AIFloat3& pos) {
    if (!game || !lua) return false;
    int myAlly = game->GetMyAllyTeam();
    
    std::string allyStr = std::to_string(myAlly);
    std::lock_guard<std::mutex> lock(stateMutex);
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

void CControllerAI::UpdateObservation() {
    if (!game || !economy) return;

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
        json u;
        springai::UnitDef* def = unit->GetDef();
        if (def) {
            u["defId"] = def->GetUnitDefId();
            delete def;
        }
        springai::AIFloat3 pos = unit->GetPos();
        springai::AIFloat3 vel = unit->GetVel();
        u["pos"] = json::array({pos.x, pos.y, pos.z});
        u["vel"] = json::array({vel.x, vel.y, vel.z});
        u["health"] = unit->GetHealth();
        u["maxHealth"] = unit->GetMaxHealth();
        u["experience"] = unit->GetExperience();
        u["buildProgress"] = unit->GetBuildProgress();
        u["isBeingBuilt"] = unit->IsBeingBuilt();
        u["isCloaked"] = unit->IsCloaked();
        u["isParalyzed"] = unit->IsParalyzed();
        obs["units"][std::to_string(unit->GetUnitId())] = u;
        delete unit;
    }

    // Enemy units
    obs["enemies"] = json::object();
    std::vector<springai::Unit*> enemies = callback->GetEnemyUnits();
    for (springai::Unit* unit : enemies) {
        json e;
        springai::AIFloat3 pos = unit->GetPos();
        e["pos"] = json::array({pos.x, pos.y, pos.z});
        
        springai::UnitDef* def = unit->GetDef();
        if (def) {
            e["defId"] = def->GetUnitDefId();
            delete def;
        }
        e["health"] = unit->GetHealth();
        
        obs["enemies"][std::to_string(unit->GetUnitId())] = e;
        delete unit;
    }

    // Economy
    std::vector<springai::Resource*> resources = callback->GetResources();
    obs["economy"] = json::object();
    for (springai::Resource* res_ptr : resources) {
        json r;
        r["current"] = economy->GetCurrent(res_ptr);
        r["storage"] = economy->GetStorage(res_ptr);
        r["income"] = economy->GetIncome(res_ptr);
        r["usage"] = economy->GetUsage(res_ptr);
        obs["economy"][res_ptr->GetName()] = r;
        delete res_ptr;
    }

    std::lock_guard<std::mutex> lock(stateMutex);
    obs["events"] = eventBuffer;
    eventBuffer = json::array(); // Clear buffer after poll
    lastObservation = obs;
}

void CControllerAI::ProcessCommands() {
    std::lock_guard<std::mutex> lock(commandQueueMutex);
    while (!commandQueue.empty()) {
        json cmd = commandQueue.front();
        commandQueue.pop();

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
                delete target;
            } else if (type == "build" && unit) {
                int defId = cmd["defId"];
                springai::UnitDef* def = springai::WrappUnitDef::GetInstance(skirmishAIId, defId);
                springai::AIFloat3 pos;
                pos.x = cmd["pos"][0];
                pos.y = cmd["pos"][1];
                pos.z = cmd["pos"][2];
                unit->Build(def, pos, cmd.value("facing", 0), opts, 100000);
                delete def;
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
                delete target;
            } else if (type == "repair" && unit) {
                int targetId = cmd["targetId"];
                springai::Unit* target = springai::WrappUnit::GetInstance(skirmishAIId, targetId);
                if (target) unit->Repair(target, opts, 100000);
                delete target;
            } else if (type == "reclaim" && unit) {
                if (cmd.contains("targetId")) {
                    springai::Unit* target = springai::WrappUnit::GetInstance(skirmishAIId, cmd["targetId"]);
                    if (target) unit->ReclaimUnit(target, opts, 100000);
                    delete target;
                } else if (cmd.contains("featureId")) {
                    springai::Feature* feature = springai::WrappFeature::GetInstance(skirmishAIId, cmd["featureId"]);
                    if (feature) unit->ReclaimFeature(feature, opts, 100000);
                    delete feature;
                }
            } else if (type == "resurrect" && unit) {
                springai::Feature* feature = springai::WrappFeature::GetInstance(skirmishAIId, cmd["featureId"]);
                if (feature) unit->Resurrect(feature, opts, 100000);
                delete feature;
            } else if (type == "capture" && unit) {
                int targetId = cmd["targetId"];
                springai::Unit* target = springai::WrappUnit::GetInstance(skirmishAIId, targetId);
                if (target) unit->Capture(target, opts, 100000);
                delete target;
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
                        std::lock_guard<std::mutex> lock(cvMutex);
                        setupComplete = true;
                        cv.notify_all();
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
                std::lock_guard<std::mutex> lock(cvMutex);
                frameFinished = true;
                cv.notify_one();
            }
            delete unit;
        } catch (...) {}
    }
}

json CControllerAI::EventToJson(int topic, const void* data) {
    json e;
    e["topic"] = topic;
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
        case EVENT_ENEMY_ENTER_LOS: {
            struct SEnemyEnterLOSEvent* ev = (struct SEnemyEnterLOSEvent*)data;
            e["enemyId"] = ev->enemy;
            break;
        }
        case EVENT_LUA_MESSAGE: {
            struct SLuaMessageEvent* ev = (struct SLuaMessageEvent*)data;
            e["data"] = ev->inData;
            break;
        }
        default: break;
    }
    return e;
}

int CControllerAI::HandleEvent(int topic, const void* data) {
    if (topic == EVENT_INIT) {
        CacheMetadata();
    }

    // Capture events
    json ev = EventToJson(topic, data);
    if (!ev.is_null() && ev.contains("topic")) {
        std::lock_guard<std::mutex> lock(stateMutex);
        eventBuffer.push_back(ev);
    }

    UpdateObservation();
    ProcessCommands();

    // 1. Mandatory Setup Blocking (even if sync=false)
    // We block the engine thread at frame -1 until the AI is ready.
    if (game && game->GetCurrentFrame() < 0 && !setupComplete) {
        auto start = std::chrono::steady_clock::now();
        bool warned = false;
        while (!setupComplete && running) {
            UpdateObservation(); // Refresh for the poll
            ProcessCommands();
            if (setupComplete) break;
            std::unique_lock<std::mutex> lock(cvMutex);
            if (cv.wait_for(lock, std::chrono::seconds(1)) == std::cv_status::timeout) {
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start).count();
                if (elapsed >= 10 && !warned) {
                    if (log) log->DoLog("ControllerAI Warning: Engine has been blocked for more than 10s during setup phase! Waiting for set_start_pos command.");
                    warned = true;
                }
            }
        }
    }

    // 2. Standard Synchronous Blocking (frame 0 onwards)
    if (synchronousMode && topic == EVENT_UPDATE && game && game->GetCurrentFrame() >= 0) {
        {
            std::lock_guard<std::mutex> lock(cvMutex);
            frameFinished = false;
        }
        
        auto start = std::chrono::steady_clock::now();
        bool warned = false;
        std::unique_lock<std::mutex> lock(cvMutex);
        while (!frameFinished && running) {
            if (cv.wait_for(lock, std::chrono::seconds(1)) == std::cv_status::timeout) {
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start).count();
                if (elapsed >= 10 && !warned) {
                    if (log) {
                        std::stringstream ss;
                        ss << "ControllerAI Warning: Engine has been blocked for more than 10s on frame " << game->GetCurrentFrame() << "! Waiting for finish_frame command.";
                        log->DoLog(ss.str().c_str());
                    }
                    warned = true;
                }
            }
        }
    }
    
    return 0;
}

} // namespace controllerai
