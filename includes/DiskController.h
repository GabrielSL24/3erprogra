#pragma once
#include "httplib.h"
#include "Entity.h"
#include <sstream>
#include <unordered_map>
#include <iostream>
#include <tinyxml2.h>
#include <nlohmann/json.hpp>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>

class DiskController {
public:
	DiskController();
	void writeToDiskNode(const std::string& node_url,
						const std::string& block_id,
						const std::vector<char>& data,
						bool is_parity);
private:
	int connect_S();
	static void receiveXML(const httplib::Request& req, httplib::Response& res);
	static std::string base64_encode(const char* data, size_t length);
};