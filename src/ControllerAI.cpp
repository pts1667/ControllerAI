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
#include "WeaponDef.h"
#include "WeaponMount.h"
#include "MoveData.h"
#include "FlankingBonus.h"
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
#include <stdexcept>
#include <regex>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace controllerai {

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
    blockNFrames(1)
{
    server = std::make_unique<CControllerAIServer>(bindAddress, port);

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
        gameInfoCache["teamId"] = game->GetMyTeam();
        gameInfoCache["allyTeamId"] = game->GetMyAllyTeam();

        script = SafeCString(game->GetSetupScript());
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

int CControllerAI::ReadIntValue(const json& query, const char* key) const {
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

float CControllerAI::ReadFloatValue(const json& query, const char* key) const {
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

bool CControllerAI::ReadBoolValue(const json& query, const char* key, bool defaultValue) const {
    if (!query.contains(key)) {
        return defaultValue;
    }

    const json& value = query.at(key);
    if (value.is_boolean()) {
        return value.get<bool>();
    }
    if (value.is_number_integer()) {
        return value.get<int>() != 0;
    }
    if (value.is_string()) {
        const std::string raw = value.get<std::string>();
        if (raw == "1" || raw == "true" || raw == "TRUE" || raw == "True") {
            return true;
        }
        if (raw == "0" || raw == "false" || raw == "FALSE" || raw == "False") {
            return false;
        }
    }

    throw std::runtime_error(std::string("Query parameter '") + key + "' must be a boolean.");
}

std::string CControllerAI::ReadStringValue(const json& query, const char* key) const {
    if (!query.contains(key)) {
        throw std::runtime_error(std::string("Missing query parameter: ") + key);
    }

    const json& value = query.at(key);
    if (!value.is_string()) {
        throw std::runtime_error(std::string("Query parameter '") + key + "' must be a string.");
    }

    return value.get<std::string>();
}

springai::AIFloat3 CControllerAI::ReadPositionValue(const json& query) const {
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

springai::UnitDef* CControllerAI::FindUnitDefById(int unitDefId) {
    if (skirmishAIId == -1 || unitDefId < 0) {
        return nullptr;
    }

    return springai::WrappUnitDef::GetInstance(skirmishAIId, unitDefId);
}

springai::FeatureDef* CControllerAI::FindFeatureDefById(int featureDefId) {
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

springai::FeatureDef* CControllerAI::FindFeatureDefByName(const std::string& name) {
    if (!callback) {
        return nullptr;
    }

    std::vector<springai::FeatureDef*> defs = callback->GetFeatureDefs();
    for (springai::FeatureDef* def : defs) {
        if (def && SafeCString(def->GetName()) == name) {
            return def;
        }
    }

    return nullptr;
}

json CControllerAI::SerializeResource(springai::Resource* resource) {
    if (!resource) {
        return json();
    }

    return json({
        {"id", resource->GetResourceId()},
        {"name", SafeCString(resource->GetName())},
        {"optimum", resource->GetOptimum()}
    });
}

json CControllerAI::SerializeWeaponDefRef(springai::WeaponDef* weaponDef) {
    if (!weaponDef) {
        return json();
    }

    return json({
        {"id", weaponDef->GetWeaponDefId()},
        {"name", SafeCString(weaponDef->GetName())},
        {"type", SafeCString(weaponDef->GetType())}
    });
}

json CControllerAI::SerializeWeaponMount(springai::WeaponMount* weaponMount) {
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

json CControllerAI::SerializeMoveData(springai::MoveData* moveData) {
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

json CControllerAI::SerializeFlankingBonus(springai::FlankingBonus* flankingBonus) {
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

json CControllerAI::SerializeUnitDef(springai::UnitDef* unitDef) {
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

json CControllerAI::SerializeFeatureDef(springai::FeatureDef* featureDef) {
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

json CControllerAI::SerializeFeature(springai::Feature* feature) {
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
        result["definition"] = SerializeFeatureDef(def);
    }

    if (resurrectDef) {
        result["resurrectDef"] = {
            {"id", resurrectDef->GetUnitDefId()},
            {"name", SafeCString(resurrectDef->GetName())}
        };
    }

    return result;
}

json CControllerAI::SerializeUnitDetails(springai::Unit* unit) {
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
    base["definition"] = SerializeUnitDef(unit->GetDef());
    return base;
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
    startupBlocking = false;
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
        springai::UnitDef* unitDef = FindUnitDefById(ReadIntValue(query, "unitDefId"));
        if (!unitDef) {
            throw std::runtime_error("Unknown UnitDef ID.");
        }
        return SerializeUnitDef(unitDef);
    }

    if (type == "unit_def_by_name") {
        springai::UnitDef* unitDef = callback->GetUnitDefByName(ReadStringValue(query, "name").c_str());
        if (!unitDef) {
            throw std::runtime_error("Unknown UnitDef name.");
        }
        return SerializeUnitDef(unitDef);
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
        return SerializeUnitDef(unitDef);
    }

    if (type == "unit_by_id") {
        springai::Unit* unit = springai::WrappUnit::GetInstance(skirmishAIId, ReadIntValue(query, "unitId"));
        if (!unit) {
            throw std::runtime_error("Unknown Unit ID.");
        }
        return SerializeUnitDetails(unit);
    }

    if (type == "feature_by_id") {
        springai::Feature* feature = springai::WrappFeature::GetInstance(skirmishAIId, ReadIntValue(query, "featureId"));
        if (!feature) {
            throw std::runtime_error("Unknown Feature ID.");
        }
        return SerializeFeature(feature);
    }

    if (type == "feature_def_by_id") {
        springai::FeatureDef* featureDef = FindFeatureDefById(ReadIntValue(query, "featureDefId"));
        if (!featureDef) {
            throw std::runtime_error("Unknown FeatureDef ID.");
        }
        return SerializeFeatureDef(featureDef);
    }

    if (type == "feature_def_by_name") {
        springai::FeatureDef* featureDef = FindFeatureDefByName(ReadStringValue(query, "name"));
        if (!featureDef) {
            throw std::runtime_error("Unknown FeatureDef name.");
        }
        return SerializeFeatureDef(featureDef);
    }

    if (type == "resource_by_name") {
        springai::Resource* resource = callback->GetResourceByName(ReadStringValue(query, "name").c_str());
        if (!resource) {
            throw std::runtime_error("Unknown resource name.");
        }

        json result = SerializeResource(resource);
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
            {"resource", SafeCString(resource->GetName())},
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
            {"resource", SafeCString(resource->GetName())},
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

        springai::UnitDef* unitDef = FindUnitDefById(ReadIntValue(query, "unitDefId"));
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

        springai::UnitDef* unitDef = FindUnitDefById(ReadIntValue(query, "unitDefId"));
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
                        startupBlocking = false;
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
                if (game && game->GetCurrentFrame() < 0) {
                    startupBlocking = false;
                } else {
                    frameFinished = true;
                }
            }
        } catch (...) {}
    }
}

void CControllerAI::WaitForResume() {
    if (!server) return;

    while (running) {
        if (game && game->GetCurrentFrame() < 0 && startupBlocking) {
            if (!server->WaitForWork()) return;
            ProcessSettings();
            ProcessQueries();
            ProcessCommands();
            continue;
        }

        if (game && canChooseStartPos && !setupComplete) {
            if (!server->WaitForWork()) return;
            ProcessSettings();
            ProcessQueries();
            ProcessCommands();
            continue;
        }

        if (!synchronousMode || !game || game->GetCurrentFrame() < 0 || frameFinished) {
            return;
        }

        if (!server->WaitForWork()) return;
        ProcessSettings();
        ProcessQueries();
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

    if (topic != EVENT_RELEASE) {
        EnsureInterfacesInitialized();
    }

    if (topic != EVENT_RELEASE) {
        ProcessSettings();
        ProcessQueries();
    }

    if (topic == EVENT_RELEASE) {
        const int reason = data ? static_cast<const SReleaseEvent*>(data)->reason : 0;
        Release(reason);
        return 0;
    }

    if (topic == EVENT_INIT) {
        CacheStaticData();
        UpdateObservation();
        WaitForResume();
        return 0;
    }

    // Capture events
    json ev = EventToJson(topic, data);
    if (!ev.is_null() && ev.contains("topic")) {
        eventBuffer.push_back(ev);
    }

    if (topic == EVENT_UPDATE) {
        const int frame = data ? static_cast<const SUpdateEvent*>(data)->frame : -1;
        const bool shouldPublish = (frame < 0) || ShouldPublishObservationForFrame(frame);
        if (frame >= 0) {
            frameFinished = !shouldPublish;
        }

        ProcessCommands();
        if (shouldPublish) {
            UpdateObservation();
            WaitForResume();
        }
    }
    
    return 0;
}

} // namespace controllerai
