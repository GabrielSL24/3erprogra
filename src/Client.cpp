#include "Client.h"

//Constructor
Client::Client() {}

// Funcion para enviar el XML de la configuarcion de los DiskNodes al DiskController
Client::XMLResult Client::sendXML(const std::string& port, const std::string& path) {
	httplib::Client cli("localhost", 1717);

	// Construye el contenido xml
	std::string xml =
		" <DiskNode>\n";
		xml += "    <Port>" + port + "</Port>\n";
		xml += "    <Path>" + path + "</Path>\n";
		xml += "</DiskNode>";

		// Envia el XML
		if (auto res = cli.Post("/", xml, "application/xml")) {
			std::cout << "Respuesta del servidor: " << res->body << std::endl;
			if (res->status == 200) {
				return XMLResult::SUCCESS;
			}
			else if (res->status == 400) {
				return XMLResult::INVALIDXML;
			}
			else {
				std::cerr << "Error del servidor: " << res->status << " - " << res->body << std::endl;
				return XMLResult::SERVERERROR;
			}
			
		}
		else {
			std::cerr << "Error al conectar con el servidor." << std::endl;
			return XMLResult::CONNECTIONERROR;
		}
}