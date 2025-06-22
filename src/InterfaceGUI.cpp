#include "InterfaceGUI.h"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

InterfaceGui::InterfaceGui(DiskController& controller)
    : diskController(controller) {
    selectedFilePath[0] = '\0';
    std::filesystem::create_directory(DOWNLOADS_DIR);
}

InterfaceGui::~InterfaceGui() {
    if (currentOperation.valid()) {
        currentOperation.wait();
    }
}

void InterfaceGui::run() {
    initUI();
    mainLoop();
    cleanup();
}

void InterfaceGui::initUI() {
    if (!glfwInit()) {
        throw std::runtime_error("Error al iniciar GLFW");
    }

    const char* glsl_version = "#version 330";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    window = glfwCreateWindow(WINDOW_WIDTH, WINDOW_HEIGHT, WINDOW_TITLE, NULL, NULL);
    if (!window) {
        glfwTerminate();
        throw std::runtime_error("Error al crear ventana GLFW");
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    asyncRefreshFileList();
}

void InterfaceGui::mainLoop() {
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        update();
        showOperationStatus();

        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }
}

void InterfaceGui::update() {
    ImGui::Begin("TECMFS - Sistema de Archivos Distribuido", nullptr, ImGuiWindowFlags_MenuBar);

    // Menú
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("Archivo")) {
            if (ImGui::MenuItem("Subir archivo") && !operationInProgress) {
                openFileDialog();
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    // Sección de subida
    ImGui::Text("Archivo seleccionado: %s", selectedFilePath);
    if (ImGui::Button("Subir al sistema", ImVec2(120, 0)) && strlen(selectedFilePath) > 0 && !operationInProgress) {
        operationInProgress = true;
        currentOperationName = "Subiendo archivo";
        currentOperation = std::async(std::launch::async, [this]() {
            try {
                diskController.distributeFile(selectedFilePath);
                std::lock_guard<std::mutex> lock(fileListMutex);
                fileList = diskController.listAvailableFiles();
                successMessage = "Archivo subido exitosamente!";
            } catch (const std::exception& e) {
                errorMessage = e.what();
            }
            operationInProgress = false;
        });
    }

    // Lista de archivos
    ImGui::Separator();
    ImGui::Text("Archivos almacenados:");
    if (ImGui::Button("Actualizar lista", ImVec2(120, 0)) && !operationInProgress) {
        asyncRefreshFileList();
    }

    {
        std::lock_guard<std::mutex> lock(fileListMutex);
        for (const auto& file : fileList) {
            ImGui::Text("%s", file.c_str());
            //boton descargar
            ImGui::SameLine();
            if (ImGui::Button(("Descargar##" + file).c_str(), ImVec2(80, 0)) && !operationInProgress) {
                asyncDownloadFile(file);
            }
            // Botón de eliminar (nuevo)
            ImGui::SameLine();
            if (ImGui::Button(("Eliminar##" + file).c_str(), ImVec2(80, 0))) {
                if (!operationInProgress) {
                    // Confirmación antes de eliminar
                    ImGui::OpenPopup(("Confirmar##" + file).c_str());
                }
            }
            // Popup de confirmación
            if (ImGui::BeginPopupModal(("Confirmar##" + file).c_str(), nullptr, 
                                    ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::Text("¿Eliminar %s permanentemente?", file.c_str());
                ImGui::Separator();
                
                if (ImGui::Button("Sí", ImVec2(120, 0))) {
                    asyncDeleteFile(file);
                    ImGui::CloseCurrentPopup();
                }
                
                ImGui::SameLine();
                if (ImGui::Button("Cancelar", ImVec2(120, 0))) {
                    ImGui::CloseCurrentPopup();
                }
                
                ImGui::EndPopup();
            }
        }
    }

    // Estado de nodos
    ImGui::Separator();
    displayNodeStatus();

    ImGui::End();
}

void InterfaceGui::showOperationStatus() {
    if (operationInProgress) {
        ImGui::OpenPopup("Operación en progreso");
        if (ImGui::BeginPopupModal("Operación en progreso", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
            ImGui::Text("%s...", currentOperationName.c_str());
            ImGui::ProgressBar(operationProgress);
            ImGui::EndPopup();
        }
    }

    if (!errorMessage.empty()) {
        ImGui::OpenPopup("Error");
        if (ImGui::BeginPopupModal("Error", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Error: %s", errorMessage.c_str());
            if (ImGui::Button("OK", ImVec2(120, 0))) {
                errorMessage.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    if (!successMessage.empty()) {
        ImGui::OpenPopup("Éxito");
        if (ImGui::BeginPopupModal("Éxito", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("%s", successMessage.c_str());
            if (ImGui::Button("OK", ImVec2(120, 0))) {
                successMessage.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
}

void InterfaceGui::asyncRefreshFileList() {
    if (operationInProgress) return;

    operationInProgress = true;
    currentOperationName = "Actualizando lista";
    currentOperation = std::async(std::launch::async, [this]() {
        try {
            auto files = diskController.listAvailableFiles();
            std::lock_guard<std::mutex> lock(fileListMutex);
            fileList = std::move(files);
        } catch (const std::exception& e) {
            errorMessage = "Error al actualizar: " + std::string(e.what());
        }
        operationInProgress = false;
    });
}

void InterfaceGui::asyncDownloadFile(const std::string& filename) {
    std::filesystem::create_directory(DOWNLOADS_DIR);
    if (operationInProgress) return;

    operationInProgress = true;
    currentOperationName = "Descargando " + filename;
    currentOperation = std::async(std::launch::async, [this, filename]() {
        try {
            std::string savePath = std::string(DOWNLOADS_DIR) + "/" + filename;

            // Verificar directorio y permisos
            if (!std::filesystem::exists(DOWNLOADS_DIR)) {
                throw std::runtime_error("Directorio no existe: " + std::string(DOWNLOADS_DIR));
            }

            std::ofstream testFile(std::string(DOWNLOADS_DIR) + "/test.tmp");
            if (!testFile) throw std::runtime_error("Sin permisos en: " + std::string(DOWNLOADS_DIR));
            testFile.close();
            std::filesystem::remove(std::string(DOWNLOADS_DIR) + "/test.tmp");

            // Descargar y guardar
            auto fileData = diskController.retrieveFile(filename);
            std::ofstream out(savePath, std::ios::binary);
            if (!out) throw std::runtime_error("Error al crear: " + savePath);
            out.write(fileData.data(), fileData.size());

            // Actualizar mensaje de éxito (protegido por mutex)
            {
                std::lock_guard<std::mutex> lock(fileListMutex);
                successMessage = "Archivo guardado en:\n" + std::filesystem::absolute(savePath).string();
                errorMessage.clear(); // Limpiar error previo si existía
            }
        } catch (const std::exception& e) {
            // Actualizar mensaje de error (protegido por mutex)
            std::lock_guard<std::mutex> lock(fileListMutex);
            errorMessage = "Error: " + std::string(e.what());
            successMessage.clear();
        }
        operationInProgress = false; // Marcar operación como finalizada
    });
}

void InterfaceGui::asyncDeleteFile(const std::string& filename) {
    if (operationInProgress) return;

    operationInProgress = true;
    currentOperationName = "Eliminando " + filename;
    currentOperation = std::async(std::launch::async, [this, filename]() {
        try {
            bool deleted = diskController.deleteFile(filename);
            if (deleted) {
                diskController.saveMetadata();
                // Actualizar la lista de archivos
                auto files = diskController.listAvailableFiles();
                std::lock_guard<std::mutex> lock(fileListMutex);
                fileList = std::move(files);
                successMessage = "Archivo eliminado: " + filename;
            } else {
                errorMessage = "No se pudo eliminar: " + filename;
            }
        } catch (const std::exception& e) {
            errorMessage = "Error al eliminar: " + std::string(e.what());
        }
        operationInProgress = false;
    });
}


void InterfaceGui::openFileDialog() {
    const char* filters[] = { "*.pdf", "*.txt", "*.jpg", "*" };
    char* path = tinyfd_openFileDialog(
        "Seleccionar archivo",
        "",
        4,
        filters,
        "Todos los archivos",
        0
    );

    if (path) {
        strncpy(selectedFilePath, path, sizeof(selectedFilePath));
        selectedFilePath[sizeof(selectedFilePath) - 1] = '\0';
    }
}

void InterfaceGui::displayNodeStatus() {
    static std::future<std::vector<DiskController::NodeStatus>> statusFuture;
    static std::chrono::steady_clock::time_point lastUpdate;
    static std::vector<DiskController::NodeStatus> lastStatus;

    auto now = std::chrono::steady_clock::now();

    // Actualizar cada 2 segundos (ajustable)
    if (now - lastUpdate > std::chrono::seconds(2) || !statusFuture.valid()) {
        if (!statusFuture.valid() || statusFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            statusFuture = diskController.getNodesStatusAsync();
            lastUpdate = now;
        }
    }

    // Mostrar último estado disponible mientras se actualiza
    if (statusFuture.valid() && statusFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            lastStatus = statusFuture.get();
        } catch (...) {
            lastStatus.clear();
        }
        }

    if (!lastStatus.empty()) {
        for (const auto& node : lastStatus) {
            ImGui::Text("Nodo %d:", node.nodeId);
            ImGui::Text("- Puerto: %d", node.port);
            ImGui::Text("- Bloques: %d/%d", node.usedBlocks, node.totalBlocks);

            // Estado con color
            if (node.isActive) {
                ImGui::TextColored(ImVec4(0, 1, 0, 1), "- Estado: Activo");
            }
            else {
                ImGui::TextColored(ImVec4(1, 0, 0, 1), "- Estado: Inactivo");
            }

            float progress = node.totalBlocks > 0 ?
                static_cast<float>(node.usedBlocks) / node.totalBlocks : 0.0f;
            ImGui::ProgressBar(progress, ImVec2(100, 20));

            ImGui::Separator();
        }
    } else {
        ImGui::Text("Cargando estado de los nodos...");
    }
}

void InterfaceGui::cleanup() {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
}