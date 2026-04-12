#ifndef CONTROLLERAI_COMMON_H
#define CONTROLLERAI_COMMON_H

#include "nlohmann/json.hpp"

#include <cstddef>
#include <string>

namespace springai {
class Game;
class Map;
class Resource;
}

namespace controllerai {
namespace detail {

using json = nlohmann::json;

std::string SafeCString(const char* value);
std::string Base64Encode(const unsigned char* data, size_t len);
json GetResourceSpotsData(springai::Game* game, springai::Map* map, springai::Resource* resource);
bool TryParseZkSpawnBoxes(springai::Game* game, json& spawnBoxesCache);
bool TryParseClassicSpawnBoxes(const std::string& script, springai::Map* map, json& spawnBoxesCache);
bool IsPointInSpawnBox(const json& spawnBox, float x, float z);

} // namespace detail
} // namespace controllerai

#endif