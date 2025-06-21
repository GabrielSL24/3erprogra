#pragma once
#include <string>
#include <ctime>
#include <filesystem>
#include <nlohmann/json.hpp>

struct File {
    std::string id;
    std::string filename;
    std::string fileType;
    size_t size;
    std::time_t uploadDate;

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
          uploadDate(j["uploadDate"]) {}

    // Convertir a JSON
    nlohmann::json toJson() const {
        return {
            {"id", id},
            {"filename", filename},
            {"fileType", fileType},
            {"size", size},
            {"uploadDate", uploadDate}
        };
    }

    static std::string generateFileHash(const std::string& filePath);
};