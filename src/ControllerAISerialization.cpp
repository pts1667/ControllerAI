#include "ControllerAISerialization.h"

#include "ControllerAICommon.h"

#include "ExternalAI/Interface/AISEvents.h"

#include "Command.h"
#include "CommandDescription.h"
#include "OOAICallback.h"
#include "Feature.h"
#include "FeatureDef.h"
#include "FlankingBonus.h"
#include "MoveData.h"
#include "Resource.h"
#include "Unit.h"
#include "UnitDef.h"
#include "WeaponDef.h"
#include "WeaponMount.h"

#include <utility>
#include <vector>

namespace controllerai {
namespace detail {

json SerializeResource(springai::Resource* resource) {
    if (!resource) {
        return json();
    }

    return json({
        {"id", resource->GetResourceId()},
        {"name", SafeCString(resource->GetName())},
        {"optimum", resource->GetOptimum()}
    });
}

json SerializeCommand(springai::Command* command) {
    if (!command) {
        return json();
    }

    return json({
        {"commandId", command->GetCommandId()},
        {"type", command->GetType()},
        {"id", command->GetId()},
        {"options", command->GetOptions()},
        {"tag", command->GetTag()},
        {"timeout", command->GetTimeOut()},
        {"params", command->GetParams()}
    });
}

json SerializeCommandDescription(springai::CommandDescription* commandDescription) {
    if (!commandDescription) {
        return json();
    }

    json params = json::array();
    for (const char* value : commandDescription->GetParams()) {
        params.push_back(SafeCString(value));
    }

    return json({
        {"supportedCommandId", commandDescription->GetSupportedCommandId()},
        {"id", commandDescription->GetId()},
        {"name", SafeCString(commandDescription->GetName())},
        {"tooltip", SafeCString(commandDescription->GetToolTip())},
        {"showUnique", commandDescription->IsShowUnique()},
        {"disabled", commandDescription->IsDisabled()},
        {"params", std::move(params)}
    });
}

json SerializeWeaponDefRef(springai::WeaponDef* weaponDef) {
    if (!weaponDef) {
        return json();
    }

    return json({
        {"id", weaponDef->GetWeaponDefId()},
        {"name", SafeCString(weaponDef->GetName())},
        {"type", SafeCString(weaponDef->GetType())}
    });
}

json SerializeWeaponMount(springai::WeaponMount* weaponMount) {
    if (!weaponMount) {
        return json();
    }

    const springai::AIFloat3 mainDir = weaponMount->GetMainDir();
    return json({
        {"id", weaponMount->GetWeaponMountId()},
        {"name", SafeCString(weaponMount->GetName())},
        {"weaponDef", SerializeWeaponDefRef(weaponMount->GetWeaponDef())},
        {"slavedTo", weaponMount->GetSlavedTo()},
        {"mainDir", json::array({mainDir.x, mainDir.y, mainDir.z})},
        {"maxAngleDif", weaponMount->GetMaxAngleDif()},
        {"badTargetCategory", weaponMount->GetBadTargetCategory()},
        {"onlyTargetCategory", weaponMount->GetOnlyTargetCategory()}
    });
}

json SerializeMoveData(springai::MoveData* moveData) {
    if (!moveData) {
        return json();
    }

    return json({
        {"unitDefId", moveData->GetUnitDefId()},
        {"name", SafeCString(moveData->GetName())},
        {"xSize", moveData->GetXSize()},
        {"zSize", moveData->GetZSize()},
        {"depth", moveData->GetDepth()},
        {"maxSlope", moveData->GetMaxSlope()},
        {"slopeMod", moveData->GetSlopeMod()},
        {"pathType", moveData->GetPathType()},
        {"crushStrength", moveData->GetCrushStrength()},
        {"speedModClass", moveData->GetSpeedModClass()},
        {"terrainClass", moveData->GetTerrainClass()},
        {"followGround", moveData->GetFollowGround()},
        {"isSubMarine", moveData->IsSubMarine()}
    });
}

json SerializeFlankingBonus(springai::FlankingBonus* flankingBonus) {
    if (!flankingBonus) {
        return json();
    }

    const springai::AIFloat3 dir = flankingBonus->GetDir();
    return json({
        {"unitDefId", flankingBonus->GetUnitDefId()},
        {"mode", flankingBonus->GetMode()},
        {"dir", json::array({dir.x, dir.y, dir.z})},
        {"max", flankingBonus->GetMax()},
        {"min", flankingBonus->GetMin()},
        {"mobilityAdd", flankingBonus->GetMobilityAdd()}
    });
}

json SerializeUnitDef(springai::UnitDef* unitDef, springai::OOAICallback* callback) {
    if (!unitDef) {
        return json();
    }

    json resources = json::object();
    if (callback) {
        std::vector<springai::Resource*> allResources = callback->GetResources();
        for (springai::Resource* resource : allResources) {
            if (!resource) {
                continue;
            }

            const std::string resourceName = SafeCString(resource->GetName());
            resources[resourceName] = {
                {"id", resource->GetResourceId()},
                {"optimum", resource->GetOptimum()},
                {"upkeep", unitDef->GetUpkeep(resource)},
                {"resourceMake", unitDef->GetResourceMake(resource)},
                {"makesResource", unitDef->GetMakesResource(resource)},
                {"cost", unitDef->GetCost(resource)},
                {"extractsResource", unitDef->GetExtractsResource(resource)},
                {"resourceExtractorRange", unitDef->GetResourceExtractorRange(resource)},
                {"windResourceGenerator", unitDef->GetWindResourceGenerator(resource)},
                {"tidalResourceGenerator", unitDef->GetTidalResourceGenerator(resource)},
                {"storage", unitDef->GetStorage(resource)}
            };
        }
    }

    json yardMaps = json::object();
    for (int facing = 0; facing < 4; ++facing) {
        std::vector<short> yardMap = unitDef->GetYardMap(facing);
        if (!yardMap.empty()) {
            yardMaps[std::to_string(facing)] = yardMap;
        }
    }

    json buildOptions = json::array();
    std::vector<springai::UnitDef*> buildable = unitDef->GetBuildOptions();
    for (springai::UnitDef* option : buildable) {
        if (!option) {
            continue;
        }

        buildOptions.push_back({
            {"id", option->GetUnitDefId()},
            {"name", SafeCString(option->GetName())},
            {"humanName", SafeCString(option->GetHumanName())}
        });
    }

    json weaponMounts = json::array();
    std::vector<springai::WeaponMount*> mounts = unitDef->GetWeaponMounts();
    for (springai::WeaponMount* mount : mounts) {
        weaponMounts.push_back(SerializeWeaponMount(mount));
    }

    const springai::AIFloat3 flareDropVector = unitDef->GetFlareDropVector();

    json serialized = {
        {"id", unitDef->GetUnitDefId()},
        {"name", SafeCString(unitDef->GetName())},
        {"humanName", SafeCString(unitDef->GetHumanName())},
        {"height", unitDef->GetHeight()},
        {"radius", unitDef->GetRadius()},
        {"resources", resources},
        {"buildTime", unitDef->GetBuildTime()},
        {"autoHeal", unitDef->GetAutoHeal()},
        {"idleAutoHeal", unitDef->GetIdleAutoHeal()},
        {"idleTime", unitDef->GetIdleTime()},
        {"power", unitDef->GetPower()},
        {"health", unitDef->GetHealth()},
        {"category", unitDef->GetCategory()},
        {"categoryString", SafeCString(unitDef->GetCategoryString())},
        {"speed", unitDef->GetSpeed()},
        {"turnRate", unitDef->GetTurnRate()},
        {"isTurnInPlace", unitDef->IsTurnInPlace()},
        {"turnInPlaceDistance", unitDef->GetTurnInPlaceDistance()},
        {"turnInPlaceSpeedLimit", unitDef->GetTurnInPlaceSpeedLimit()},
        {"isUpright", unitDef->IsUpright()},
        {"isCollide", unitDef->IsCollide()},
        {"losRadius", unitDef->GetLosRadius()},
        {"airLosRadius", unitDef->GetAirLosRadius()},
        {"losHeight", unitDef->GetLosHeight()},
        {"radarRadius", unitDef->GetRadarRadius()},
        {"sonarRadius", unitDef->GetSonarRadius()},
        {"jammerRadius", unitDef->GetJammerRadius()},
        {"sonarJamRadius", unitDef->GetSonarJamRadius()},
        {"seismicRadius", unitDef->GetSeismicRadius()},
        {"seismicSignature", unitDef->GetSeismicSignature()},
        {"isStealth", unitDef->IsStealth()},
        {"isSonarStealth", unitDef->IsSonarStealth()},
        {"isBuildRange3D", unitDef->IsBuildRange3D()},
        {"buildDistance", unitDef->GetBuildDistance()},
        {"buildSpeed", unitDef->GetBuildSpeed()},
        {"reclaimSpeed", unitDef->GetReclaimSpeed()},
        {"repairSpeed", unitDef->GetRepairSpeed()},
        {"maxRepairSpeed", unitDef->GetMaxRepairSpeed()},
        {"resurrectSpeed", unitDef->GetResurrectSpeed()},
        {"captureSpeed", unitDef->GetCaptureSpeed()},
        {"terraformSpeed", unitDef->GetTerraformSpeed()},
        {"upDirSmoothing", unitDef->GetUpDirSmoothing()},
        {"mass", unitDef->GetMass()},
        {"isPushResistant", unitDef->IsPushResistant()},
        {"isStrafeToAttack", unitDef->IsStrafeToAttack()},
        {"minCollisionSpeed", unitDef->GetMinCollisionSpeed()},
        {"slideTolerance", unitDef->GetSlideTolerance()},
        {"maxHeightDif", unitDef->GetMaxHeightDif()},
        {"minWaterDepth", unitDef->GetMinWaterDepth()},
        {"waterline", unitDef->GetWaterline()},
        {"maxWaterDepth", unitDef->GetMaxWaterDepth()},
        {"armoredMultiple", unitDef->GetArmoredMultiple()},
        {"armorType", unitDef->GetArmorType()},
        {"maxWeaponRange", unitDef->GetMaxWeaponRange()},
        {"tooltip", SafeCString(unitDef->GetTooltip())},
        {"wreckName", SafeCString(unitDef->GetWreckName())},
        {"deathExplosion", SerializeWeaponDefRef(unitDef->GetDeathExplosion())},
        {"selfDExplosion", SerializeWeaponDefRef(unitDef->GetSelfDExplosion())},
        {"isAbleToSelfD", unitDef->IsAbleToSelfD()},
        {"selfDCountdown", unitDef->GetSelfDCountdown()},
        {"isAbleToSubmerge", unitDef->IsAbleToSubmerge()},
        {"isAbleToFly", unitDef->IsAbleToFly()},
        {"isAbleToMove", unitDef->IsAbleToMove()},
        {"isAbleToHover", unitDef->IsAbleToHover()},
        {"isFloater", unitDef->IsFloater()},
        {"isBuilder", unitDef->IsBuilder()},
        {"isActivateWhenBuilt", unitDef->IsActivateWhenBuilt()},
        {"isOnOffable", unitDef->IsOnOffable()},
        {"isFullHealthFactory", unitDef->IsFullHealthFactory()},
        {"isFactoryHeadingTakeoff", unitDef->IsFactoryHeadingTakeoff()},
        {"isReclaimable", unitDef->IsReclaimable()},
        {"isCapturable", unitDef->IsCapturable()},
        {"isAbleToRestore", unitDef->IsAbleToRestore()},
        {"isAbleToRepair", unitDef->IsAbleToRepair()},
        {"isAbleToSelfRepair", unitDef->IsAbleToSelfRepair()},
        {"isAbleToReclaim", unitDef->IsAbleToReclaim()},
        {"isAbleToAttack", unitDef->IsAbleToAttack()},
        {"isAbleToPatrol", unitDef->IsAbleToPatrol()},
        {"isAbleToFight", unitDef->IsAbleToFight()},
        {"isAbleToGuard", unitDef->IsAbleToGuard()},
        {"isAbleToAssist", unitDef->IsAbleToAssist()},
        {"isAssistable", unitDef->IsAssistable()},
        {"isAbleToRepeat", unitDef->IsAbleToRepeat()},
        {"isAbleToFireControl", unitDef->IsAbleToFireControl()},
        {"fireState", unitDef->GetFireState()},
        {"moveState", unitDef->GetMoveState()},
        {"wingDrag", unitDef->GetWingDrag()},
        {"wingAngle", unitDef->GetWingAngle()},
        {"frontToSpeed", unitDef->GetFrontToSpeed()},
        {"speedToFront", unitDef->GetSpeedToFront()},
        {"myGravity", unitDef->GetMyGravity()},
        {"maxBank", unitDef->GetMaxBank()},
        {"maxPitch", unitDef->GetMaxPitch()},
        {"turnRadius", unitDef->GetTurnRadius()},
        {"wantedHeight", unitDef->GetWantedHeight()},
        {"verticalSpeed", unitDef->GetVerticalSpeed()},
        {"isHoverAttack", unitDef->IsHoverAttack()},
        {"isAirStrafe", unitDef->IsAirStrafe()},
        {"dlHoverFactor", unitDef->GetDlHoverFactor()},
        {"maxAcceleration", unitDef->GetMaxAcceleration()},
        {"maxDeceleration", unitDef->GetMaxDeceleration()},
        {"maxAileron", unitDef->GetMaxAileron()},
        {"maxElevator", unitDef->GetMaxElevator()},
        {"maxRudder", unitDef->GetMaxRudder()},
        {"yardMaps", yardMaps},
        {"xSize", unitDef->GetXSize()},
        {"zSize", unitDef->GetZSize()},
        {"loadingRadius", unitDef->GetLoadingRadius()},
        {"unloadSpread", unitDef->GetUnloadSpread()},
        {"transportCapacity", unitDef->GetTransportCapacity()},
        {"transportSize", unitDef->GetTransportSize()},
        {"minTransportSize", unitDef->GetMinTransportSize()},
        {"isAirBase", unitDef->IsAirBase()},
        {"isFirePlatform", unitDef->IsFirePlatform()},
        {"transportMass", unitDef->GetTransportMass()},
        {"minTransportMass", unitDef->GetMinTransportMass()},
        {"isHoldSteady", unitDef->IsHoldSteady()},
        {"isReleaseHeld", unitDef->IsReleaseHeld()},
        {"isNotTransportable", unitDef->IsNotTransportable()},
        {"isTransportByEnemy", unitDef->IsTransportByEnemy()},
        {"transportUnloadMethod", unitDef->GetTransportUnloadMethod()},
        {"fallSpeed", unitDef->GetFallSpeed()},
        {"unitFallSpeed", unitDef->GetUnitFallSpeed()},
        {"isAbleToCloak", unitDef->IsAbleToCloak()},
        {"isStartCloaked", unitDef->IsStartCloaked()},
        {"cloakCost", unitDef->GetCloakCost()},
        {"cloakCostMoving", unitDef->GetCloakCostMoving()},
        {"decloakDistance", unitDef->GetDecloakDistance()},
        {"isDecloakSpherical", unitDef->IsDecloakSpherical()},
        {"isDecloakOnFire", unitDef->IsDecloakOnFire()},
        {"isAbleToKamikaze", unitDef->IsAbleToKamikaze()},
        {"kamikazeDist", unitDef->GetKamikazeDist()},
        {"isTargetingFacility", unitDef->IsTargetingFacility()},
        {"canManualFire", unitDef->CanManualFire()},
        {"isNeedGeo", unitDef->IsNeedGeo()},
        {"isFeature", unitDef->IsFeature()},
        {"isHideDamage", unitDef->IsHideDamage()},
        {"isShowPlayerName", unitDef->IsShowPlayerName()},
        {"isAbleToResurrect", unitDef->IsAbleToResurrect()},
        {"isAbleToCapture", unitDef->IsAbleToCapture()},
        {"highTrajectoryType", unitDef->GetHighTrajectoryType()},
        {"noChaseCategory", unitDef->GetNoChaseCategory()},
        {"isAbleToDropFlare", unitDef->IsAbleToDropFlare()},
        {"flareReloadTime", unitDef->GetFlareReloadTime()},
        {"flareEfficiency", unitDef->GetFlareEfficiency()},
        {"flareDelay", unitDef->GetFlareDelay()},
        {"flareDropVector", json::array({flareDropVector.x, flareDropVector.y, flareDropVector.z})},
        {"flareTime", unitDef->GetFlareTime()},
        {"flareSalvoSize", unitDef->GetFlareSalvoSize()},
        {"flareSalvoDelay", unitDef->GetFlareSalvoDelay()},
        {"isAbleToLoopbackAttack", unitDef->IsAbleToLoopbackAttack()},
        {"isLevelGround", unitDef->IsLevelGround()},
        {"maxThisUnit", unitDef->GetMaxThisUnit()},
        {"decoyDef", unitDef->GetDecoyDef() ? json({{"id", unitDef->GetDecoyDef()->GetUnitDefId()}, {"name", SafeCString(unitDef->GetDecoyDef()->GetName())}}) : json()},
        {"isDontLand", unitDef->IsDontLand()},
        {"shieldDef", SerializeWeaponDefRef(unitDef->GetShieldDef())},
        {"stockpileDef", SerializeWeaponDefRef(unitDef->GetStockpileDef())},
        {"buildOptions", buildOptions},
        {"customParams", unitDef->GetCustomParams()},
        {"weaponMounts", weaponMounts},
        {"moveData", SerializeMoveData(unitDef->GetMoveData())},
        {"flankingBonus", SerializeFlankingBonus(unitDef->GetFlankingBonus())}
    };

    return serialized;
}

json SerializeFeatureDef(springai::FeatureDef* featureDef, springai::OOAICallback* callback) {
    if (!featureDef) {
        return json();
    }

    json containedResources = json::object();
    if (callback) {
        std::vector<springai::Resource*> allResources = callback->GetResources();
        for (springai::Resource* resource : allResources) {
            if (!resource) {
                continue;
            }

            containedResources[SafeCString(resource->GetName())] = featureDef->GetContainedResource(resource);
        }
    }

    return json({
        {"id", featureDef->GetFeatureDefId()},
        {"name", SafeCString(featureDef->GetName())},
        {"description", SafeCString(featureDef->GetDescription())},
        {"containedResources", containedResources},
        {"maxHealth", featureDef->GetMaxHealth()},
        {"reclaimTime", featureDef->GetReclaimTime()},
        {"mass", featureDef->GetMass()},
        {"isUpright", featureDef->IsUpright()},
        {"drawType", featureDef->GetDrawType()},
        {"modelName", SafeCString(featureDef->GetModelName())},
        {"resurrectable", featureDef->GetResurrectable()},
        {"smokeTime", featureDef->GetSmokeTime()},
        {"isDestructable", featureDef->IsDestructable()},
        {"isReclaimable", featureDef->IsReclaimable()},
        {"isAutoreclaimable", featureDef->IsAutoreclaimable()},
        {"isBlocking", featureDef->IsBlocking()},
        {"isBurnable", featureDef->IsBurnable()},
        {"isFloating", featureDef->IsFloating()},
        {"isNoSelect", featureDef->IsNoSelect()},
        {"isGeoThermal", featureDef->IsGeoThermal()},
        {"xSize", featureDef->GetXSize()},
        {"zSize", featureDef->GetZSize()},
        {"customParams", featureDef->GetCustomParams()}
    });
}

json SerializeFeature(springai::Feature* feature, springai::OOAICallback* callback) {
    if (!feature) {
        return json();
    }

    springai::AIFloat3 pos = feature->GetPosition();
    springai::FeatureDef* def = feature->GetDef();
    springai::UnitDef* resurrectDef = feature->GetResurrectDef();

    json result = {
        {"id", feature->GetFeatureId()},
        {"pos", json::array({pos.x, pos.y, pos.z})},
        {"health", feature->GetHealth()},
        {"reclaimLeft", feature->GetReclaimLeft()},
        {"buildingFacing", feature->GetBuildingFacing()}
    };

    if (def) {
        result["defId"] = def->GetFeatureDefId();
        result["name"] = SafeCString(def->GetName());
        result["definition"] = SerializeFeatureDef(def, callback);
    }

    if (resurrectDef) {
        result["resurrectDef"] = {
            {"id", resurrectDef->GetUnitDefId()},
            {"name", SafeCString(resurrectDef->GetName())}
        };
    }

    return result;
}

std::tuple<int, json> ParseUnit(springai::Unit* unit) {
    if (!unit) {
        return std::make_tuple(-1, json());
    }

    json result;
    springai::UnitDef* def = unit->GetDef();
    if (def) {
        result["defId"] = def->GetUnitDefId();
        result["name"] = SafeCString(def->GetName());
    }

    const springai::AIFloat3 pos = unit->GetPos();
    const springai::AIFloat3 vel = unit->GetVel();

    result["allyTeam"] = unit->GetAllyTeam();
    result["pos"] = json::array({pos.x, pos.y, pos.z});
    result["vel"] = json::array({vel.x, vel.y, vel.z});
    result["health"] = unit->GetHealth();
    result["maxHealth"] = unit->GetMaxHealth();
    result["experience"] = unit->GetExperience();
    result["buildProgress"] = unit->GetBuildProgress();
    result["isBeingBuilt"] = unit->IsBeingBuilt();
    result["isCloaked"] = unit->IsCloaked();
    result["isParalyzed"] = unit->IsParalyzed();

    return std::make_tuple(unit->GetUnitId(), result);
}

std::tuple<int, json> ParseRadarBlip(springai::Unit* unit) {
    if (!unit) {
        return std::make_tuple(-1, json());
    }

    json blip;
    const springai::AIFloat3 pos = unit->GetPos();
    const springai::AIFloat3 vel = unit->GetVel();

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

json SerializeUnitDetails(springai::Unit* unit, springai::OOAICallback* callback) {
    if (!unit) {
        return json();
    }

    auto [unitId, base] = ParseUnit(unit);
    if (unitId == -1) {
        return json();
    }

    json resources = json::object();
    if (callback) {
        std::vector<springai::Resource*> allResources = callback->GetResources();
        for (springai::Resource* resource : allResources) {
            if (!resource) {
                continue;
            }

            resources[SafeCString(resource->GetName())] = {
                {"use", unit->GetResourceUse(resource)},
                {"make", unit->GetResourceMake(resource)}
            };
        }
    }

    base["id"] = unitId;
    base["teamId"] = unit->GetTeam();
    base["limit"] = unit->GetLimit();
    base["maxUnits"] = unit->GetMax();
    base["stockpile"] = unit->GetStockpile();
    base["stockpileQueued"] = unit->GetStockpileQueued();
    base["maxSpeed"] = unit->GetMaxSpeed();
    base["maxRange"] = unit->GetMaxRange();
    base["group"] = unit->GetGroup();
    base["paralyzeDamage"] = unit->GetParalyzeDamage();
    base["captureProgress"] = unit->GetCaptureProgress();
    base["speed"] = unit->GetSpeed();
    base["power"] = unit->GetPower();
    base["resources"] = resources;
    base["isActivated"] = unit->IsActivated();
    base["isNeutral"] = unit->IsNeutral();
    base["buildingFacing"] = unit->GetBuildingFacing();
    base["lastUserOrderFrame"] = unit->GetLastUserOrderFrame();
    base["definition"] = SerializeUnitDef(unit->GetDef(), callback);
    return base;
}

json EventToJson(int topic, const void* data) {
    json event;
    event["topic"] = topic;
    if (!data) {
        return event;
    }

    auto addDirection = [&event](const float* dirPosF3) {
        if (!dirPosF3) {
            return;
        }
        event["dirPos"] = json::array({dirPosF3[0], dirPosF3[1], dirPosF3[2]});
    };

    switch (topic) {
        case EVENT_INIT: {
            const auto* ev = static_cast<const SInitEvent*>(data);
            event["name"] = "Init";
            event["isSavegame"] = ev->savedGame;
            break;
        }
        case EVENT_UNIT_CREATED: {
            const auto* ev = static_cast<const SUnitCreatedEvent*>(data);
            event["unitId"] = ev->unit;
            event["builderId"] = ev->builder;
            break;
        }
        case EVENT_UNIT_FINISHED: {
            const auto* ev = static_cast<const SUnitFinishedEvent*>(data);
            event["unitId"] = ev->unit;
            break;
        }
        case EVENT_UNIT_IDLE: {
            const auto* ev = static_cast<const SUnitIdleEvent*>(data);
            event["unitId"] = ev->unit;
            break;
        }
        case EVENT_UNIT_DESTROYED: {
            const auto* ev = static_cast<const SUnitDestroyedEvent*>(data);
            event["unitId"] = ev->unit;
            event["attackerId"] = ev->attacker;
            break;
        }
        case EVENT_UNIT_DAMAGED: {
            const auto* ev = static_cast<const SUnitDamagedEvent*>(data);
            event["unitId"] = ev->unit;
            event["attackerId"] = ev->attacker;
            event["damage"] = ev->damage;
            event["weaponDefId"] = ev->weaponDefId;
            event["paralyzer"] = ev->paralyzer;
            addDirection(ev->dir_posF3);
            break;
        }
        case EVENT_ENEMY_ENTER_LOS: {
            const auto* ev = static_cast<const SEnemyEnterLOSEvent*>(data);
            event["enemyId"] = ev->enemy;
            break;
        }
        case EVENT_ENEMY_DAMAGED: {
            const auto* ev = static_cast<const SEnemyDamagedEvent*>(data);
            event["enemyId"] = ev->enemy;
            event["attackerId"] = ev->attacker;
            event["damage"] = ev->damage;
            event["weaponDefId"] = ev->weaponDefId;
            event["paralyzer"] = ev->paralyzer;
            addDirection(ev->dir_posF3);
            break;
        }
        case EVENT_LUA_MESSAGE: {
            const auto* ev = static_cast<const SLuaMessageEvent*>(data);
            event["data"] = SafeCString(ev->inData);
            break;
        }
        default:
            break;
    }

    return event;
}

} // namespace detail
} // namespace controllerai