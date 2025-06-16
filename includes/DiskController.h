#include "httplib.h"
#include "Entity.h"
#include <sstream>
#include <unordered_map>
#include <iostream>
#include <tinyxml2.h>


class DiskController {
	public:
		DiskController();

	private:
		int connect_S();
		static void receiveXML(const httplib::Request& req, httplib::Response& res);
};
