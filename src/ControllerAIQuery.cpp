#include "ControllerAI.h"

#include "ControllerAICommon.h"
#include "ControllerAISerialization.h"
#include "ControllerAIServer.h"

#include "Command.h"
#include "CommandDescription.h"
#include "Economy.h"
#include "Feature.h"
#include "FeatureDef.h"
#include "Game.h"
#include "Info.h"
#include "Map.h"
#include "OOAICallback.h"
#include "OptionValues.h"
#include "Resource.h"
#include "SkirmishAI.h"
#include "Team.h"
#include "Unit.h"
#include "UnitDef.h"
#include "WrappFeature.h"
#include "WrappTeam.h"
#include "WrappUnit.h"
#include "WrappUnitDef.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <memory>
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

std::string TrimAsciiWhitespace(std::string value) {
    std::size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        ++start;
    }

    std::size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }

    return value.substr(start, end - start);
}

std::vector<std::string> ReadStringListValue(const json& query, const char* key) {
    if (!query.contains(key)) {
        throw std::runtime_error(std::string("Missing query parameter: ") + key);
    }

    const json& value = query.at(key);
    std::vector<std::string> result;

    if (value.is_string()) {
        std::string raw = value.get<std::string>();
        std::size_t start = 0;
        while (start <= raw.size()) {
            const std::size_t end = raw.find(',', start);
            std::string item = TrimAsciiWhitespace(raw.substr(start, end == std::string::npos ? std::string::npos : end - start));
            if (!item.empty()) {
                result.push_back(std::move(item));
            }

            if (end == std::string::npos) {
                break;
            }
            start = end + 1;
        }
    } else if (value.is_array()) {
        result.reserve(value.size());
        for (const json& entry : value) {
            if (!entry.is_string()) {
                throw std::runtime_error(std::string("Query parameter '") + key + "' must contain only strings.");
            }

            std::string item = TrimAsciiWhitespace(entry.get<std::string>());
            if (!item.empty()) {
                result.push_back(std::move(item));
            }
        }
    } else {
        throw std::runtime_error(std::string("Query parameter '") + key + "' must be a string or an array of strings.");
    }

    if (result.empty()) {
        throw std::runtime_error(std::string("Query parameter '") + key + "' must not be empty.");
    }

    return result;
}

float ReadOptionalFloatValue(const json& query, const char* key, float defaultValue) {
    return query.contains(key) ? ReadFloatValue(query, key) : defaultValue;
}

std::string ReadOptionalStringValue(const json& query, const char* key, const std::string& defaultValue) {
    return query.contains(key) ? ReadStringValue(query, key) : defaultValue;
}

json SerializeRulesParamValue(float floatValue, const char* stringValue) {
    return json({
        {"floatValue", floatValue},
        {"stringValue", detail::SafeCString(stringValue)}
    });
}

template <typename Source>
json SerializeRulesParams(Source* source, const std::vector<std::string>& keys, float defaultFloat, const std::string& defaultString) {
    json result = json::object();
    for (const std::string& key : keys) {
        result[key] = SerializeRulesParamValue(
            source->GetRulesParamFloat(key.c_str(), defaultFloat),
            source->GetRulesParamString(key.c_str(), defaultString.c_str())
        );
    }
    return result;
}

json SerializeOptionValues(springai::OptionValues* options, const std::vector<std::string>* keys = nullptr) {
    json result = json::object();
    if (!options) {
        return result;
    }

    if (keys != nullptr) {
        for (const std::string& key : *keys) {
            result[key] = detail::SafeCString(options->GetValueByKey(key.c_str()));
        }
        return result;
    }

    const int size = options->GetSize();
    for (int index = 0; index < size; ++index) {
        const char* key = options->GetKey(index);
        if (!key) {
            continue;
        }
        result[key] = detail::SafeCString(options->GetValue(index));
    }

    return result;
}

