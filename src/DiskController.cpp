#include "DiskController.h"
#include <fstream>
#include <iostream>
#include <filesystem>
#include <chrono>
#include <future>    // Para std::async, std::future
#include <mutex>     // Para std::mutex, std::lock_guard
#include <atomic>    // Para std::atomic
#include <utility>   // Para std::move

DiskController::DiskController() {
    std::cout << "Controller iniciado. Nodos disponibles:\n";
    for (const auto& url : diskNodeUrls) {
        std::cout << "- " << url << "\n";
    }

    // Iniciar servidor en otro hilo
    serverThread = std::thread([this]() {
        this->startServer();
    });
    loadMetadata();
}

void DiskController::writeToDiskNode(const std::string& node_url,
                                   const std::string& block_id,
                                   const std::vector<char>& data,
                                   bool is_parity) {
    const int max_retries = 3;
    int retry_count = 0;
    bool success = false;

    while (retry_count < max_retries && !success) {
        httplib::Client client(node_url);
        client.set_connection_timeout(10);  // 10 segundos para conectar
        client.set_read_timeout(30);        // 30 segundos para leer
        client.set_write_timeout(30);       // 30 segundos para escribir

        try {
            nlohmann::json request;
            std::string full_block_id = (is_parity ? "parity_" : "block_") + block_id;
            request["block_id"] = full_block_id;
            request["is_parity"] = is_parity;
            request["data"] = base64_encode(data.data(), data.size());


            auto res = client.Post("/write_block", request.dump(), "application/json");

            if (res && res->status == 200) {
                        success = true;
            } else {
                std::cerr << "Intento " << retry_count + 1 << " fallido con nodo "
                          << node_url << ". Error: "
                          << (res ? res->status : -1) << std::endl;
                retry_count++;
                std::this_thread::sleep_for(std::chrono::seconds(1));
                    }
        } catch (const std::exception& e) {
            std::cerr << "Excepcion al escribir en nodo " << node_url
                      << ": " << e.what() << std::endl;
            retry_count++;
            std::this_thread::sleep_for(std::chrono::seconds(1));
                }
                }

    if (!success) {
        throw std::runtime_error("No se pudo escribir en el nodo despues de " +
            std::to_string(max_retries) + " intentos: " + node_url);
    }
}

void DiskController::distributeFile(const std::string& filePath) {
    try {
        //1.validaciones iniciales
        if (!std::filesystem::exists(filePath)) {
            throw std::runtime_error("Archivo no encontrado: " + filePath);
        }

        std::string filename = std::filesystem::path(filePath).filename().string();
        {
            std::lock_guard<std::mutex> lock(filesMutex);
            if (std::find(registeredFiles.begin(), registeredFiles.end(), filename) != registeredFiles.end()) {
                throw std::runtime_error("El archivo ya existe en el sistema: " + filename);
            }
        }

        //2.Lee y divide el archivo
        std::ifstream file(filePath, std::ios::binary | std::ios::ate);
        size_t fileSize = file.tellg();
        file.seekg(0, std::ios::beg);

        const size_t blockSize = 4096;
        size_t numBlocks = (fileSize + blockSize - 1) / blockSize;
        std::vector<std::vector<char>> blocks(numBlocks);

        for (size_t i = 0; i < numBlocks; ++i) {
            blocks[i].resize(blockSize);
            file.read(blocks[i].data(), blockSize);
            blocks[i].resize(file.gcount());
        }

        // 3. Distribucion RAID 5
        std::vector<std::future<void>> writeFutures;
        std::vector<std::pair<int, bool>> blockMapEntries;

        for (size_t stripeIdx = 0; stripeIdx < numBlocks; ++stripeIdx) {
            int parityPos = getParityPositionForStripe(stripeIdx);
            std::vector<char> parityBlock = blocks[stripeIdx];

            // Calcular paridad (XOR de todos los bloques de datos)
            for (int nodeIdx = 0; nodeIdx < 4; ++nodeIdx) {
                if (nodeIdx != parityPos) {
                    for (size_t i = 0; i < blocks[stripeIdx].size(); ++i) {
                        parityBlock[i] ^= blocks[stripeIdx][i];
                    }
                }
            }

            //Guarda metadatos y inicia escrituras
            for (int nodeIdx = 0; nodeIdx < 4; ++nodeIdx) {
                bool isParity = (nodeIdx == parityPos);
                blockMapEntries.emplace_back(nodeIdx, isParity);

                std::string blockName = filename + "_stripe_" + std::to_string(stripeIdx);
                std::vector<char>& data = isParity ? parityBlock : blocks[stripeIdx];

                writeFutures.push_back(std::async(std::launch::async, [=, &data]() {
                    writeToDiskNode(diskNodeUrls[nodeIdx], blockName, data, isParity);
                }));
            }
        }

        //Espera a que toda la escritura termina
        for (auto& future : writeFutures) {
            future.get();
        }

        //Registra archivo si salio bien
        {
            std::lock_guard<std::mutex> lockMap(fileMapMutex);
            std::lock_guard<std::mutex> lockFiles(filesMutex);
            fileBlockMap[filename] = blockMapEntries;
            registeredFiles.push_back(filename);
        }

        try {

        File newFile(filePath);
        newFile.blockMap = blockMapEntries;
        registerFile(newFile);
        } catch (const std::exception& e) {
            std::cerr << "Error en registerFile: " << e.what() << std::endl;
        }

        std::cout << "Archivo '" << filename << "' distribuido exitosamente.\n";

    } catch (const std::exception& e) {
        std::cerr << "Error en distributeFile: " << e.what() << std::endl;
        throw;
    }
}

