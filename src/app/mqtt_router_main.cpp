#include "mqtt_router_service.h"
#include "mqtt_config.h"
#include <spdlog/spdlog.h>
#include <signal.h>
#include <iostream>
#include <yaml-cpp/yaml.h>

static std::unique_ptr<MQTTRouterService> g_router_service;

void signal_handler(int signal) {
    spdlog::info("Received signal {}, shutting down router service", signal);
    if (g_router_service) {
        g_router_service->stop();
    }
}

int load_router_config(const std::string& config_file, MQTTRouterConfig& config) {
  int __mq_ret = 0;
  do {
      try {
          YAML::Node yaml_config = YAML::LoadFile(config_file);
          
          if (yaml_config["router"]) {
              auto router_node = yaml_config["router"];
              
              if (router_node["service_host"]) {
                  config.service_host = router_node["service_host"].as<std::string>();
              }
              
              if (router_node["service_port"]) {
                  config.service_port = router_node["service_port"].as<int>();
              }
              
              if (router_node["redo_log_path"]) {
                  config.redo_log_path = router_node["redo_log_path"].as<std::string>();
              }
              
              if (router_node["snapshot_path"]) {
                  config.snapshot_path = router_node["snapshot_path"].as<std::string>();
              }
              
              if (router_node["snapshot_interval_seconds"]) {
                  config.snapshot_interval_seconds = router_node["snapshot_interval_seconds"].as<int>();
              }
              
              if (router_node["redo_log_flush_interval_ms"]) {
                  config.redo_log_flush_interval_ms = router_node["redo_log_flush_interval_ms"].as<int>();
              }
              
              if (router_node["max_redo_log_entries"]) {
                  config.max_redo_log_entries = router_node["max_redo_log_entries"].as<int>();
              }
              
              if (router_node["worker_thread_count"]) {
                  config.worker_thread_count = router_node["worker_thread_count"].as<int>();
              }
              
              if (router_node["coroutines_per_thread"]) {
                  config.coroutines_per_thread = router_node["coroutines_per_thread"].as<int>();
              }
              
              if (router_node["max_memory_limit"]) {
                  config.max_memory_limit = router_node["max_memory_limit"].as<size_t>();
              }
          }
          
          __mq_ret = 0;
          break;
      } catch (const std::exception& e) {
          spdlog::error("Failed to load router config: {}", e.what());
          __mq_ret = -1;
          break;
      }
  } while (false);

  return __mq_ret;
}

void print_usage(const char* program_name) {
    std::cout << "Usage: " << program_name << " [options]\n"
              << "Options:\n"
              << "  -c, --config <file>    Configuration file path (default: mqtt_router.yaml)\n"
              << "  -h, --help            Show this help message\n"
              << "  -v, --version         Show version information\n";
}

int main(int argc, char* argv[]) {
  int __mq_ret = 0;
  do {
      std::string config_file = "mqtt_router.yaml";
      
      for (int i = 1; i < argc; ++i) {
          std::string arg = argv[i];
          
          if (arg == "-c" || arg == "--config") {
              if (i + 1 < argc) {
                  config_file = argv[++i];
              } else {
                  std::cerr << "Error: Config file path required after " << arg << "\n";
                  __mq_ret = 1;
                  break;
              }
          } else if (arg == "-h" || arg == "--help") {
              print_usage(argv[0]);
              __mq_ret = 0;
              break;
          } else if (arg == "-v" || arg == "--version") {
              std::cout << "MQTT Router Service v1.0.0\n";
              __mq_ret = 0;
              break;
          } else {
              std::cerr << "Error: Unknown option " << arg << "\n";
              print_usage(argv[0]);
              __mq_ret = 1;
              break;
          }
      }
      
      signal(SIGINT, signal_handler);
      signal(SIGTERM, signal_handler);
      
      spdlog::set_level(spdlog::level::info);
      spdlog::info("Starting MQTT Router Service");
      
      MQTTRouterConfig config;
      if (load_router_config(config_file, config) != 0) {
          spdlog::error("Failed to load configuration from {}", config_file);
          __mq_ret = 1;
          break;
      }
      
      spdlog::info("Router configuration:");
      spdlog::info("  Service host: {}", config.service_host);
      spdlog::info("  Service port: {}", config.service_port);
      spdlog::info("  Redo log path: {}", config.redo_log_path);
      spdlog::info("  Snapshot path: {}", config.snapshot_path);
      spdlog::info("  Worker threads: {}", config.worker_thread_count);
      spdlog::info("  Memory limit: {} MB", config.max_memory_limit / (1024 * 1024));
      
      g_router_service.reset(new MQTTRouterService(config));
      
      if (g_router_service->initialize() != 0) {
          spdlog::error("Failed to initialize router service");
          __mq_ret = 1;
          break;
      }
      
      if (g_router_service->start() != 0) {
          spdlog::error("Failed to start router service");
          __mq_ret = 1;
          break;
      }
      
      spdlog::info("MQTT Router Service is running. Press Ctrl+C to stop.");
      
      while (true) {
          std::this_thread::sleep_for(std::chrono::seconds(10));
          
          size_t total_servers, total_clients, total_subscriptions;
          if (g_router_service->get_statistics(total_servers, total_clients, total_subscriptions) == 0) {
              spdlog::info("Statistics: {} servers, {} clients, {} subscriptions", 
                          total_servers, total_clients, total_subscriptions);
          }
      }
      
      __mq_ret = 0;
      break;
  } while (false);

  return __mq_ret;
}
