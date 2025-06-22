#pragma once
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "DiskController.h"
#include <GLFW/glfw3.h>         // Biblioteca para ventanas y manejo de input
#include <vector>
#include <string>
#include <filesystem>
#include <tinyfiledialogs/tinyfiledialogs.h>   // Para dialogos de archivo del sistema
#include <atomic>               // Para operaciones thread-safe
#include <future>               // Para operaciones asíncronas
#include <mutex>                // Para exclusion mutua (thread safety)

class DiskController;

class InterfaceGui {
public:
    explicit InterfaceGui(DiskController& controller);

    ~InterfaceGui();

    void run();

private:
    //Metodos de inicializacion y ciclo principal
    void initUI();
    void mainLoop();
    void update();
    void cleanup();

    //Funcionalidades de la interfaz
    void openFileDialog();
    void refreshFileList();
    void asyncRefreshFileList();
    void downloadFile(const std::string& filename);
    void asyncDownloadFile(const std::string& filename);
    void displayNodeStatus();       // Muestra estado de los nodos del RAID
    void showOperationStatus();
    void asyncDeleteFile(const std::string& filename);

    //Variables de estado
    GLFWwindow* window = nullptr;                   // Ventana principal
    char selectedFilePath[1024] = {0};              // Ruta del archivo seleccionado
    std::vector<std::string> fileList;              // Lista de archivos disponibles
    std::string errorMessage;
    std::string successMessage;

    //Control de operaciones asíncronas
    std::future<void> currentOperation;             // Operacion en segundo plano
    std::atomic<bool> operationInProgress{false};   // Indicador de operacion activa
    std::mutex fileListMutex;                       // Proteccion para fileList
    std::string currentOperationName;               // Nombre de la operacion actual
    float operationProgress = 0.0f;                 // Progreso (0.0 a 1.0)

    //Referencia al controlador, Acceso al diskcontroller
    DiskController& diskController;

    //Constantes de configuracion
    static constexpr int WINDOW_WIDTH = 1280;
    static constexpr int WINDOW_HEIGHT = 800;
    static constexpr const char* WINDOW_TITLE =
        "TECMFS - Sistema de Archivos Distribuido RAID 5";
    static constexpr const char* DOWNLOADS_DIR =
        "./Descargas";
};