std::vector<char> DiskController::retrieveFile(const std::string& filename) {
    std::vector<char> fullFile;
    std::lock_guard<std::mutex> lock(fileMapMutex);

    if (fileBlockMap.find(filename) == fileBlockMap.end()) {
        throw std::runtime_error("Archivo no encontrado en los metadatos");
    }

    size_t totalStripes = fileBlockMap[filename].size() / diskNodeUrls.size();

    for (size_t stripeIdx = 0; stripeIdx < totalStripes; ++stripeIdx) {
        std::vector<std::pair<std::vector<char>, bool>> blocksWithInfo; // Almacena bloque + si es paridad
        int missingNode = -1;

        // Recuperar bloques con su información de paridad
        for (int nodeIdx = 0; nodeIdx < diskNodeUrls.size(); ++nodeIdx) {
            bool isParity = fileBlockMap[filename][stripeIdx * diskNodeUrls.size() + nodeIdx].second;
            std::string blockType = isParity ? "parity_" : "block_";
            std::string blockName = blockType + filename + "_stripe_" + std::to_string(stripeIdx);

            try {
                httplib::Client client(diskNodeUrls[nodeIdx]);
                auto res = client.Get(("/read_block/" + blockName).c_str());

                if (res && res->status == 200) {
                    auto json = nlohmann::json::parse(res->body);
                    if (!json["data"].is_null() && !json["data"].empty()) {
                        blocksWithInfo.emplace_back(base64_decode(json["data"]), isParity);
                        continue;
                    }
                }
            }
            catch (...) {
                // Error al conectar con el nodo
            }

            // Si llegamos aquí, el bloque falta
            if (missingNode == -1) {
                missingNode = nodeIdx;
            }
            else {
                // Segundo bloque faltante - no podemos reconstruir
                throw std::runtime_error("Demasiados bloques faltantes para stripe " +
                    std::to_string(stripeIdx));
            }
        }

        std::vector<char> stripeData;
        if (missingNode != -1) {
            std::cout << "Debug - Bloques disponibles para stripe " << stripeIdx << ":\n";
            for (const auto& blockInfo : blocksWithInfo) {
                std::cout << "- Tipo: " << (blockInfo.second ? "PARIDAD" : "DATOS")
                    << ", Tamaño: " << blockInfo.first.size() << " bytes\n";
            }

            stripeData = reconstructMissingBlock(blocksWithInfo, missingNode, stripeIdx);
        }
        else {
            // Buscar cualquier bloque de datos (no paridad)
            for (const auto& blockInfo : blocksWithInfo) {
                if (!blockInfo.second) {
                    stripeData = blockInfo.first;
                    break;
                }
            }
        }

        if (stripeData.empty()) {
            throw std::runtime_error("No se pudo obtener datos para stripe " +
                std::to_string(stripeIdx));
        }

        fullFile.insert(fullFile.end(), stripeData.begin(), stripeData.end());
    }

    return fullFile;
}

