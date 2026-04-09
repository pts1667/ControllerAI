#ifndef CONTROLLERAI_SERIALIZATION_H
#define CONTROLLERAI_SERIALIZATION_H

#include "nlohmann/json.hpp"

#include <tuple>

namespace springai {
class OOAICallback;
class Resource;
class WeaponDef;
class WeaponMount;
class MoveData;
class FlankingBonus;
class UnitDef;
class FeatureDef;
class Feature;
class Unit;
}

namespace controllerai {
namespace detail {

using json = nlohmann::json;

json SerializeResource(springai::Resource* resource);
json SerializeWeaponDefRef(springai::WeaponDef* weaponDef);
json SerializeWeaponMount(springai::WeaponMount* weaponMount);
json SerializeMoveData(springai::MoveData* moveData);
json SerializeFlankingBonus(springai::FlankingBonus* flankingBonus);
json SerializeUnitDef(springai::UnitDef* unitDef, springai::OOAICallback* callback);
json SerializeFeatureDef(springai::FeatureDef* featureDef, springai::OOAICallback* callback);
json SerializeFeature(springai::Feature* feature, springai::OOAICallback* callback);
json SerializeUnitDetails(springai::Unit* unit, springai::OOAICallback* callback);
std::tuple<int, json> ParseUnit(springai::Unit* unit);
std::tuple<int, json> ParseRadarBlip(springai::Unit* unit);
json EventToJson(int topic, const void* data);

} // namespace detail
} // namespace controllerai

#endif