#include "ControllerAICommon.h"

#include "Game.h"
#include "Map.h"

#include <algorithm>
#include <cctype>
#include <regex>

namespace controllerai {
namespace {

size_t FindMatchingBrace(const std::string& text, size_t openBracePos) {
    if (openBracePos == std::string::npos) {
        return std::string::npos;
    }

    int depth = 0;
    for (size_t index = openBracePos; index < text.size(); ++index) {
        if (text[index] == '{') {
            ++depth;
        } else if (text[index] == '}') {
            --depth;
            if (depth == 0) {
                return index;
            }
        }
    }

    return std::string::npos;
}

bool PointInPolygon(const json& polygon, float x, float z) {
    if (!polygon.is_array() || polygon.size() < 3) {
        return false;
    }

    bool inside = false;
    for (size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
        const json& current = polygon[i];
        const json& previous = polygon[j];
        if (!current.is_array() || current.size() < 2 || !previous.is_array() || previous.size() < 2) {
            continue;
        }

        const float xi = current[0].get<float>();
        const float zi = current[1].get<float>();
        const float xj = previous[0].get<float>();
        const float zj = previous[1].get<float>();
        const bool intersects = ((zi > z) != (zj > z)) &&
            (x < (xj - xi) * (z - zi) / ((zj - zi) + 1e-6f) + xi);

        if (intersects) {
            inside = !inside;
        }
    }

    return inside;
}

} // namespace

namespace detail {

std::string SafeCString(const char* value) {
    return value != nullptr ? std::string(value) : std::string();
}

std::string Base64Encode(const unsigned char* data, size_t len) {
    static const char* base64Chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    int bufferSize = 0;
    int paddingIndex = 0;
    unsigned char charArray3[3];
    unsigned char charArray4[4];

    while (len--) {
        charArray3[bufferSize++] = *(data++);
        if (bufferSize == 3) {
            charArray4[0] = (charArray3[0] & 0xfc) >> 2;
            charArray4[1] = ((charArray3[0] & 0x03) << 4) + ((charArray3[1] & 0xf0) >> 4);
            charArray4[2] = ((charArray3[1] & 0x0f) << 2) + ((charArray3[2] & 0xc0) >> 6);
            charArray4[3] = charArray3[2] & 0x3f;

            for (bufferSize = 0; bufferSize < 4; ++bufferSize) {
                result += base64Chars[charArray4[bufferSize]];
            }
            bufferSize = 0;
        }
    }

    if (bufferSize) {
        for (paddingIndex = bufferSize; paddingIndex < 3; ++paddingIndex) {
            charArray3[paddingIndex] = '\0';
        }

        charArray4[0] = (charArray3[0] & 0xfc) >> 2;
        charArray4[1] = ((charArray3[0] & 0x03) << 4) + ((charArray3[1] & 0xf0) >> 4);
        charArray4[2] = ((charArray3[1] & 0x0f) << 2) + ((charArray3[2] & 0xc0) >> 6);
        charArray4[3] = charArray3[2] & 0x3f;

        for (paddingIndex = 0; paddingIndex < bufferSize + 1; ++paddingIndex) {
            result += base64Chars[charArray4[paddingIndex]];
        }
        while ((bufferSize++ < 3)) {
            result += '=';
        }
    }

    return result;
}

bool TryParseZkSpawnBoxes(springai::Game* game, json& spawnBoxesCache) {
    if (!game) {
        return false;
    }

    const int boxCount = static_cast<int>(game->GetRulesParamFloat("startbox_max_n", -1.0f));
    if (boxCount <= 0) {
        return false;
    }

    for (int boxId = 0; boxId < boxCount; ++boxId) {
        const std::string boxIdStr = std::to_string(boxId);
        const int polygonCount = static_cast<int>(game->GetRulesParamFloat(("startbox_n_" + boxIdStr).c_str(), -1.0f));
        if (polygonCount <= 0) {
            continue;
        }

        json polygons = json::array();
        bool hasVertex = false;
        float left = 0.0f;
        float top = 0.0f;
        float right = 0.0f;
        float bottom = 0.0f;

        for (int polygonIndex = 1; polygonIndex <= polygonCount; ++polygonIndex) {
            const std::string polygonPrefix = boxIdStr + "_" + std::to_string(polygonIndex);
            const int vertexCount = static_cast<int>(game->GetRulesParamFloat(("startbox_polygon_" + polygonPrefix).c_str(), -1.0f));
            if (vertexCount <= 0) {
                continue;
            }

            json polygon = json::array();
            for (int vertexIndex = 1; vertexIndex <= vertexCount; ++vertexIndex) {
                const std::string vertexSuffix = polygonPrefix + "_" + std::to_string(vertexIndex);
                const float x = game->GetRulesParamFloat(("startbox_polygon_x_" + vertexSuffix).c_str(), 0.0f);
                const float z = game->GetRulesParamFloat(("startbox_polygon_z_" + vertexSuffix).c_str(), 0.0f);
                polygon.push_back(json::array({x, z}));

                if (!hasVertex) {
                    left = x;
                    right = x;
                    top = z;
                    bottom = z;
                    hasVertex = true;
                } else {
                    left = std::min(left, x);
                    right = std::max(right, x);
                    top = std::min(top, z);
                    bottom = std::max(bottom, z);
                }
            }

            if (!polygon.empty()) {
                polygons.push_back(std::move(polygon));
            }
        }

        if (hasVertex && !polygons.empty()) {
            spawnBoxesCache[boxIdStr] = {
                {"left", left},
                {"top", top},
                {"right", right},
                {"bottom", bottom},
                {"polygons", std::move(polygons)}
            };
        }
    }

    return !spawnBoxesCache.empty();
}

bool TryParseClassicSpawnBoxes(const std::string& script, springai::Map* map, json& spawnBoxesCache) {
    if (!map || script.empty()) {
        return false;
    }

    const float widthElmos = static_cast<float>(map->GetWidth() * 8);
    const float heightElmos = static_cast<float>(map->GetHeight() * 8);

    const std::regex allyPattern("\\[allyteam(\\d+)\\]", std::regex::ECMAScript | std::regex::icase);
    const std::regex rectPattern("startrect(left|right|top|bottom)\\s*=\\s*(-?\\d+(?:\\.\\d+)?)\\s*;", std::regex::ECMAScript | std::regex::icase);

    std::string::const_iterator searchStart = script.cbegin();
    std::smatch allyMatch;
    while (std::regex_search(searchStart, script.cend(), allyMatch, allyPattern)) {
        const int allyId = std::stoi(allyMatch[1].str());
        const size_t headerEnd = static_cast<size_t>(std::distance(script.cbegin(), allyMatch[0].second));
        const size_t bodyStart = script.find('{', headerEnd);
        if (bodyStart == std::string::npos) {
            searchStart = allyMatch[0].second;
            continue;
        }

        const size_t bodyEnd = FindMatchingBrace(script, bodyStart);
        if (bodyEnd == std::string::npos) {
            break;
        }

        bool hasLeft = false;
        bool hasTop = false;
        bool hasRight = false;
        bool hasBottom = false;
        float left = 0.0f;
        float top = 0.0f;
        float right = 0.0f;
        float bottom = 0.0f;

        std::string::const_iterator rectSearch = script.cbegin() + bodyStart + 1;
        const std::string::const_iterator rectEnd = script.cbegin() + bodyEnd;
        std::smatch rectMatch;
        while (std::regex_search(rectSearch, rectEnd, rectMatch, rectPattern)) {
            std::string edge = rectMatch[1].str();
            std::transform(edge.begin(), edge.end(), edge.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });

            const float value = std::stof(rectMatch[2].str());
            if (edge == "left") {
                left = value;
                hasLeft = true;
            } else if (edge == "top") {
                top = value;
                hasTop = true;
            } else if (edge == "right") {
                right = value;
                hasRight = true;
            } else if (edge == "bottom") {
                bottom = value;
                hasBottom = true;
            }

            rectSearch = rectMatch[0].second;
        }

        if (hasLeft && hasTop && hasRight && hasBottom) {
            spawnBoxesCache[std::to_string(allyId)] = {
                {"left", left * widthElmos},
                {"top", top * heightElmos},
                {"right", right * widthElmos},
                {"bottom", bottom * heightElmos}
            };
        }

        searchStart = script.cbegin() + bodyEnd + 1;
    }

    return !spawnBoxesCache.empty();
}

bool IsPointInSpawnBox(const json& spawnBox, float x, float z) {
    if (!spawnBox.is_object()) {
        return false;
    }

    if (spawnBox.contains("left") && spawnBox.contains("right") && spawnBox.contains("top") && spawnBox.contains("bottom")) {
        const float left = spawnBox["left"].get<float>();
        const float right = spawnBox["right"].get<float>();
        const float top = spawnBox["top"].get<float>();
        const float bottom = spawnBox["bottom"].get<float>();
        if (x < left || x > right || z < top || z > bottom) {
            return false;
        }
    }

    if (!spawnBox.contains("polygons") || !spawnBox["polygons"].is_array() || spawnBox["polygons"].empty()) {
        return true;
    }

    for (const json& polygon : spawnBox["polygons"]) {
        if (PointInPolygon(polygon, x, z)) {
            return true;
        }
    }

    return false;
}

} // namespace detail
} // namespace controllerai