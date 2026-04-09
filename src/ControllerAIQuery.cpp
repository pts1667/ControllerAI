#include "ControllerAI.h"

#include "ControllerAICommon.h"
#include "ControllerAISerialization.h"
#include "ControllerAIServer.h"

#include "Economy.h"
#include "Feature.h"
#include "FeatureDef.h"
#include "Game.h"
#include "Map.h"
#include "OOAICallback.h"
#include "Resource.h"
#include "Unit.h"
#include "UnitDef.h"
#include "WrappFeature.h"
#include "WrappUnit.h"
#include "WrappUnitDef.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

namespace controllerai {
namespace {

int ReadIntValue(const json& query, const char* key) {
    if (!query.contains(key)) {
        throw std::runtime_error(std::string("Missing query parameter: ") + key);
    }

    const json& value = query.at(key);
    if (value.is_number_integer()) {
        return value.get<int>();
    }
    if (value.is_number_unsigned()) {
        return static_cast<int>(value.get<unsigned int>());
    }
    if (value.is_string()) {
        return std::stoi(value.get<std::string>());
    }

    throw std::runtime_error(std::string("Query parameter '") + key + "' must be an integer.");
}

float ReadFloatValue(const json& query, const char* key) {
    if (!query.contains(key)) {
        throw std::runtime_error(std::string("Missing query parameter: ") + key);
    }

    const json& value = query.at(key);
    if (value.is_number()) {
        return value.get<float>();
    }
    if (value.is_string()) {
        return std::stof(value.get<std::string>());
    }

    throw std::runtime_error(std::string("Query parameter '") + key + "' must be a number.");
}

std::string ReadStringValue(const json& query, const char* key) {
    if (!query.contains(key)) {
        throw std::runtime_error(std::string("Missing query parameter: ") + key);
    }

    const json& value = query.at(key);
    if (!value.is_string()) {
        throw std::runtime_error(std::string("Query parameter '") + key + "' must be a string.");
    }

    return value.get<std::string>();
}

springai::AIFloat3 ReadPositionValue(const json& query) {
    springai::AIFloat3 pos = {0.0f, 0.0f, 0.0f};

    if (query.contains("pos")) {
        const json& rawPos = query.at("pos");
        if (!rawPos.is_array() || rawPos.size() < 3) {
            throw std::runtime_error("Query parameter 'pos' must be an array of three numbers.");
        }

        pos.x = rawPos[0].get<float>();
        pos.y = rawPos[1].get<float>();
        pos.z = rawPos[2].get<float>();
        return pos;
    }

    pos.x = ReadFloatValue(query, "x");
    pos.y = query.contains("y") ? ReadFloatValue(query, "y") : 0.0f;
    pos.z = ReadFloatValue(query, "z");
    return pos;
}

springai::UnitDef* FindUnitDefById(int skirmishAIId, int unitDefId) {
    if (skirmishAIId == -1 || unitDefId < 0) {
        return nullptr;
    }

    return springai::WrappUnitDef::GetInstance(skirmishAIId, unitDefId);
}

springai::FeatureDef* FindFeatureDefById(springai::OOAICallback* callback, int featureDefId) {
    if (!callback) {
        return nullptr;
    }

    std::vector<springai::FeatureDef*> defs = callback->GetFeatureDefs();
    for (springai::FeatureDef* def : defs) {
        if (def && def->GetFeatureDefId() == featureDefId) {
            return def;
        }
    }

    return nullptr;
}

springai::FeatureDef* FindFeatureDefByName(springai::OOAICallback* callback, const std::string& name) {
    if (!callback) {
        return nullptr;
    }

    std::vector<springai::FeatureDef*> defs = callback->GetFeatureDefs();
    for (springai::FeatureDef* def : defs) {
        if (def && detail::SafeCString(def->GetName()) == name) {
            return def;
        }
    }

    return nullptr;
}

} // namespace

json CControllerAI::GetSettings() const {
    return json({
        {"block_n_frames", blockNFrames}
    });
}

bool CControllerAI::ShouldPublishObservationForFrame(int frame) const {
    if (frame < 0) {
        return true;
    }

    const int interval = std::max(blockNFrames, 1);
    return (frame % interval) == 0;
}

json CControllerAI::HandleQuery(const json& query) {
    if (!callback) {
        throw std::runtime_error("AI callback is not available.");
    }

    const std::string type = ReadStringValue(query, "type");

    if (type == "unit_def_id_by_name") {
        springai::UnitDef* unitDef = callback->GetUnitDefByName(ReadStringValue(query, "name").c_str());
        if (!unitDef) {
            throw std::runtime_error("Unknown UnitDef name.");
        }
        return unitDef->GetUnitDefId();
    }

    if (type == "unit_def_by_id") {
        springai::UnitDef* unitDef = FindUnitDefById(skirmishAIId, ReadIntValue(query, "unitDefId"));
        if (!unitDef) {
            throw std::runtime_error("Unknown UnitDef ID.");
        }
        return detail::SerializeUnitDef(unitDef, callback);
    }

    if (type == "unit_def_by_name") {
        springai::UnitDef* unitDef = callback->GetUnitDefByName(ReadStringValue(query, "name").c_str());
        if (!unitDef) {
            throw std::runtime_error("Unknown UnitDef name.");
        }
        return detail::SerializeUnitDef(unitDef, callback);
    }

    if (type == "unit_def_id_by_unit_id") {
        springai::Unit* unit = springai::WrappUnit::GetInstance(skirmishAIId, ReadIntValue(query, "unitId"));
        if (!unit) {
            throw std::runtime_error("Unknown Unit ID.");
        }

        springai::UnitDef* unitDef = unit->GetDef();
        if (!unitDef) {
            throw std::runtime_error("Unit has no UnitDef.");
        }
        return unitDef->GetUnitDefId();
    }

    if (type == "unit_def_by_unit_id") {
        springai::Unit* unit = springai::WrappUnit::GetInstance(skirmishAIId, ReadIntValue(query, "unitId"));
        if (!unit) {
            throw std::runtime_error("Unknown Unit ID.");
        }

        springai::UnitDef* unitDef = unit->GetDef();
        if (!unitDef) {
            throw std::runtime_error("Unit has no UnitDef.");
        }
        return detail::SerializeUnitDef(unitDef, callback);
    }

    if (type == "unit_by_id") {
        springai::Unit* unit = springai::WrappUnit::GetInstance(skirmishAIId, ReadIntValue(query, "unitId"));
        if (!unit) {
            throw std::runtime_error("Unknown Unit ID.");
        }
        return detail::SerializeUnitDetails(unit, callback);
    }

    if (type == "feature_by_id") {
        springai::Feature* feature = springai::WrappFeature::GetInstance(skirmishAIId, ReadIntValue(query, "featureId"));
        if (!feature) {
            throw std::runtime_error("Unknown Feature ID.");
        }
        return detail::SerializeFeature(feature, callback);
    }

    if (type == "feature_def_by_id") {
        springai::FeatureDef* featureDef = FindFeatureDefById(callback, ReadIntValue(query, "featureDefId"));
        if (!featureDef) {
            throw std::runtime_error("Unknown FeatureDef ID.");
        }
        return detail::SerializeFeatureDef(featureDef, callback);
    }

    if (type == "feature_def_by_name") {
        springai::FeatureDef* featureDef = FindFeatureDefByName(callback, ReadStringValue(query, "name"));
        if (!featureDef) {
            throw std::runtime_error("Unknown FeatureDef name.");
        }
        return detail::SerializeFeatureDef(featureDef, callback);
    }

    if (type == "resource_by_name") {
        springai::Resource* resource = callback->GetResourceByName(ReadStringValue(query, "name").c_str());
        if (!resource) {
            throw std::runtime_error("Unknown resource name.");
        }

        json result = detail::SerializeResource(resource);
        if (map) {
            result["extractorRadius"] = map->GetExtractorRadius(resource);
            result["maxResource"] = map->GetMaxResource(resource);
            result["averageSpotIncome"] = map->GetResourceMapSpotsAverageIncome(resource);
        }
        if (economy) {
            result["economy"] = {
                {"current", economy->GetCurrent(resource)},
                {"storage", economy->GetStorage(resource)},
                {"income", economy->GetIncome(resource)},
                {"usage", economy->GetUsage(resource)},
                {"excess", economy->GetExcess(resource)}
            };
        }
        return result;
    }

    if (type == "resource_spots_by_name") {
        if (!map) {
            throw std::runtime_error("Map interface is not available.");
        }

        springai::Resource* resource = callback->GetResourceByName(ReadStringValue(query, "name").c_str());
        if (!resource) {
            throw std::runtime_error("Unknown resource name.");
        }

        json spots = json::array();
        std::vector<springai::AIFloat3> positions = map->GetResourceMapSpotsPositions(resource);
        for (const springai::AIFloat3& pos : positions) {
            spots.push_back(json::array({pos.x, pos.y, pos.z}));
        }

        return json({
            {"resource", detail::SafeCString(resource->GetName())},
            {"averageIncome", map->GetResourceMapSpotsAverageIncome(resource)},
            {"spots", spots}
        });
    }

    if (type == "nearest_resource_spot") {
        if (!map) {
            throw std::runtime_error("Map interface is not available.");
        }

        springai::Resource* resource = callback->GetResourceByName(ReadStringValue(query, "name").c_str());
        if (!resource) {
            throw std::runtime_error("Unknown resource name.");
        }

        const springai::AIFloat3 origin = ReadPositionValue(query);
        const springai::AIFloat3 nearest = map->GetResourceMapSpotsNearest(resource, origin);
        return json({
            {"resource", detail::SafeCString(resource->GetName())},
            {"from", json::array({origin.x, origin.y, origin.z})},
            {"pos", json::array({nearest.x, nearest.y, nearest.z})}
        });
    }

    if (type == "elevation_at") {
        if (!map) {
            throw std::runtime_error("Map interface is not available.");
        }

        const springai::AIFloat3 pos = ReadPositionValue(query);
        return json({
            {"pos", json::array({pos.x, pos.y, pos.z})},
            {"elevation", map->GetElevationAt(pos.x, pos.z)}
        });
    }

    if (type == "start_position") {
        if (!map) {
            throw std::runtime_error("Map interface is not available.");
        }

        const springai::AIFloat3 pos = map->GetStartPos();
        return json({
            {"pos", json::array({pos.x, pos.y, pos.z})}
        });
    }

    if (type == "can_build_at") {
        if (!map) {
            throw std::runtime_error("Map interface is not available.");
        }

        springai::UnitDef* unitDef = FindUnitDefById(skirmishAIId, ReadIntValue(query, "unitDefId"));
        if (!unitDef) {
            throw std::runtime_error("Unknown UnitDef ID.");
        }

        const springai::AIFloat3 pos = ReadPositionValue(query);
        const int facing = query.contains("facing") ? ReadIntValue(query, "facing") : 0;
        return json({
            {"possible", map->IsPossibleToBuildAt(unitDef, pos, facing)},
            {"pos", json::array({pos.x, pos.y, pos.z})},
            {"facing", facing}
        });
    }

    if (type == "closest_build_site") {
        if (!map) {
            throw std::runtime_error("Map interface is not available.");
        }

        springai::UnitDef* unitDef = FindUnitDefById(skirmishAIId, ReadIntValue(query, "unitDefId"));
        if (!unitDef) {
            throw std::runtime_error("Unknown UnitDef ID.");
        }

        const springai::AIFloat3 pos = ReadPositionValue(query);
        const float searchRadius = ReadFloatValue(query, "searchRadius");
        const int minDist = query.contains("minDist") ? ReadIntValue(query, "minDist") : 0;
        const int facing = query.contains("facing") ? ReadIntValue(query, "facing") : 0;
        const springai::AIFloat3 result = map->FindClosestBuildSite(unitDef, pos, searchRadius, minDist, facing);

        return json({
            {"found", result.x >= 0.0f},
            {"pos", json::array({result.x, result.y, result.z})},
            {"facing", facing},
            {"searchRadius", searchRadius},
            {"minDist", minDist}
        });
    }

    throw std::runtime_error("Unsupported query type.");
}

json CControllerAI::HandleSettings(const json& settingsPatch) {
    if (!settingsPatch.is_object()) {
        throw std::runtime_error("Settings payload must be a JSON object.");
    }

    if (settingsPatch.contains("block_n_frames")) {
        int newValue = ReadIntValue(settingsPatch, "block_n_frames");
        if (newValue < 1) {
            throw std::runtime_error("block_n_frames must be at least 1.");
        }
        blockNFrames = newValue;
    }

    if (game && game->GetCurrentFrame() >= 0 && !ShouldPublishObservationForFrame(game->GetCurrentFrame())) {
        frameFinished = true;
    }

    json settings = GetSettings();
    if (server) {
        server->PublishSettings(settings);
    }
    return settings;
}

void CControllerAI::ProcessQueries() {
    if (!server) {
        return;
    }

    server->ProcessQueries([this](const json& query) {
        return HandleQuery(query);
    });
}

void CControllerAI::ProcessSettings() {
    if (!server) {
        return;
    }

    server->ProcessSettings([this](const json& settingsPatch) {
        return HandleSettings(settingsPatch);
    });
}

} // namespace controllerai