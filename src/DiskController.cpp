#include "DiskController.h"

// Constructor de la clase
DiskController::DiskController() {
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