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
        // 1. Validaciones iniciales
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

        // 2. Lee y divide el archivo
        std::ifstream file(filePath, std::ios::binary | std::ios::ate);
        size_t fileSize = file.tellg();
        file.seekg(0, std::ios::beg);

        const size_t blockSize = 4096;
        size_t numBlocks = (fileSize + blockSize - 1) / blockSize;
        std::vector<std::vector<char>> blocks(numBlocks);

        for (size_t i = 0; i < numBlocks; ++i) {
            size_t bytesToRead = (i == numBlocks - 1) ? (fileSize % blockSize) : blockSize;
            if (bytesToRead == 0) bytesToRead = blockSize; // Para archivos exactamente múltiplos

            blocks[i].resize(bytesToRead);
            file.read(blocks[i].data(), bytesToRead);
        }

        // 3. Distribución RAID 5
        std::vector<std::future<void>> writeFutures;
        std::vector<std::pair<int, bool>> blockMapEntries;

        for (size_t stripeIdx = 0; stripeIdx < numBlocks; ++stripeIdx) {
            int parityPos = getParityPositionForStripe(stripeIdx);
            std::vector<char> parityBlock(blocks[stripeIdx].size(), 0);

            // Calcular paridad (XOR de todos los bloques de datos)
            for (int nodeIdx = 0; nodeIdx < 4; ++nodeIdx) {
                if (nodeIdx != parityPos) {
                    for (size_t i = 0; i < blocks[stripeIdx].size(); ++i) {
                        parityBlock[i] ^= blocks[stripeIdx][i];
                    }
                }
            }

            // Guarda metadatos y inicia escrituras
            for (int nodeIdx = 0; nodeIdx < 4; ++nodeIdx) {
                bool isParity = (nodeIdx == parityPos);
                blockMapEntries.emplace_back(nodeIdx, isParity);

                std::string blockName = filename + "_stripe_" + std::to_string(stripeIdx);

                std::string node_url = diskNodeUrls[nodeIdx];
                std::vector<char> data_copy = isParity ? parityBlock : blocks[stripeIdx];
                bool is_parity = isParity;

                writeFutures.push_back(std::async(std::launch::async,
                    [this, node_url, blockName, data_copy, is_parity]() {
                        this->writeToDiskNode(node_url, blockName, data_copy, is_parity);
                    }
                ));
            }
        }

        bool all_writtes = true;
        // Espera a que toda la escritura termine
        for (auto& future : writeFutures) {
            try {
                future.get();
            }
            catch (...) {
                all_writtes = false;
                std::cerr << "Error al escribir bloque" << std::endl;
            }
        }

        if (!all_writtes) {
            throw std::runtime_error("No se pudieron escribir todos los bloques del archivo");
        }

        // VERIFICACIÓN DE BLOQUES ESCRITOS (NUEVO)
        for (size_t stripeIdx = 0; stripeIdx < numBlocks; ++stripeIdx) {
            for (int nodeIdx = 0; nodeIdx < 4; ++nodeIdx) {
                bool isParity = (nodeIdx == getParityPositionForStripe(stripeIdx));
                std::string blockName = (isParity ? "parity_" : "block_") + filename + "_stripe_" + std::to_string(stripeIdx);

                try {
                    httplib::Client client(diskNodeUrls[nodeIdx]);
                    auto res = client.Get(("/read_block/" + blockName).c_str());
                    if (!res || res->status != 200) {
                        throw std::runtime_error("Fallo al verificar bloque " + blockName + " en nodo " + diskNodeUrls[nodeIdx]);
                    }
                }
                catch (...) {
                    throw std::runtime_error("Error al verificar bloque " + blockName + " en nodo " + diskNodeUrls[nodeIdx]);
                }
            }
        }

        // Registra archivo si salió bien
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
        }
        catch (const std::exception& e) {
            std::cerr << "Error en registerFile: " << e.what() << std::endl;
        }

        std::cout << "Archivo '" << filename << "' distribuido exitosamente.\n";

    }
    catch (const std::exception& e) {
        std::cerr << "Error en distributeFile: " << e.what() << std::endl;
        throw;
    }
}