std::vector<char> DiskController::reconstructMissingBlock(
    const std::vector<std::pair<std::vector<char>, bool>>& availableBlocksInfo,
    int missingNodeIndex,
    size_t stripeIndex)
{
    // 1. Filtrar bloques vacios o corruptos
    std::vector<std::pair<std::vector<char>, bool>> validBlocks;
    for (const auto& blockInfo : availableBlocksInfo) {
        if (!blockInfo.first.empty() && blockInfo.first.size() == 4096) {
            validBlocks.push_back(blockInfo);
        }
    }

    // 2. Verificar que tenemos suficientes bloques
    if (validBlocks.size() < diskNodeUrls.size() - 1) {
        throw std::runtime_error("Bloques válidos insuficientes para reconstrucción");
    }

    // 3. Obtener tamaño de bloque esperado (del primer bloque disponible)
    size_t blockSize = 0;

    // 4. Verificar consistencia de tamaños
    for (const auto& blockInfo : availableBlocksInfo) {
        if (blockInfo.first.size() >= 4096) { //Tamaño minimo esperado
            blockSize = blockInfo.first.size();
            break;
        }
    }

    if (blockSize == 0) {
        throw std::runtime_error("No se encontraron bloques validos para determinar el tamano");
    }

    // 5. Calcula posición de paridad
    size_t parityPos = stripeIndex % diskNodeUrls.size();

    // 6. Reconstrucción
    std::vector<char> reconstructed(blockSize, 0);

    if (static_cast<size_t>(missingNodeIndex) == parityPos) {
        // Caso 1: Falta el bloque de paridad - usar solo bloques de datos
        for (const auto& blockInfo : availableBlocksInfo) {
            if (!blockInfo.second) { // Si es bloque de datos
                for (size_t i = 0; i < blockSize; ++i) {
                    reconstructed[i] ^= blockInfo.first[i];
                }
            }
        }
    }
    else {
        // Caso 2: Falta un bloque de datos - usar paridad + otros datos
        bool parityFound = false;

        for (const auto& blockInfo : availableBlocksInfo) {
            if (!blockInfo.second) { // Bloques de datos
                for (size_t i = 0; i < blockSize; ++i) {
                    reconstructed[i] ^= blockInfo.first[i];
                }
            }
            else { // Bloque de paridad
                parityFound = true;
                for (size_t i = 0; i < blockSize; ++i) {
                    reconstructed[i] ^= blockInfo.first[i];
                }
            }
        }
        if (!parityFound) {
            throw std::runtime_error("Bloque de paridad no encontrado para reconstrucción");
        }
    }
    return reconstructed;
}

int DiskController::getParityPositionForStripe(size_t stripeIndex) const {
    return stripeIndex % 4;  // Rotacion entre 0-3 para 4 nodos
}

