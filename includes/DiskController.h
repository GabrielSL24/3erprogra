#pragma once
#include "httplib.h"          // Servidor HTTP para comunicacion con Disk Nodes y GUI
#include "Entity.h"           // Definiciones de entidades (ej: bloques, metadata)
#include "File.h" 
#include <string>
#include <vector>
#include <unordered_map>
#include <filesystem> // Manejo de rutas de archivos
#include <nlohmann/json.hpp> // Para respuestas API en JSON
#include <openssl/bio.h> // Calculo de paridad (XOR a nivel de bytes)
#include <openssl/evp.h>
#include <future> // Operaciones asincronas como la reconstruccion de documentos.
#include <fstream>

class DiskController {
public:
    DiskController(); // Inicializa conexiones con Disk Nodes
    //Distribucion de archivos en RAID 5
    void distributeFile(const std::string& filePath);
    //Recuperacion de archivos con capacidad de tolerar fallos
    std::vector<char> retrieveFile(const std::string& filename);

    //Comunicacion con los Disknodes
    // Envia bloques a los nodos mediante HTTP POST
    void writeToDiskNode(
        const std::string& node_url,    // Ej: "http://localhost:5002"
        const std::string& block_id,    // Ej: "doc1.pdf.binary-block2"
        const std::vector<char>& data,  // Datos o paridad
        bool is_parity
    );

    //Reconstruye un bloque faltante usando XOR.
    std::vector<char> reconstructMissingBlock(const std::vector<std::pair<std::vector<char>, bool>>& availableBlocksInfo,
                                            int missingNodeIndex,
                                            size_t stripeIndex);
    //Lista los archivos registrados en el RAID, Api para GUI
    std::vector<char> reconstructFromParity(const std::vector<std::vector<char>>& blocks,
                                           int missingIndex);
    std::vector<std::string> listAvailableFiles(); //Verifica conexion y espacio en nodos (async)

    struct NodeStatus {
        int nodeId;         // ID del nodo
        int port;           // Puerto del nodo
        int usedBlocks;     // Bloques usados
        int totalBlocks;    // Capacidad total
        bool isActive;      // Estado de conexión
    };

    std::future<std::vector<NodeStatus>> getNodesStatusAsync();

    void startServer(); //Inicia el servidor en un hilo separado
    static void receiveXML(const httplib::Request& req, httplib::Response& res);//Recibe config XML de los Disknodes

    void registerFile(const File& file);
    bool removeFile(const std::string& fileId);
    bool deleteFile(const std::string& fileId);
    const std::vector<File>& getFiles() const;
    const File* findFile(const std::string& fileId) const;

private:
    int connect_S();

    std::vector<std::vector<char>> splitFile(const std::string& filePath, size_t blockSize = 4096);
    std::vector<char> calculateParity(const std::vector<std::vector<char>>& blocks);
    static std::string base64_encode(const char* data, size_t length);
    static std::vector<char> base64_decode(const std::string& encoded);
    int getParityPositionForStripe(size_t stripeIndex) const;

    std::vector<char> attemptAlternativeRecovery(
        const std::vector<std::pair<std::vector<char>, bool>>& availableBlocks,
        size_t stripeIndex);

    // Variables
    httplib::Server server;  // Mover el servidor como miembro de clase
    std::thread serverThread;

    std::mutex fileMapMutex;  // Para proteger fileBlockMap
    std::mutex filesMutex;    // Para proteger registeredFiles

    std::vector<std::string> registeredFiles;    // Archivos almacenados
    std::vector<std::string> diskNodeUrls = {    // URLs de los nodos
        "http://127.0.0.1:5001",
        "http://127.0.0.1:5002",
        "http://127.0.0.1:5003",
        "http://127.0.0.1:5004"
    };

    std::unordered_map<std::string, std::vector<std::pair<int, bool>>> fileBlockMap; // Mapa archivo->bloques
    int currentParityPosition = 0; // Para rotacion de paridad en RAID 5

    std::vector<File> registeredFilesData;  // Lista de archivos
    mutable std::mutex filesDataMutex;
    std::string dataFilePath = "files.json";

    void loadMetadata();
    void saveMetadata();
};