json SerializeInfoValues(springai::Info* info, const std::vector<std::string>* keys = nullptr) {
    json result = json::object();
    if (!info) {
        return result;
    }

    if (keys != nullptr) {
        for (const std::string& key : *keys) {
            result[key] = detail::SafeCString(info->GetValueByKey(key.c_str()));
        }
        return result;
    }

    const int size = info->GetSize();
    for (int index = 0; index < size; ++index) {
        const char* key = info->GetKey(index);
        if (!key) {
            continue;
        }
        result[key] = detail::SafeCString(info->GetValue(index));
    }

    return result;
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

json FindNearestSpot(const json& spots, const springai::AIFloat3& origin) {
    float bestDistanceSq = std::numeric_limits<float>::max();
    json bestSpot;

    if (!spots.is_array()) {
        return bestSpot;
    }

    for (const json& spot : spots) {
        if (!spot.is_object() || !spot.contains("pos") || !spot["pos"].is_array() || spot["pos"].size() < 3) {
            continue;
        }

        const float x = spot["pos"][0].get<float>();
        const float z = spot["pos"][2].get<float>();
        const float dx = x - origin.x;
        const float dz = z - origin.z;
        const float distanceSq = dx * dx + dz * dz;
        if (distanceSq < bestDistanceSq) {
            bestDistanceSq = distanceSq;
            bestSpot = spot;
        }
    }

    return bestSpot;
}

json SerializeCurrentCommands(springai::Unit* unit) {
    json commands = json::array();
    if (!unit) {
        return commands;
    }

    std::vector<springai::Command*> currentCommands = unit->GetCurrentCommands();
    for (springai::Command* command : currentCommands) {
        commands.push_back(detail::SerializeCommand(command));
        delete command;
    }

    return commands;
}

json SerializeSupportedCommands(springai::Unit* unit) {
    json commands = json::array();
    if (!unit) {
        return commands;
    }

    std::vector<springai::CommandDescription*> supportedCommands = unit->GetSupportedCommands();
    for (springai::CommandDescription* command : supportedCommands) {
        commands.push_back(detail::SerializeCommandDescription(command));
        delete command;
    }

    return commands;
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

    if (type == "game_rules_param") {
        if (!game) {
            throw std::runtime_error("Game interface is not available.");
        }

        const std::string key = ReadStringValue(query, "key");
        const float defaultFloat = ReadOptionalFloatValue(query, "defaultFloat", 0.0f);
        const std::string defaultString = ReadOptionalStringValue(query, "defaultString", "");
        json result = SerializeRulesParamValue(
            game->GetRulesParamFloat(key.c_str(), defaultFloat),
            game->GetRulesParamString(key.c_str(), defaultString.c_str())
        );
        result["key"] = key;
        return result;
    }

    if (type == "game_rules_params") {
        if (!game) {
            throw std::runtime_error("Game interface is not available.");
        }

        const std::vector<std::string> keys = ReadStringListValue(query, "keys");
        const float defaultFloat = ReadOptionalFloatValue(query, "defaultFloat", 0.0f);
        const std::string defaultString = ReadOptionalStringValue(query, "defaultString", "");
        return json({
            {"values", SerializeRulesParams(game.get(), keys, defaultFloat, defaultString)}
        });
    }

    if (type == "team_rules_param" || type == "team_rules_params") {
        const int defaultTeamId = game ? game->GetMyTeam() : (skirmishAI ? skirmishAI->GetTeamId() : -1);
        const int teamId = query.contains("teamId") ? ReadIntValue(query, "teamId") : defaultTeamId;
        if (teamId < 0) {
            throw std::runtime_error("Team interface is not available.");
        }

        std::unique_ptr<springai::Team> team(springai::WrappTeam::GetInstance(skirmishAIId, teamId));
        if (!team) {
            throw std::runtime_error("Unknown Team ID.");
        }

        const float defaultFloat = ReadOptionalFloatValue(query, "defaultFloat", 0.0f);
        const std::string defaultString = ReadOptionalStringValue(query, "defaultString", "");

        if (type == "team_rules_param") {
            const std::string key = ReadStringValue(query, "key");
            json result = SerializeRulesParamValue(
                team->GetRulesParamFloat(key.c_str(), defaultFloat),
                team->GetRulesParamString(key.c_str(), defaultString.c_str())
            );
            result["teamId"] = teamId;
            result["key"] = key;
            return result;
        }

        const std::vector<std::string> keys = ReadStringListValue(query, "keys");
        return json({
            {"teamId", teamId},
            {"values", SerializeRulesParams(team.get(), keys, defaultFloat, defaultString)}
        });
    }

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

    if (type == "unit_current_commands") {
        springai::Unit* unit = springai::WrappUnit::GetInstance(skirmishAIId, ReadIntValue(query, "unitId"));
        if (!unit) {
            throw std::runtime_error("Unknown Unit ID.");
        }

        json commands = SerializeCurrentCommands(unit);
        return json({
            {"unitId", unit->GetUnitId()},
            {"count", commands.size()},
            {"hasCommands", !commands.empty()},
            {"commands", std::move(commands)}
        });
    }

    if (type == "unit_supported_commands") {
        springai::Unit* unit = springai::WrappUnit::GetInstance(skirmishAIId, ReadIntValue(query, "unitId"));
        if (!unit) {
            throw std::runtime_error("Unknown Unit ID.");
        }

        json commands = SerializeSupportedCommands(unit);
        return json({
            {"unitId", unit->GetUnitId()},
            {"count", commands.size()},
            {"commands", std::move(commands)}
        });
    }

    if (type == "unit_queue_state") {
        springai::Unit* unit = springai::WrappUnit::GetInstance(skirmishAIId, ReadIntValue(query, "unitId"));
        if (!unit) {
            throw std::runtime_error("Unknown Unit ID.");
        }

        json commands = SerializeCurrentCommands(unit);
        return json({
            {"unitId", unit->GetUnitId()},
            {"currentCommandCount", commands.size()},
            {"hasCommands", !commands.empty()},
            {"isIdle", commands.empty()},
            {"stockpile", unit->GetStockpile()},
            {"stockpileQueued", unit->GetStockpileQueued()},
            {"commands", std::move(commands)}
        });
    }

    if (type == "unit_rules_param" || type == "unit_rules_params") {
        springai::Unit* unit = springai::WrappUnit::GetInstance(skirmishAIId, ReadIntValue(query, "unitId"));
        if (!unit) {
            throw std::runtime_error("Unknown Unit ID.");
        }

        const float defaultFloat = ReadOptionalFloatValue(query, "defaultFloat", 0.0f);
        const std::string defaultString = ReadOptionalStringValue(query, "defaultString", "");

        if (type == "unit_rules_param") {
            const std::string key = ReadStringValue(query, "key");
            json result = SerializeRulesParamValue(
                unit->GetRulesParamFloat(key.c_str(), defaultFloat),
                unit->GetRulesParamString(key.c_str(), defaultString.c_str())
            );
            result["unitId"] = unit->GetUnitId();
            result["key"] = key;
            return result;
        }

        const std::vector<std::string> keys = ReadStringListValue(query, "keys");
        return json({
            {"unitId", unit->GetUnitId()},
            {"values", SerializeRulesParams(unit, keys, defaultFloat, defaultString)}
        });
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
            json resourceSpots = detail::GetResourceSpotsData(game.get(), map.get(), resource);
            result["extractorRadius"] = map->GetExtractorRadius(resource);
            result["maxResource"] = map->GetMaxResource(resource);
            result["averageSpotIncome"] = resourceSpots.value("averageIncome", map->GetResourceMapSpotsAverageIncome(resource));
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

        return detail::GetResourceSpotsData(game.get(), map.get(), resource);
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
        json resourceSpots = detail::GetResourceSpotsData(game.get(), map.get(), resource);
        json nearestSpot = FindNearestSpot(resourceSpots.value("spots", json::array()), origin);
        if (nearestSpot.is_null() || nearestSpot.empty()) {
            throw std::runtime_error("No resource spots available.");
        }

        return json({
            {"resource", detail::SafeCString(resource->GetName())},
            {"from", json::array({origin.x, origin.y, origin.z})},
            {"pos", nearestSpot["pos"]},
            {"income", nearestSpot.value("income", 0.0f)}
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

    if (type == "position_in_los") {
        if (!map) {
            throw std::runtime_error("Map interface is not available.");
        }

        const springai::AIFloat3 pos = ReadPositionValue(query);
        return json({
            {"pos", json::array({pos.x, pos.y, pos.z})},
            {"inLos", IsPositionInLos(pos)}
        });
    }

    if (type == "water_damage") {
        if (!map) {
            throw std::runtime_error("Map interface is not available.");
        }

        const float waterDamage = map->GetWaterDamage();
        return json({
            {"waterDamage", waterDamage},
            {"waterIsHarmful", waterDamage > 0.0f}
        });
    }

    if (type == "height_map") {
        if (!map) {
            throw std::runtime_error("Map interface is not available.");
        }

        std::vector<float> heights = map->GetHeightMap();
        return json({
            {"width", map->GetWidth()},
            {"height", map->GetHeight()},
            {"data_b64", detail::Base64Encode(
                reinterpret_cast<const unsigned char*>(heights.data()),
                heights.size() * sizeof(float)
            )}
        });
    }

    if (type == "slope_map") {
        if (!map) {
            throw std::runtime_error("Map interface is not available.");
        }

        std::vector<float> slopes = map->GetSlopeMap();
        return json({
            {"width", map->GetWidth() / 2},
            {"height", map->GetHeight() / 2},
            {"data_b64", detail::Base64Encode(
                reinterpret_cast<const unsigned char*>(slopes.data()),
                slopes.size() * sizeof(float)
            )}
        });
    }

    if (type == "ai_option_by_key") {
        if (!skirmishAI) {
            throw std::runtime_error("SkirmishAI interface is not available.");
        }

        const std::string key = ReadStringValue(query, "key");
        std::unique_ptr<springai::OptionValues> options(skirmishAI->GetOptionValues());
        if (!options) {
            throw std::runtime_error("AI options are not available.");
        }

        return json({
            {"key", key},
            {"value", detail::SafeCString(options->GetValueByKey(key.c_str()))}
        });
    }

    if (type == "ai_options") {
        if (!skirmishAI) {
            throw std::runtime_error("SkirmishAI interface is not available.");
        }

        std::unique_ptr<springai::OptionValues> options(skirmishAI->GetOptionValues());
        if (!options) {
            throw std::runtime_error("AI options are not available.");
        }

        if (query.contains("keys")) {
            const std::vector<std::string> keys = ReadStringListValue(query, "keys");
            return SerializeOptionValues(options.get(), &keys);
        }

        return SerializeOptionValues(options.get());
    }

    if (type == "ai_info_by_key") {
        if (!skirmishAI) {
            throw std::runtime_error("SkirmishAI interface is not available.");
        }

        const std::string key = ReadStringValue(query, "key");
        std::unique_ptr<springai::Info> info(skirmishAI->GetInfo());
        if (!info) {
            throw std::runtime_error("AI info is not available.");
        }

        return json({
            {"key", key},
            {"value", detail::SafeCString(info->GetValueByKey(key.c_str()))}
        });
    }

    if (type == "ai_info") {
        if (!skirmishAI) {
            throw std::runtime_error("SkirmishAI interface is not available.");
        }

        std::unique_ptr<springai::Info> info(skirmishAI->GetInfo());
        if (!info) {
            throw std::runtime_error("AI info is not available.");
        }

        if (query.contains("keys")) {
            const std::vector<std::string> keys = ReadStringListValue(query, "keys");
            return SerializeInfoValues(info.get(), &keys);
        }

        return SerializeInfoValues(info.get());
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
