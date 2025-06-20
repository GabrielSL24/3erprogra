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

        std::cout << "Archivo '" << filename << "' distribuido exitosamente.\n";

    } catch (const std::exception& e) {
        std::cerr << "Error en distributeFile: " << e.what() << std::endl;
        throw;
    }
}

std::vector<char> DiskController::retrieveFile(const std::string& filename) {
    std::vector<char> fullFile;
    size_t totalStripes = fileBlockMap[filename].size() / diskNodeUrls.size();

    for (size_t stripeIdx = 0; stripeIdx < totalStripes; ++stripeIdx) {
        std::vector<std::vector<char>> blocks;
        std::vector<int> nodeIndices;
        int missingNode = -1;

        //Recupera todos los bloques (datos + paridad)
        for (int nodeIdx = 0; nodeIdx < diskNodeUrls.size(); ++nodeIdx) {
            bool isParity = fileBlockMap[filename][stripeIdx * diskNodeUrls.size() + nodeIdx].second;
            std::string blockType = isParity ? "parity_" : "block_";
            std::string blockName = blockType + filename + "_stripe_" + std::to_string(stripeIdx);

            httplib::Client client(diskNodeUrls[nodeIdx]);
            auto res = client.Get(("/read_block/" + blockName).c_str());

            if (res && res->status == 200) {
                auto json = nlohmann::json::parse(res->body);
                blocks.push_back(base64_decode(json["data"]));
                nodeIndices.push_back(nodeIdx);
            } else {
                missingNode = nodeIdx;
            }
        }

        //Reconstruccion correcta (considera paridad)
        std::vector<char> stripeData;
        if (blocks.size() >= diskNodeUrls.size() - 1) { // Al menos 3 bloques (2 datos + 1 paridad)
            if (missingNode != -1) {
                stripeData = reconstructMissingBlock(blocks, missingNode);
            } else {
                //Selecciona solo los bloques de datos (excluir paridad)
                for (size_t i = 0; i < blocks.size(); ++i) {
                    if (!fileBlockMap[filename][stripeIdx * diskNodeUrls.size() + nodeIndices[i]].second) {
                        stripeData = blocks[i];
                        break;
                    }
                }
            }
        } else {
            throw std::runtime_error("Bloques insuficientes para stripe " + std::to_string(stripeIdx));
        }

        fullFile.insert(fullFile.end(), stripeData.begin(), stripeData.end());
    }

    return fullFile;
}

std::vector<char> DiskController::reconstructMissingBlock(
    const std::vector<std::vector<char>>& availableBlocks,
    int missingNodeIndex)
{
    if (availableBlocks.size() != diskNodeUrls.size() - 1) {
        throw std::runtime_error("Se requieren " +
                               std::to_string(diskNodeUrls.size() - 1) +
                               " bloques para reconstruccion");
    }

    if (missingNodeIndex < 0 || missingNodeIndex >= static_cast<int>(diskNodeUrls.size())) {
        throw std::runtime_error("Indice de nodo faltante invalido");
    }

    std::vector<char> reconstructed(availableBlocks[0].size());
    for (const auto& block : availableBlocks) {
        for (size_t i = 0; i < block.size(); ++i) {
            reconstructed[i] ^= block[i];
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
                client.set_connection_timeout(1); // 1 segundo es suficiente para LAN
                client.set_read_timeout(1);

                try {
                    auto res = client.Get("/status");
                    if (res && res->status == 200) {
                        auto json = nlohmann::json::parse(res->body);
                        return NodeStatus{
                            json.value("node_id", static_cast<int>(i+1)),
                            json.value("port", 5000 + static_cast<int>(i+1)),
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