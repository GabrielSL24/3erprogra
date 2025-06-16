#include "DiskController.h"
#include "Client.h"
#include "InterfaceGUI.h"
#include <thread>
#include <iostream>

int main() {
    // Crear hilo para el servidor
    std::thread server_thread([]() {
        DiskController server;
        });

    // Ejecutar cliente en el hilo principal
    Client client;

    InterfaceGui interfaz;
    interfaz.run();

    // Esperar a que el hilo del servidor termine (aunque no lo hará normalmente)
    server_thread.join();

    return 0;
}