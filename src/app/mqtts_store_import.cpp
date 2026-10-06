#include <sys/stat.h>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include "mqtt_append_log.h"
#include "mqtt_durable_store.h"

int main(int argc, char** argv)
{
  if (argc != 4) {
    std::fprintf(stderr, "Usage: mqtts-store-import EXPORT NEW_DIRECTORY PARTITIONS\n");
    return 2;
  }
  try {
    mqtt::PersistenceConfig config;
    config.enabled = true;
    config.path = argv[2];
    std::string partitions = argv[3];
    size_t parsed = 0;
    config.partitions = std::stoul(partitions, &parsed);
    if (parsed != partitions.size() || !config.partitions || config.partitions > 32)
      throw std::runtime_error("invalid partition count");
    // Exclusive directory creation rejects an existing store, including an empty one.
    if (mkdir(config.path.c_str(), 0700) != 0)
      throw std::runtime_error("destination must not exist and its parent must be writable");
    auto exported = mqtt::journal::read_file(argv[1], size_t(config.max_disk_bytes / 8));
    mqtt::DurableStore store(config);
    store.import_legacy(exported);
    auto state = store.statistics();
    std::printf("Imported %zu sessions, %zu pending deliveries, %zu logical bytes\n",
                state.sessions, state.pending, state.bytes);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "Migration refused: %s\n", e.what());
    return 1;
  }
}
