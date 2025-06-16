#pragma once
#include "httplib.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "Client.h"
#include "Entity.h"

#include <GLFW/glfw3.h>
#include <iostream>

class InterfaceGui {
	public:
		InterfaceGui();
		void run();

	private:
		void initUI();
		void mainLoop();
		void update();
		void cleanup();

		GLFWwindow* window = nullptr;
		char port_input[16];
		char path_input[256];
		int diskNode_cant = 0;

		Client client;
};