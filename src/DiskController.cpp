#include "DiskController.h"

// Constructor de la clase
DiskController::DiskController() {
    // Test básico - esto sería temporal
    std::vector<char> test_data = {'H', 'e', 'l', 'l', 'o'};

    // Escribe en los 4 nodos (simulando distribución RAID 5)
    writeToDiskNode("http://localhost:5005", "block_1", test_data, false);
    writeToDiskNode("http://localhost:5002", "block_2", test_data, false);
    writeToDiskNode("http://localhost:5003", "block_3", test_data, false);
    writeToDiskNode("http://localhost:5004", "parity_1", test_data, true);

	 connect_S();
}

// Funcion que inicia el servidor
int DiskController::connect_S() {
	httplib::Server svr;

    // Manejo de solicitudes
    svr.Post("/", [](const httplib::Request& req, httplib::Response& res) {
        std::string contentType = req.get_header_value("Content-Type");
        
        // Verifica si el contenido es XML
        if (contentType.find("application/xml") != std::string::npos) {
            receiveXML(req, res);
        }
        else {
            res.status = 415;
            res.set_content("Tipo de contenido no soportado", "text/plain");
        }
    });
    
    std::cout << "Servidor escuchando...\n";
    svr.listen("0.0.0.0", 1717);

    return 0;
}

void DiskController::writeToDiskNode(const std::string& node_url,
                                    const std::string& block_id,
                                    const std::vector<char>& data,
                                    bool is_parity) {
    httplib::Client client(node_url);
    client.set_connection_timeout(5);  // Timeout de 5 segundos
    client.set_read_timeout(5);

    std::string base64_data = base64_encode(data.data(), data.size());
    nlohmann::json request;
    request["block_id"] = block_id;
    request["data"] = base64_data;
    request["is_parity"] = is_parity;

    std::cout << "Attempting to write to: " << node_url << std::endl;

    auto res = client.Post("/write_block", request.dump(), "application/json");

    if (res) {
        std::cout << "Success writing to " << node_url
                  << " - Status: " << res->status
                  << " - Response: " << res->body << std::endl;
    } else {
        auto err = res.error();
        std::cerr << "Failed to write to " << node_url
                  << " - Error: " << httplib::to_string(err) << std::endl;
    }
}

std::string DiskController::base64_encode(const char* data, size_t length) {
    if (!data || length == 0) return "";

    BIO *b64 = BIO_new(BIO_f_base64());
    BIO *mem = BIO_new(BIO_s_mem());
    BIO_push(b64, mem);
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);

    if (BIO_write(b64, data, static_cast<int>(length)) <= 0) {
        BIO_free_all(b64);
        return "";
    }
    BIO_flush(b64);

    BUF_MEM *bufferPtr;
    BIO_get_mem_ptr(b64, &bufferPtr);

    // Construcción segura del string
    std::string encoded(bufferPtr->data, bufferPtr->length);
    BIO_free_all(b64);

    return encoded;
}

void DiskController::receiveXML(const httplib::Request& req, httplib::Response& res) {
    tinyxml2::XMLDocument doc;

    if (doc.Parse(req.body.c_str()) != tinyxml2::XML_SUCCESS) {
        res.status = 400;
        res.set_content("XML mal formado", "text/plain");
        return;
    }

    auto* root = doc.FirstChildElement("DiskNode");
    if (!root) {
        res.status = 400;
        res.set_content("Nodo DiskNode faltante", "text/plain");
        return;
    }

    const char* port = root->FirstChildElement("Port") ? root->FirstChildElement("Port")->GetText() : nullptr;
    const char* path = root->FirstChildElement("Path") ? root->FirstChildElement("Path")->GetText() : nullptr;

    if (!port || !path) {
        res.status = 400;
        res.set_content("Faltan datos (Port o Path)", "text/plain");
        return;
    }

    std::cout << "[Servidor] Puerto recibido (XML): " << port << "\n";
    std::cout << "[Servidor] Path recibido (XML): " << path << "\n";

    res.set_content("Nodo registrado exitosamente", "text/plain");
}