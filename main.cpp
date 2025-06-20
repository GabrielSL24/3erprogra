#include "DiskController.h"
#include "InterfaceGUI.h"
#include <thread>
#include <iostream>
#include <csignal>
#include <atomic>
#include <chrono>

// Variable atomica para manejar la señal de interrupción
std::atomic<bool> shutdown_flag(false);

// Manejador de señales para Ctrl+C
void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        shutdown_flag = true;
    }
}

int main() {
    try {
        //Configura manejado de señales
        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);

        std::cout << "Iniciando TECMFS - Sistema de Archivos Distribuido RAID 5\n";

        //1.Inicializa controlador
        DiskController controller;
        std::cout << "Controlador RAID 5 inicializado correctamente\n";

        //2.Inicializa interfaz gráfica
        InterfaceGui gui(controller);
        std::cout << "Interfaz grafica inicializada correctamente\n";
        std::cout << "Interfaz disponible - Use Ctrl+C para salir\n";

        //3.Bucle principal optimizado
        auto last_status_update = std::chrono::steady_clock::now();
        constexpr auto status_update_interval = std::chrono::seconds(2);

        while (!shutdown_flag) {
            auto now = std::chrono::steady_clock::now();

            //Ejecuta la interfaz grafica, con límite de FPS
            gui.run();

            //Controla la frecuencia de actualizacion
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_status_update);
            if (elapsed < std::chrono::milliseconds(16)) {  //a 60 FPS
                std::this_thread::sleep_for(std::chrono::milliseconds(16) - elapsed);
            }

            last_status_update = now;
        }

        // 4.Limpieza
        std::cout << "\n Recibida señal de apagado...\n";
        std::cout << "Deteniendo componentes...\n";

        //Espera a que todas las operaciones asincronas completen
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        std::cout << "Aplicacion cerrada correctamente\n";
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "\n❌ ERROR FATAL: " << e.what() << std::endl;
        return 1;
    }
}