std::string DiskController::base64_encode(const char* data, size_t length) {
    if (!data || length == 0) {
        return "";
    }

    //Verifica que el tamaño no exceda el maximo permitido
    if (length > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("Datos demasiado grandes para codificación base64");
    }

    BIO *b64 = nullptr;
    BIO *mem = nullptr;
    std::string result;

    try {
        b64 = BIO_new(BIO_f_base64());
        if (!b64) throw std::runtime_error("Error creando BIO base64");

        mem = BIO_new(BIO_s_mem());
        if (!mem) throw std::runtime_error("Error creando BIO mem");

        BIO_push(b64, mem);
        BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);

        // Conversión segura con static_cast
        int write_len = static_cast<int>(length);
        if (BIO_write(b64, data, write_len) <= 0) {
            throw std::runtime_error("Error escribiendo datos en BIO");
        }

        if (BIO_flush(b64) != 1) {
            throw std::runtime_error("Error en BIO_flush");
        }

        char* buffer = nullptr;
        long buffer_len = BIO_get_mem_data(b64, &buffer);

        if (buffer && buffer_len > 0) {
            result.assign(buffer, buffer_len);
        }
    } catch (...) {
        if (b64) BIO_free_all(b64);
        throw;
    }

    BIO_free_all(b64);
    return result;
}

std::vector<char> DiskController::base64_decode(const std::string& encoded) {
    // Verifica tamaño máximo
    if (encoded.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("Datos codificados demasiado grandes");
    }

    BIO *b64 = BIO_new(BIO_f_base64());
    BIO *mem = BIO_new_mem_buf(encoded.data(), static_cast<int>(encoded.size()));
    BIO_push(b64, mem);
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);

    std::vector<char> decoded(encoded.size());
    int len = BIO_read(b64, decoded.data(), static_cast<int>(encoded.size()));

    if (len > 0) {
        decoded.resize(len);
    } else {
        decoded.clear();
    }

    BIO_free_all(b64);
    return decoded;
}

std::vector<std::string> DiskController::listAvailableFiles() {
    return registeredFiles;
}

void DiskController::startServer() {
    server.Post("/", receiveXML);
    server.Get("/status", [this](const httplib::Request&, httplib::Response& res) {
        res.set_content("Controller activo", "text/plain");
    });

    std::cout << "Controller Server iniciado en puerto 1717\n";
    if (!server.listen("0.0.0.0", 1717)) {
        std::cerr << "Error al iniciar el servidor HTTP!" << std::endl;
    }
}

void DiskController::receiveXML(const httplib::Request& req, httplib::Response& res) {
    std::string contentType = req.get_header_value("Content-Type");

    if (contentType.find("application/xml") != std::string::npos) {
        res.set_content("XML recibido correctamente", "text/plain");
    } else {
        res.status = 415; //Tipo no soportado
        res.set_content("Solo se acepta XML", "text/plain");
    }
}

std::future<std::vector<DiskController::NodeStatus>> DiskController::getNodesStatusAsync() {
    return std::async(std::launch::async, [this]() {
        std::vector<NodeStatus> status;
        std::vector<std::future<NodeStatus>> futures;

        for (size_t i = 0; i < diskNodeUrls.size(); i++) {
            futures.emplace_back(std::async(std::launch::async, [this, i]() {
                httplib::Client client(diskNodeUrls[i]);
                client.set_connection_timeout(4); // 1 segundo es suficiente para LAN
                client.set_read_timeout(4);

                try {
                    auto res = client.Get("/status");

                    if (res && res->status == 200) {
                        auto json = nlohmann::json::parse(res->body);
                        return NodeStatus{
                        json.value("node_id", static_cast<int>(i + 1)),
                        json.value("port", json.value("port", 5000 + static_cast<int>(i + 1))),
                        json.value("used_blocks", 0),
                        json.value("total_blocks", 0),
                        true
                        };
                    }
                } catch (...) {
                    // Ignora errores, retorna estado por defecto
                }
                
                return NodeStatus{
                    static_cast<int>(i+1),
                    5000 + static_cast<int>(i+1),
                    0,
                    0,
                    false
                };
            }));
        }

        for (auto& f : futures) {
            status.push_back(f.get());
        }

        return status;
    });
}


void DiskController::registerFile(const File& file) {
    std::lock_guard<std::mutex> lock(filesDataMutex);
    registeredFilesData.push_back(file);
    saveMetadata();
}

