#include "ControllerAI.h"

#include "ControllerAICommon.h"
#include "ControllerAISerialization.h"
#include "ControllerAIServer.h"

#include "Economy.h"
#include "Feature.h"
#include "Game.h"
#include "Lua.h"
#include "OOAICallback.h"
#include "Resource.h"
#include "Unit.h"
#include "UnitDef.h"
#include "WrappFeature.h"
#include "WrappUnit.h"
#include "WrappUnitDef.h"

#include <chrono>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <vector>

namespace controllerai {

void CControllerAI::UpdateObservation() {
    if (!callback || !game || !economy || !server) {
        return;
    }

    json observation;
    observation["frame"] = game->GetCurrentFrame();
    observation["aiId"] = skirmishAIId;
    observation["teamId"] = game->GetMyTeam();
    observation["allyTeamId"] = game->GetMyAllyTeam();

    observation["units"] = json::object();
    std::vector<springai::Unit*> myUnits = callback->GetFriendlyUnits();
    for (springai::Unit* unit : myUnits) {
        if (!unit) {
            continue;
        }

        auto [unitId, data] = detail::ParseUnit(unit);
        if (unitId == -1) {
            continue;
        }
        observation["units"][std::to_string(unitId)] = data;
    }

    observation["enemies"] = json::object();
    std::unordered_set<int> enemyIds;
    std::vector<springai::Unit*> enemies = callback->GetEnemyUnits();
    for (springai::Unit* unit : enemies) {
        if (!unit) {
            continue;
        }

        auto [unitId, data] = detail::ParseUnit(unit);
        if (unitId == -1) {
            continue;
        }
        enemyIds.insert(unitId);
        data["inLos"] = true;
        observation["enemies"][std::to_string(unitId)] = data;
    }

    observation["radarBlips"] = json::object();
    std::vector<springai::Unit*> radarContacts = callback->GetEnemyUnitsInRadarAndLos();
    for (springai::Unit* unit : radarContacts) {
        if (!unit) {
            continue;
        }

        auto [unitId, blip] = detail::ParseRadarBlip(unit);
        if (unitId == -1 || enemyIds.find(unitId) != enemyIds.end()) {
            continue;
        }

        observation["radarBlips"][std::to_string(unitId)] = blip;
    }

    observation["features"] = json::object();
    springai::Resource* metalResource = callback->GetResourceByName("Metal");
    if (!metalResource) {
        std::vector<springai::Resource*> resources = callback->GetResources();
        for (springai::Resource* resource : resources) {
            if (!resource) {
                continue;
            }

            const std::string resourceName = detail::SafeCString(resource->GetName());
            if (resourceName == "Metal" || resourceName == "metal") {
                metalResource = resource;
                break;
            }
        }
    }

    std::vector<springai::Feature*> features = callback->GetFeatures();
    for (springai::Feature* feature : features) {
        if (!feature) {
            continue;
        }

        springai::FeatureDef* featureDef = feature->GetDef();
        if (!featureDef || !featureDef->IsReclaimable() || !metalResource) {
            continue;
        }

        if (featureDef->GetContainedResource(metalResource) < 1.0f) {
            continue;
        }

        json data = detail::SerializeFeature(feature, callback);
        if (!data.is_object()) {
            continue;
        }

        observation["features"][std::to_string(feature->GetFeatureId())] = std::move(data);
    }

    observation["economy"] = json::object();
    std::vector<springai::Resource*> resources = callback->GetResources();
    for (springai::Resource* resource : resources) {
        if (!resource) {
            continue;
        }

        json economyState;
        economyState["current"] = economy->GetCurrent(resource);
        economyState["storage"] = economy->GetStorage(resource);
        economyState["income"] = economy->GetIncome(resource);
        economyState["usage"] = economy->GetUsage(resource);
        observation["economy"][detail::SafeCString(resource->GetName())] = economyState;
    }

    observation["events"] = eventBuffer;
    eventBuffer = json::array();
    server->PublishObservation(std::move(observation));
}

bool CControllerAI::IsSpawnPosValid(const springai::AIFloat3& pos) {
    if (!game || !lua || !server) {
        return false;
    }

    const std::string allyKey = std::to_string(game->GetMyAllyTeam());
    json spawnBoxesCache = server->GetSpawnBoxes();
    if (spawnBoxesCache.contains(allyKey) && !detail::IsPointInSpawnBox(spawnBoxesCache[allyKey], pos.x, pos.z)) {
        return false;
    }

    const std::string separators[] = {",", "/"};
    for (const std::string& separator : separators) {
        std::stringstream stream;
        stream << "ai_is_valid_startpos:" << static_cast<int>(pos.x) << separator << static_cast<int>(pos.z);
        const std::string message = stream.str();
        std::string result = lua->CallRules(message.c_str(), static_cast<int>(message.size()));
        if (result == "not_valid" || result == "0") {
            return false;
        }
    }

    return true;
}

void CControllerAI::ProcessCommands() {
    if (!server) {
        return;
    }

    std::vector<json> commands = server->DrainCommands();
    for (json& cmd : commands) {
        std::string type = "<unknown>";
        if (cmd.is_object()) {
            type = cmd.value("type", std::string("<missing>"));
        } else {
            type = "<non-object>";
        }

        try {
            if (!cmd.is_object()) {
                throw std::runtime_error("Command payload must be a JSON object.");
            }

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
                if (target) {
                    unit->Attack(target, opts, 100000);
                }
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
                if (target) {
                    unit->Guard(target, opts, 100000);
                }
            } else if (type == "repair" && unit) {
                int targetId = cmd["targetId"];
                springai::Unit* target = springai::WrappUnit::GetInstance(skirmishAIId, targetId);
                if (target) {
                    unit->Repair(target, opts, 100000);
                }
            } else if (type == "reclaim" && unit) {
                if (cmd.contains("targetId")) {
                    springai::Unit* target = springai::WrappUnit::GetInstance(skirmishAIId, cmd["targetId"]);
                    if (target) {
                        unit->ReclaimUnit(target, opts, 100000);
                    }
                } else if (cmd.contains("featureId")) {
                    springai::Feature* feature = springai::WrappFeature::GetInstance(skirmishAIId, cmd["featureId"]);
                    if (feature) {
                        unit->ReclaimFeature(feature, opts, 100000);
                    }
                }
            } else if (type == "resurrect" && unit) {
                springai::Feature* feature = springai::WrappFeature::GetInstance(skirmishAIId, cmd["featureId"]);
                if (feature) {
                    unit->Resurrect(feature, opts, 100000);
                }
            } else if (type == "capture" && unit) {
                int targetId = cmd["targetId"];
                springai::Unit* target = springai::WrappUnit::GetInstance(skirmishAIId, targetId);
                if (target) {
                    unit->Capture(target, opts, 100000);
                }
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
                    if (game) {
                        game->SendStartPosition(ready, pos);
                    }
                    if (ready) {
                        setupComplete = true;
                        startupBlocking = false;
                    }
                } else if (game) {
                    game->SendTextMessage("ControllerAI: Invalid start position received!", 0);
                }
            } else if (type == "set_commander") {
                std::string name = cmd["name"];
                if (lua) {
                    std::string msg = "ai_commander:" + name;
                    lua->CallRules(msg.c_str(), static_cast<int>(msg.size()));
                }
            } else if (type == "set_side") {
                std::string name = cmd["name"];
                if (lua) {
                    std::string msg = "ai_side:" + name;
                    lua->CallRules(msg.c_str(), static_cast<int>(msg.size()));
                }
            } else if (type == "lua") {
                if (lua) {
                    std::string data = cmd["data"];
                    lua->CallRules(data.c_str(), static_cast<int>(data.size()));
                }
            } else if (type == "finish_frame") {
                if (game && game->GetCurrentFrame() < 0) {
                    startupBlocking = false;
                } else {
                    frameFinished = true;
                }
            }
        } catch (const std::exception& e) {
            std::ostringstream message;
            message << "ControllerAI: command execution failed"
                    << " type=" << type
                    << " error=" << e.what();
            LogToInfolog(message.str());
        } catch (...) {
            std::ostringstream message;
            message << "ControllerAI: command execution failed"
                    << " type=" << type
                    << " error=<unknown exception>";
            LogToInfolog(message.str());
        }
    }
}

