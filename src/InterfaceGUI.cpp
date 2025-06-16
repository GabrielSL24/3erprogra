#include "InterfaceGUI.h"
#include <cstring>

// Constructor de la clase
InterfaceGui::InterfaceGui() {}

// Funcion que ejecuta la GUI
void InterfaceGui::run() {
	initUI();
	mainLoop();
	cleanup();
}


void InterfaceGui::initUI() { 
	if (!glfwInit()) {
		std::cerr << "Error al iniciar GLFW" << std::endl;
		exit(EXIT_FAILURE);
	}

	//Configuracion de la version de OpenGL
	const char* glsl_version = "#version 330";
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
	glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

	window = glfwCreateWindow(800, 600, "TECMFS-DISK", NULL, NULL);
	if (window == NULL) {
		std::cerr << "Error al crear ventana de GLFW" << std::endl;
		glfwTerminate();
		exit(EXIT_FAILURE);
	}

	glfwMakeContextCurrent(window);
	glfwSwapInterval(1);

	IMGUI_CHECKVERSION(); // Inicializa ImGui
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO(); (void)io;

	ImGui::StyleColorsDark();
	ImGui_ImplGlfw_InitForOpenGL(window, true);
	ImGui_ImplOpenGL3_Init(glsl_version);

	//Incializar buffers
	port_input[0] = '\0';
	path_input[0] = '\0';
}

// Ciclo principal de la aplicacion
void InterfaceGui::mainLoop() {
	while (!glfwWindowShouldClose(window)) {
		glfwPollEvents();

		ImGui_ImplOpenGL3_NewFrame();
		ImGui_ImplGlfw_NewFrame();
		ImGui::NewFrame();

		update(); // logica de la interfaz

		ImGui::Render();
		int display_w, display_h;
		glfwGetFramebufferSize(window, &display_w, &display_h);
		glViewport(0, 0, display_w, display_h);
		glClearColor(0.1f, 0.1f, 0.1f, 1.0f); // Fondo de color
		glClear(GL_COLOR_BUFFER_BIT);
		ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

		glfwSwapBuffers(window);
	}
}


void InterfaceGui::update() {
	ImGui::Begin("TECMFS Config");

	ImGui::InputText("Puerto", port_input, IM_ARRAYSIZE(port_input));
	ImGui::InputText("Path", path_input, IM_ARRAYSIZE(path_input));

	if (diskNode_cant >= 4) {
		ImGui::TextColored(ImVec4(1, 0, 0, 1), "Ya se enviaron los 4 Disk Nodes maximos.");
	}
	else {
		std::string label = "Enviar datos Disk Node " + std::to_string(diskNode_cant + 1);
		if (ImGui::Button(label.c_str())) {
			try {
				int port_in = std::stoi(port_input);
				std::string port = std::to_string(port_in);
				std::string path = path_input;
				 
				auto result = client.sendXML(port, path); // Envia XML al servidor
				
				switch (result) {
					case Client::XMLResult::SUCCESS:
						diskNode_cant++;
						break;
					case Client::XMLResult::INVALIDXML:
						std::cerr << "Faltan datos: puerto o path.\n";
						break;
					case Client::XMLResult::SERVERERROR:
						std::cerr << "Error desconocido en el servidor.\n";
						break;
					case Client::XMLResult::CONNECTIONERROR:
						std::cerr << "No se pudo conectar al servidor.\n";
						break;
				}
			}
			catch (const std::exception& e) {
				std::cerr << "Puerto invalido: " << e.what() << std::endl;
			}
		}
	}
	ImGui::End();
}

void InterfaceGui::cleanup() {
	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplGlfw_Shutdown();
	ImGui::DestroyContext();
	glfwDestroyWindow(window);
	glfwTerminate();
}