bool DiskController::removeFile(const std::string& fileId) {
    std::lock_guard<std::mutex> lock(filesDataMutex);
    auto it = std::remove_if(registeredFilesData.begin(), registeredFilesData.end(),
        [&fileId](const File& f) { return f.id == fileId; });
    
    if (it != registeredFilesData.end()) {
        registeredFilesData.erase(it, registeredFilesData.end());
        saveMetadata();
        return true;
    }
    return false;
}

const std::vector<File>& DiskController::getFiles() const {
    std::lock_guard<std::mutex> lock(filesDataMutex);
    return registeredFilesData;
}

const File* DiskController::findFile(const std::string& fileId) const {
    std::lock_guard<std::mutex> lock(filesDataMutex);
    for (const auto& file : registeredFilesData) {
        if (file.id == fileId) return &file;
    }
    return nullptr;
}

void DiskController::loadMetadata() {
    std::lock_guard<std::mutex> lock(filesDataMutex);
    std::ifstream file(dataFilePath);
    if (file.good()) {
        try {
            nlohmann::json j;
            file >> j;
            for (const auto& item : j) {
                File f(item);
                registeredFilesData.push_back(f);

                // --- Reconstruir estructuras ---
                {
                    std::lock_guard<std::mutex> lockMap(fileMapMutex);
                    std::lock_guard<std::mutex> lockFiles(filesMutex);
                    
                    // 1. Agregar a registeredFiles
                    registeredFiles.push_back(f.filename);
                    
                    // 2. Reconstruir fileBlockMap si hay datos
                    if (!f.blockMap.empty()) {
                        fileBlockMap[f.filename] = f.blockMap;
                    }
                }
            }
        } catch (...) {
            std::cerr << "Error cargando metadatos" << std::endl;
        }
    }
}

void DiskController::saveMetadata() {
    std::lock_guard<std::mutex> lock(filesDataMutex);
    nlohmann::json j;
    for (const auto& file : registeredFilesData) {
        j.push_back(file.toJson());
    }
    std::ofstream file(dataFilePath);
    file << j.dump(4);
}

bool DiskController::deleteFile(const std::string& fileId) {
    // 1. Buscar el archivo en los metadatos
    const File* file = findFile(fileId);
    if (!file) return false;

    // 2. Eliminar bloques de los nodos
    try {
        std::vector<std::future<bool>> deleteFutures;
        const std::string& filename = file->filename;

        if (fileBlockMap.find(filename) != fileBlockMap.end()) {
            const auto& blocks = fileBlockMap[filename];
            size_t totalStripes = blocks.size() / diskNodeUrls.size();

            for (size_t stripeIdx = 0; stripeIdx < totalStripes; ++stripeIdx) {
                for (int nodeIdx = 0; nodeIdx < diskNodeUrls.size(); ++nodeIdx) {
                    bool isParity = blocks[stripeIdx * diskNodeUrls.size() + nodeIdx].second;
                    std::string blockType = isParity ? "parity_" : "block_";
                    std::string blockName = blockType + filename + "_stripe_" + std::to_string(stripeIdx);

                    deleteFutures.push_back(std::async(std::launch::async, [this, nodeIdx, blockName]() {
                        httplib::Client client(diskNodeUrls[nodeIdx]);
                        auto res = client.Delete(("/delete_block/" + blockName).c_str());
                        return res && res->status == 200;
                    }));
                }
            }

            // Verificar que todos los borrados fueron exitosos
            for (auto& future : deleteFutures) {
                if (!future.get()) return false;
            }

            // 3. Eliminar de las estructuras internas
            {
                std::lock_guard<std::mutex> lockMap(fileMapMutex);
                std::lock_guard<std::mutex> lockFiles(filesMutex);
                fileBlockMap.erase(filename);
                registeredFiles.erase(std::remove(registeredFiles.begin(), registeredFiles.end(), filename), registeredFiles.end());
            }

            // 4. Eliminar de los metadatos
            return removeFile(fileId);
        }
    } catch (...) {
        return false;
    }
    return false;
}