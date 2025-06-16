#pragma once
#include "httplib.h"
#include "Entity.h"
#include <iostream>

class Client {
	public:
		enum class XMLResult {
			SUCCESS,
			INVALIDXML,
			CONNECTIONERROR,
			SERVERERROR
		};

		Client();
		Client::XMLResult sendXML(const std::string& port, const std::string& path);

	private:

		
};