#include "ControllerAI.h"
#include "ExternalAI/Interface/AISEvents.h"
#include "ExternalAI/Interface/AISCommands.h"

#include "Game.h"
#include "Map.h"
#include "Unit.h"
#include "UnitDef.h"
#include "Resource.h"
#include "Economy.h"
#include "Cheats.h"
#include "Lua.h"

#include <iostream>
#include <chrono>

namespace controllerai {

CControllerAI::CControllerAI(springai::OOAICallback* callback) :
    callback(callback),
    skirmishAIId(callback != nullptr ? callback->GetSkirmishAIId() : -1),
    running(true)
{
    serverThread = std::thread(&CControllerAI::ServerThread, this);
}

CControllerAI::~CControllerAI() {
    running = false;
    svr.stop();
    if (serverThread.joinable()) {
        serverThread.join();
    }
}

void CControllerAI::ServerThread() {
    svr.Get("/observation", [this](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(stateMutex);
        res.set_content(lastObservation.dump(), "application/json");
    });

    svr.Get("/metadata", [this](const httplib::Request&, httplib::Response& res) {
        if (unitDefsCache.empty()) {
            CacheMetadata();
        }
        res.set_content(unitDefsCache.dump(), "application/json");
    });

    svr.Post("/command", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            json cmd = json::parse(req.body);
            {
                std::lock_guard<std::mutex> lock(commandQueueMutex);
                commandQueue.push(cmd);
            }
            res.set_content("{\"status\": \"queued\"}", "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json({{"error", e.what()}}).dump(), "application/json");
        }
    });

    svr.listen("0.0.0.0", 3017);
}

void CControllerAI::CacheMetadata() {
    if (!callback) return;
    std::vector<springai::UnitDef*> defs = callback->GetUnitDefs();
    json jDefs = json::object();
    for (springai::UnitDef* def : defs) {
        json d;
        d["name"] = def->GetName();
        d["humanName"] = def->GetHumanName();
        d["buildTime"] = def->GetBuildTime();
        d["health"] = def->GetHealth();
        d["speed"] = def->GetSpeed();
        d["range"] = def->GetMaxWeaponRange();
        // Add more as needed
        jDefs[std::to_string(def->GetUnitDefId())] = d;
        delete def;
    }
    unitDefsCache = jDefs;
}

void CControllerAI::UpdateObservation() {
    if (!callback) return;

    json obs;
    const std::unique_ptr<springai::Game> game(callback->GetGame());
    obs["frame"] = game->GetCurrentFrame();

    // Friendly units
    obs["units"] = json::object();
    std::vector<springai::Unit*> myUnits = callback->GetFriendlyUnits();
    for (springai::Unit* unit : myUnits) {
        json u;
        u["defId"] = unit->GetDef()->GetUnitDefId();
        springai::float3 pos = unit->GetPos();
        u["pos"] = {pos.x, pos.y, pos.z};
        u["health"] = unit->GetHealth();
        obs["units"][std::to_string(unit->GetUnitId())] = u;
        delete unit;
    }

    // Enemy units
    obs["enemies"] = json::object();
    std::vector<springai::Unit*> enemies = callback->GetEnemyUnits();
    for (springai::Unit* unit : enemies) {
        json e;
        springai::float3 pos = unit->GetPos();
        e["pos"] = {pos.x, pos.y, pos.z};
        obs["enemies"][std::to_string(unit->GetUnitId())] = e;
        delete unit;
    }

    // Economy
    const std::unique_ptr<springai::Economy> eco(callback->GetEconomy());
    std::vector<springai::Resource*> resources = callback->GetResources();
    obs["economy"] = json::object();
    for (springai::Resource* res : resources) {
        json r;
        r["current"] = eco->GetCurrent(res);
        r["storage"] = eco->GetStorage(res);
        r["income"] = eco->GetIncome(res);
        r["usage"] = eco->GetUsage(res);
        obs["economy"][res->GetName()] = r;
        delete res;
    }

    std::lock_guard<std::mutex> lock(stateMutex);
    lastObservation = obs;
}

void CControllerAI::ProcessCommands() {
    std::lock_guard<std::mutex> lock(commandQueueMutex);
    while (!commandQueue.empty()) {
        json cmd = commandQueue.front();
        commandQueue.pop();

        try {
            std::string type = cmd["type"];
            if (type == "move") {
                int unitId = cmd["unitId"];
                springai::Unit* unit = springai::Unit::GetInstance(skirmishAIId, unitId);
                if (unit) {
                    springai::float3 pos = {cmd["pos"][0], cmd["pos"][1], cmd["pos"][2]};
                    unit->MoveTo(pos, 0, 100000);
                    delete unit;
                }
            } else if (type == "attack") {
                int unitId = cmd["unitId"];
                int targetId = cmd["targetId"];
                springai::Unit* unit = springai::Unit::GetInstance(skirmishAIId, unitId);
                springai::Unit* target = springai::Unit::GetInstance(skirmishAIId, targetId);
                if (unit && target) {
                    unit->Attack(target, 0, 100000);
                }
                delete unit;
                delete target;
            } else if (type == "build") {
                int unitId = cmd["unitId"];
                int defId = cmd["defId"];
                springai::Unit* unit = springai::Unit::GetInstance(skirmishAIId, unitId);
                if (unit) {
                    springai::UnitDef* def = springai::UnitDef::GetInstance(skirmishAIId, defId);
                    springai::float3 pos = {cmd["pos"][0], cmd["pos"][1], cmd["pos"][2]};
                    unit->Build(def, pos, 0, 0, 100000);
                    delete unit;
                    delete def;
                }
            } else if (type == "lua") {
                const std::unique_ptr<springai::Lua> lua(callback->GetLua());
                std::string data = cmd["data"];
                lua->CallRules(data.c_str());
            }
        } catch (...) {}
    }
}

json CControllerAI::EventToJson(int topic, const void* data) {
    json e;
    e["topic"] = topic;
    switch (topic) {
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
    // Forward all events as "last_event" or similar? 
    // For now, update state on important ones.
    
    if (topic == EVENT_INIT) {
        CacheMetadata();
    }

    UpdateObservation();
    ProcessCommands();

    // We could also have a websocket or long-poll for events.
    // For a simple HTTP server, we'll just store the last few events or 
    // let the user poll /observation which is updated here.
    
    return 0;
}

} // namespace controllerai
