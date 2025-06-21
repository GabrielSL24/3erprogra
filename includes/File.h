#pragma once
#include <string>
#include <ctime>
#include <filesystem>
#include <vector>
#include <utility>
#include <nlohmann/json.hpp>

struct File {
    std::string id;
    std::string filename;
    std::string fileType;
    size_t size;
    std::time_t uploadDate;
    std::vector<std::pair<int, bool>> blockMap;

    File(const std::string& filePath, const std::string& type = "") 
        : filename(std::filesystem::path(filePath).filename().string()),
          fileType(type.empty() ? std::filesystem::path(filePath).extension().string() : type),
          size(std::filesystem::file_size(filePath)),
          uploadDate(std::time(nullptr)) {
        id = generateFileHash(filePath);
    }

    // Constructor desde JSON
    File(const nlohmann::json& j) 
        : id(j["id"]),
          filename(j["filename"]),
          fileType(j["fileType"]),
          size(j["size"]),
          uploadDate(j["uploadDate"]) {
        
        // Cargar blockMap desde JSON (si existe)
        if (j.contains("blockMap")) {
            for (const auto& item : j["blockMap"]) {
                blockMap.emplace_back(item[0], item[1]);
            }
        }
    }

    // Convertir a JSON
    nlohmann::json toJson() const {
        nlohmann::json j = {
            {"id", id},
            {"filename", filename},
            {"fileType", fileType},
            {"size", size},
            {"uploadDate", uploadDate},
            {"blockMap", nlohmann::json::array()}  // Inicializar array
        };

        // Serializar blockMap
        for (const auto& block : blockMap) {
            j["blockMap"].push_back({block.first, block.second});
        }

        return j;
    }

    static std::string generateFileHash(const std::string& filePath);
};