void CControllerAI::WaitForResume() {
    if (!server) {
        return;
    }

    auto waitForWorkWithLogging = [this](const char* context) {
        while (running) {
            if (!server->WaitForWorkFor(1000)) {
                return false;
            }

            const CControllerAIServer::PendingWorkState workState = server->GetPendingWorkState();
            if (workState.HasAny()) {
                return true;
            }

            std::ostringstream message;
            message << "ControllerAI: WaitForResume blocked >1s"
                    << " context=" << context
                    << " frame=" << (game ? game->GetCurrentFrame() : -1)
                    << " frameFinished=" << (frameFinished ? "true" : "false")
                    << " commandQueueNonEmpty=" << (workState.hasCommands ? "true" : "false")
                    << " queryQueueNonEmpty=" << (workState.hasQueries ? "true" : "false")
                    << " settingsQueueNonEmpty=" << (workState.hasSettings ? "true" : "false");
            LogToInfolog(message.str());
        }

        return false;
    };

    while (running) {
        if (game && game->GetCurrentFrame() < 0 && startupBlocking) {
            if (!waitForWorkWithLogging("startup")) {
                return;
            }
            ProcessSettings();
            ProcessQueries();
            ProcessCommands();
            continue;
        }

        if (game && canChooseStartPos && !setupComplete) {
            if (!waitForWorkWithLogging("choose_start_pos")) {
                return;
            }
            ProcessSettings();
            ProcessQueries();
            ProcessCommands();
            continue;
        }

        if (!synchronousMode || !game || game->GetCurrentFrame() < 0 || frameFinished) {
            return;
        }

        if (!waitForWorkWithLogging("frame_sync")) {
            return;
        }
        ProcessSettings();
        ProcessQueries();
        ProcessCommands();
    }
}

} // namespace controllerai