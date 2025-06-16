#pragma once

class Entity {
	public:
		enum Type {
			ADD,
			REMOVE,
			SEARCH,
			DOWNLOAD,
			NODES,
			UNKNOWN
		};

		static const char* TypeToString(Type type) {
			switch (type) {
				case Entity::ADD:
					return "ADD";
				case Entity::REMOVE:
					return "REMOVE";
				case Entity::SEARCH:
					return "SEARCH";
				case Entity::DOWNLOAD:
					return "DOWNLOAD";
				default:
					return "UNKNOWN";
			}
		}

		static const Type stringToType(std::string& type) {
			if (type == "ADD") return Entity::ADD;
			if (type == "REMOVE") return Entity::REMOVE;
			if (type == "SEARCH") return Entity::SEARCH;
			if (type == "DOWNLOAD") return Entity::DOWNLOAD;
			return Entity::UNKNOWN;
		}
};