std::vector<char> DiskController::retrieveFile(const std::string& filename) {
    std::vector<char> fullFile;
    std::lock_guard<std::mutex> lock(fileMapMutex);;
    
    if (fileBlockMap.find(filename) == fileBlockMap.end()) {
        throw std::runtime_error("Archivo no encontrado en los metadatos");
    }

    size_t totalStripes = fileBlockMap[filename].size() / diskNodeUrls.size();

    for (size_t stripeIdx = 0; stripeIdx < totalStripes; ++stripeIdx) {
        std::vector<std::pair<std::vector<char>, bool>> blocksWithInfo;
        int missingNode = -1;
        size_t parityPos = stripeIdx % diskNodeUrls.size();

        // Recuperar bloques disponibles
        for (int nodeIdx = 0; nodeIdx < diskNodeUrls.size(); ++nodeIdx) {
            bool isParity = fileBlockMap[filename][stripeIdx * diskNodeUrls.size() + nodeIdx].second;
            std::string blockType = isParity ? "parity_" : "block_";
            std::string blockName = blockType + filename + "_stripe_" + std::to_string(stripeIdx);

            try {
                httplib::Client client(diskNodeUrls[nodeIdx]);
                client.set_connection_timeout(5);
                client.set_read_timeout(5);
                auto res = client.Get(("/read_block/" + blockName).c_str());

                if (res && res->status == 200) {
                    auto json = nlohmann::json::parse(res->body);
                    if (!json["data"].is_null() && !json["data"].empty()) {
                        auto blockData = base64_decode(json["data"]);
                        if (!blockData.empty()) {
                            blocksWithInfo.emplace_back(blockData, isParity);
                            continue;
                        }
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
                // Verificar si podemos continuar con dos nodos faltantes
                // (si uno de ellos es el de paridad)
                bool firstMissingIsParity = (static_cast<size_t>(missingNode) == parityPos);
                bool currentMissingIsParity = (static_cast<size_t>(nodeIdx) == parityPos);

                if (!(firstMissingIsParity || currentMissingIsParity)) {
                    throw std::runtime_error("Demasiados bloques de datos faltantes para stripe " +
                        std::to_string(stripeIdx));
                }
            }
        }

        // Filtrar bloques vacíos o inválidos
        blocksWithInfo.erase(
            std::remove_if(blocksWithInfo.begin(), blocksWithInfo.end(),
                [](const auto& blockInfo) {
                    return blockInfo.first.empty();
                }),
            blocksWithInfo.end()
        );

        std::vector<char> stripeData;
        if (missingNode != -1) {
            try {
                stripeData = reconstructMissingBlock(blocksWithInfo, missingNode, stripeIdx);
            }
            catch (const std::exception&) {
                // Intentar recuperación alternativa si falla la reconstrucción estándar
                if (blocksWithInfo.size() >= diskNodeUrls.size() - 2) {
                    stripeData = attemptAlternativeRecovery(blocksWithInfo, stripeIdx);
                }
                else {
                    throw;
                }
            }
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
    // Verificación básica de PDF
    if (filename.find(".pdf") != std::string::npos && fullFile.size() > 4) {
        if (!(fullFile[0] == '%' && fullFile[1] == 'P' &&
            fullFile[2] == 'D' && fullFile[3] == 'F')) {
            std::cerr << "ADVERTENCIA: Cabecera PDF no válida. El archivo puede estar corrupto." << std::endl;
            // Guardar copia para análisis
            std::ofstream out("corrupt_" + filename, std::ios::binary);
            out.write(fullFile.data(), fullFile.size());
            throw std::runtime_error("El PDF reconstruido no tiene una cabecera válida");
        }
    }
    return fullFile;
}

std::vector<char> DiskController::reconstructMissingBlock(
    const std::vector<std::pair<std::vector<char>, bool>>& availableBlocksInfo,
    int missingNodeIndex,
    size_t stripeIndex)
{
    // 1. Verificar que tenemos bloques suficientes y determinar tamaño
    if (availableBlocksInfo.empty()) {
        throw std::runtime_error("No hay bloques disponibles para reconstrucción");
    }

    // Determinar el tamaño esperado del bloque
    size_t expected_size = 0;
    for (const auto& block : availableBlocksInfo) {
        if (!block.first.empty()) {
            expected_size = block.first.size();
            break;
        }
    }

    // Verificar consistencia en tamaños de los bloques disponibles
    for (const auto& block : availableBlocksInfo) {
        if (!block.first.empty() && block.first.size() != expected_size) {
            throw std::runtime_error("Inconsistencia en tamaños de bloque durante reconstrucción");
        }
    }

    std::vector<char> reconstructed(expected_size, 0);
    size_t parityPos = stripeIndex % diskNodeUrls.size();
    bool missingParity = (static_cast<size_t>(missingNodeIndex) == parityPos);

    if (missingParity) {
        // Reconstruir paridad: XOR de todos los bloques de datos
        int dataBlocksProcessed = 0;
        for (const auto& block : availableBlocksInfo) {
            if (!block.second) { // Si es un bloque de datos
                dataBlocksProcessed++;
                for (size_t i = 0; i < expected_size; ++i) {
                    reconstructed[i] ^= block.first[i];
                }
            }
        }
        if (dataBlocksProcessed < diskNodeUrls.size() - 1) {
            throw std::runtime_error("No hay suficientes bloques de datos para reconstruir paridad");
        }
    }
    else {
        // Reconstruir bloque de datos
        bool parityFound = false;
        for (const auto& block : availableBlocksInfo) {
            if (block.second) { // Bloque de paridad
                parityFound = true;
                // Asegurarnos de copiar solo hasta el tamaño esperado
                size_t copy_size = std::min(expected_size, block.first.size());
                std::copy(block.first.begin(), block.first.begin() + copy_size, reconstructed.begin());
                break;
            }
        }

        if (!parityFound) {
            throw std::runtime_error("No se encontró bloque de paridad");
        }

        // Aplicar XOR con los otros bloques de datos
        for (const auto& block : availableBlocksInfo) {
            if (!block.second) {
                for (size_t i = 0; i < expected_size; ++i) {
                    reconstructed[i] ^= block.first[i];
                }
            }
        }
    }

    return reconstructed;
}

int DiskController::getParityPositionForStripe(size_t stripeIndex) const {
    return stripeIndex % 4;  // Rotacion entre 0-3 para 4 nodos
}

std::vector<char> DiskController::attemptAlternativeRecovery(
    const std::vector<std::pair<std::vector<char>, bool>>& availableBlocks,
    size_t stripeIndex)
{
    (void)stripeIndex;
    // 1. Determinar tamaño de bloque
    size_t blockSize = 0;
    for (const auto& block : availableBlocks) {
        if (!block.first.empty()) {
            blockSize = block.first.size();
            break;
        }
    }

    if (blockSize == 0) {
        throw std::runtime_error("No hay bloques válidos para recuperación alternativa");
    }

    // 2. Reconstruir datos (sin usar stripeIndex ni nodeUrls)
    std::vector<char> recoveredData(blockSize, 0);
    bool hasDataBlocks = false;

    // Intentar reconstruir solo con los bloques de datos disponibles
    for (const auto& block : availableBlocks) {
        if (!block.second) { // Si es un bloque de datos
            hasDataBlocks = true;
            for (size_t i = 0; i < blockSize; ++i) {
                recoveredData[i] ^= block.first[i];
            }
        }
    }

    if (!hasDataBlocks) {
        throw std::runtime_error("No hay bloques de datos disponibles");
    }

    return recoveredData;
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
    // First add to the collection
    {
        std::lock_guard<std::mutex> lock(filesDataMutex);
        registeredFilesData.push_back(file);
    }  // Mutex released here
    
    // Then persist to disk
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

const File* DiskController::findFileByName(const std::string& filename) const {
    std::lock_guard<std::mutex> lock(filesDataMutex);
    for (const auto& file : registeredFilesData) {
        if (file.filename == filename) {
            return &file;
        }
    }
    return nullptr;
}

void DiskController::loadMetadata() {
    std::ifstream file(dataFilePath);
    if (!file.is_open()) {
        std::cout << "No existing metadata found at " << dataFilePath 
                  << ", starting fresh." << std::endl;
        return;
    }

    try {
        nlohmann::json j;
        file >> j;

        std::lock_guard<std::mutex> lockData(filesDataMutex);
        std::lock_guard<std::mutex> lockMap(fileMapMutex);
        std::lock_guard<std::mutex> lockFiles(filesMutex);

        for (const auto& item : j) {
            File f(item);
            registeredFilesData.push_back(f);
            registeredFiles.push_back(f.filename);
            fileBlockMap[f.filename] = f.blockMap;
        }
        
        std::cout << "Loaded metadata from " << dataFilePath 
                  << " (" << registeredFilesData.size() << " files)" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Error loading metadata: " << e.what() << std::endl;
    }
}

void DiskController::saveMetadata() {
    // Create parent directories if they don't exist
    try {
        auto parent_dir = std::filesystem::path(dataFilePath).parent_path();
        if (!parent_dir.empty() && !std::filesystem::exists(parent_dir)) {
            std::filesystem::create_directories(parent_dir);
        }
    } catch (const std::exception& e) {
        std::cerr << "Error creating directories: " << e.what() << std::endl;
        return;
    }

    // Make a thread-safe copy of the data
    nlohmann::json j;
    {
        std::lock_guard<std::mutex> lock(filesDataMutex);
        for (const auto& file : registeredFilesData) {
            j.push_back(file.toJson());
        }
    }

    // Write to file (outside the lock)
    std::ofstream file(dataFilePath);
    if (!file.is_open()) {
        std::cerr << "Failed to open " << dataFilePath 
                  << " for writing: " << strerror(errno) << std::endl;
        return;
    }
    
    file << j.dump(4);
    file.close();
    
    std::cout << "Successfully saved metadata to " 
              << std::filesystem::absolute(dataFilePath) << std::endl;
}

bool DiskController::deleteFile(const std::string& filename) {
    std::cout << "[DEBUG] Iniciando eliminación para archivo: " << filename << std::endl;
    
    // 1. Bloquear todos los mutex necesarios en orden consistente
    std::unique_lock<std::mutex> lockData(filesDataMutex, std::defer_lock);
    std::unique_lock<std::mutex> lockMap(fileMapMutex, std::defer_lock);
    std::unique_lock<std::mutex> lockFiles(filesMutex, std::defer_lock);
    std::lock(lockData, lockMap, lockFiles);
    std::cout << "[DEBUG] Mutex adquiridos de forma segura" << std::endl;

    // 2. Búsqueda del archivo
    const File* file = nullptr;
    for (const auto& f : registeredFilesData) {
        if (f.filename == filename) {
            file = &f;
            break;
        }
    }
    
    if (!file) {
        std::cerr << "[ERROR] Archivo no encontrado en metadatos: " << filename << std::endl;
        return false;
    }
    std::cout << "[DEBUG] Archivo encontrado - ID: " << file->id << std::endl;

    // 3. Verificar existencia en fileBlockMap
    if (fileBlockMap.find(filename) == fileBlockMap.end()) {
        std::cerr << "[ERROR] Archivo no en fileBlockMap: " << filename << std::endl;
        return false;
    }

    // 4. Preparar eliminación RAID
    const auto& blocks = fileBlockMap[filename];
    size_t totalStripes = blocks.size() / diskNodeUrls.size();
    std::cout << "[DEBUG] Total de stripes: " << totalStripes << std::endl;

    // 5. Liberar mutex durante operaciones de red
    lockData.unlock();
    lockMap.unlock();
    lockFiles.unlock();
    std::cout << "[DEBUG] Mutex liberados para operaciones de red" << std::endl;

    // 6. Eliminación RAID distribuida
    std::vector<std::future<bool>> deleteFutures;
    for (size_t stripeIdx = 0; stripeIdx < totalStripes; ++stripeIdx) {
        for (int nodeIdx = 0; nodeIdx < diskNodeUrls.size(); ++nodeIdx) {
            bool isParity = blocks[stripeIdx * diskNodeUrls.size() + nodeIdx].second;
            std::string blockType = isParity ? "parity_" : "block_";
            std::string blockName = blockType + filename + "_stripe_" + std::to_string(stripeIdx);

            deleteFutures.push_back(std::async(std::launch::async, [this, nodeIdx, blockName]() {
                try {
                    httplib::Client client(diskNodeUrls[nodeIdx]);
                    client.set_connection_timeout(5);
                    client.set_read_timeout(5);
                    
                    std::cout << "[DEBUG] Enviando DELETE para " << blockName 
                              << " a nodo " << nodeIdx << std::endl;
                    
                    std::cout << "[DEBUG RAW] URL completa que se enviará: " 
                        << diskNodeUrls[nodeIdx] << "/delete_block/" << blockName << std::endl;
                    auto res = client.Delete(("/delete_block/" + blockName).c_str());
                    
                    if (!res) {
                        std::cerr << "[ERROR] No response from node " << nodeIdx 
                                  << " for " << blockName << std::endl;
                        return false;
                    }
                    
                    std::cout << "[DEBUG] Nodo " << nodeIdx << " respondió " 
                              << res->status << " para " << blockName << std::endl;
                    return res->status == 200;
                } catch (const std::exception& e) {
                    std::cerr << "[EXCEPCION] Error eliminando " << blockName 
                              << " en nodo " << nodeIdx << ": " << e.what() << std::endl;
                    return false;
                }
            }));
        }
    }

    // 7. Verificar resultados de eliminación RAID


    bool allDeleted = true;
    for (auto& future : deleteFutures) {
        if (!future.get()) {
            allDeleted = false;
        }
    }

    if (!allDeleted) {
        std::cerr << "[ERROR] Falló la eliminación de algunos bloques" << std::endl;
        return false;
    }

    // 8. Re-adquirir mutex para actualizar estructuras
    std::lock(lockData, lockMap, lockFiles);
    std::cout << "[DEBUG] Mutex re-adquiridos para actualización" << std::endl;

    // 9. Eliminar de estructuras internas
    fileBlockMap.erase(filename);
    registeredFiles.erase(
        std::remove(registeredFiles.begin(), registeredFiles.end(), filename), 
        registeredFiles.end()
    );
    std::cout << "[DEBUG] Eliminado de estructuras internas: " << filename << std::endl;

    // 10. Eliminar de metadatos
    auto it = std::remove_if(registeredFilesData.begin(), registeredFilesData.end(),
        [&file](const File& f) { return f.id == file->id; });
    
    if (it != registeredFilesData.end()) {
        registeredFilesData.erase(it, registeredFilesData.end());
        std::cout << "[DEBUG] Archivo eliminado exitosamente: " << filename << std::endl;
        return true;
    }

    std::cerr << "[ERROR] No se pudo eliminar de registeredFilesData" << std::endl;
    return false;
}


