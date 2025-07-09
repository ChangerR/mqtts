#ifndef MQTT_MEMORY_TAGS_H
#define MQTT_MEMORY_TAGS_H

#include <string>

// 定义所有内存标签
#define MQTT_MEMORY_TAGS(DEFINE_MEMORY_TAG)   \
  DEFINE_MEMORY_TAG(MEM_TAG_ROOT, "ROOT")     \
  DEFINE_MEMORY_TAG(MEM_TAG_SOCKET, "SOCKET") \
  DEFINE_MEMORY_TAG(MEM_TAG_CLIENT, "CLIENT") \
  DEFINE_MEMORY_TAG(MEM_TAG_SESSION_MANAGER, "SESSION_MANAGER") \
  DEFINE_MEMORY_TAG(MEM_TAG_TOPIC_TREE, "TOPIC_TREE")

// 生成枚举
enum class MQTTMemoryTag {
#define DEFINE_MEMORY_TAG(tag, str) tag,
  MQTT_MEMORY_TAGS(DEFINE_MEMORY_TAG)
#undef DEFINE_MEMORY_TAG
      MEM_TAG_COUNT  // 用于表示枚举数量
};

// 获取标签对应的字符串描述
inline const std::string& get_memory_tag_str(MQTTMemoryTag tag)
{
  static const std::string tag_strings[] = {
#define DEFINE_MEMORY_TAG(tag, str) str,
      MQTT_MEMORY_TAGS(DEFINE_MEMORY_TAG)
#undef DEFINE_MEMORY_TAG
          "Unknown"  // 对应 COUNT
  };

  size_t index = static_cast<size_t>(tag);
  if (index < static_cast<size_t>(MQTTMemoryTag::MEM_TAG_COUNT)) {
    return tag_strings[index];
  }
  return tag_strings[static_cast<size_t>(MQTTMemoryTag::MEM_TAG_COUNT)];
}

#endif  // MQTT_MEMORY_TAGS_H