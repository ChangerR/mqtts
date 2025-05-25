#include <iostream>
#include <unistd.h>
#include <thread>
#include "logger.h"
#include "mqtt_server.h"

static MQTTServer* g_server = nullptr;
static std::thread g_server_thread;

void server_thread_func(const char* ip, int port)
{
    g_server = new MQTTServer();
    if (!g_server) {
        LOG->error("Failed to create MQTT server");
        return;
    }

    LOG->info("Starting MQTT server on {}:{}", ip, port);
    
    int ret = g_server->start(ip, port);
    if (ret != MQ_SUCCESS) {
        LOG->error("Failed to start MQTT server, error code: {}", ret);
        delete g_server;
        g_server = nullptr;
        return;
    }

    // Start running the server
    g_server->run();

    // Cleanup
    if (g_server) {
        g_server->stop();
        delete g_server;
        g_server = nullptr;
    }

    LOG->info("MQTT server thread stopped");
}

int main(int argc, char* argv[])
{
    // Parse command line arguments
    const char* ip = "0.0.0.0";
    int port = 1883;
    
    int opt;
    while ((opt = getopt(argc, argv, "i:p:")) != -1) {
        switch (opt) {
            case 'i':
                ip = optarg;
                break;
            case 'p':
                port = atoi(optarg);
                break;
            default:
                LOG->error("Usage: {} [-i ip] [-p port]", argv[0]);
                return 1;
        }
    }

    // Start server in a separate thread
    g_server_thread = std::thread(server_thread_func, ip, port);

    // Wait for server thread to finish
    g_server_thread.join();

    LOG->info("MQTT server stopped");
    return 0;
}