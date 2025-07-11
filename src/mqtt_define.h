#pragma once

// Common return codes
#define MQ_SUCCESS 0

// Socket error codes (-1 to -99)
#define MQ_ERR_SOCKET -1
#define MQ_ERR_SOCKET_ALLOC -2
#define MQ_ERR_SOCKET_BIND -3
#define MQ_ERR_SOCKET_LISTEN -4
#define MQ_ERR_SOCKET_ACCEPT -5
#define MQ_ERR_SOCKET_CONNECT -6
#define MQ_ERR_SOCKET_SEND -7
#define MQ_ERR_SOCKET_RECV -8
#define MQ_ERR_SOCKET_ACCEPT_WOULDBLOCK -9

// Memory error codes (-100 to -199)
#define MQ_ERR_MEMORY_ALLOC -100
#define MQ_ERR_MEMORY_LIMIT -101
#define MQ_ERR_MEMORY_PARENT_LIMIT -102

// MQTT Protocol error codes (-200 to -299)
#define MQ_ERR_PROTOCOL -200
#define MQ_ERR_PACKET_TOO_LARGE -201
#define MQ_ERR_PACKET_INVALID -202
#define MQ_ERR_PACKET_INCOMPLETE -203
#define MQ_ERR_PACKET_TYPE -204
#define MQ_ERR_PACKET_ID -205
#define MQ_ERR_PACKET_QOS -206

// MQTT Connect error codes (-300 to -399)
#define MQ_ERR_CONNECT -300
#define MQ_ERR_CONNECT_PROTOCOL -301
#define MQ_ERR_CONNECT_CLIENT_ID -302
#define MQ_ERR_CONNECT_CREDENTIALS -303
#define MQ_ERR_CONNECT_NOT_AUTHORIZED -304
#define MQ_ERR_CONNECT_SERVER_UNAVAILABLE -305

// MQTT Publish error codes (-400 to -499)
#define MQ_ERR_PUBLISH -400
#define MQ_ERR_PUBLISH_TOPIC -401
#define MQ_ERR_PUBLISH_PAYLOAD -402
#define MQ_ERR_PUBLISH_QOS -403
#define MQ_ERR_PUBLISH_RETAIN -404

// MQTT Subscribe error codes (-500 to -599)
#define MQ_ERR_SUBSCRIBE -500
#define MQ_ERR_SUBSCRIBE_TOPIC -501
#define MQ_ERR_SUBSCRIBE_QOS -502
#define MQ_ERR_SUBSCRIBE_NOT_AUTHORIZED -503

// MQTT Session error codes (-600 to -699)
#define MQ_ERR_SESSION -600
#define MQ_ERR_SESSION_NOT_CONNECTED -601
#define MQ_ERR_SESSION_ALREADY_CONNECTED -602
#define MQ_ERR_SESSION_EXPIRED -603
#define MQ_ERR_SESSION_REGISTER -604
#define MQ_ERR_SESSION_UNREGISTER -605
#define MQ_ERR_SESSION_INVALID_HANDLER -606
#define MQ_ERR_SESSION_THREAD_MISMATCH -607
#define MQ_ERR_SESSION_MANAGER_NOT_READY -608

// 通用错误
#define MQ_ERR_INVALID_ARGS -700
#define MQ_ERR_TIMEOUT -701
#define MQ_ERR_INTERNAL -702
#define MQ_ERR_PARAM_V2 -703
#define MQ_ERR_NOT_FOUND_V2 -704
#define MQ_ERR_TIMEOUT_V2 -705
#define MQ_ERR_QUEUE_FULL -706
#define MQ_ERR_INVALID_STATE -707

// Topic Tree error codes (-800 to -899)
#define MQ_ERR_TOPIC_TREE -800
#define MQ_ERR_TOPIC_TREE_INVALID_TOPIC -801
#define MQ_ERR_TOPIC_TREE_INVALID_CLIENT -802
#define MQ_ERR_TOPIC_TREE_NODE_CREATE -803
#define MQ_ERR_TOPIC_TREE_MEMORY_ALLOC -804
#define MQ_ERR_TOPIC_TREE_CONCURRENT_MODIFY -805
#define MQ_ERR_TOPIC_TREE_SUBSCRIBER_EXISTS -806
#define MQ_ERR_TOPIC_TREE_SUBSCRIBER_NOT_FOUND -807
#define MQ_ERR_TOPIC_TREE_CONCURRENT -808

#define MQ_LIKELY(x) __builtin_expect(!!(x), !!1)
#define MQ_UNLIKELY(x) __builtin_expect(!!(x), !!0)

// Helper macros
#define MQ_SUCC(arg) (MQ_LIKELY(MQ_SUCCESS == (ret = (arg))))
#define MQ_FAIL(arg) (MQ_UNLIKELY(MQ_SUCCESS != (ret = (arg))))

#define MQ_ISNULL(arg) (MQ_UNLIKELY(NULL == (arg)))
#define MQ_NOT_NULL(arg) (MQ_LIKELY(NULL != (arg)))

// Error code check macros
#define MQ_IS_ERR_SOCKET(code) (code <= MQ_ERR_SOCKET && code > MQ_ERR_SOCKET_LISTEN)
#define MQ_IS_ERR_PROTOCOL(code) (code <= MQ_ERR_PROTOCOL && code > MQ_ERR_PACKET_QOS)
#define MQ_IS_ERR_CONNECT(code) (code <= MQ_ERR_CONNECT && code > MQ_ERR_CONNECT_SERVER_UNAVAILABLE)
#define MQ_IS_ERR_PUBLISH(code) (code <= MQ_ERR_PUBLISH && code > MQ_ERR_PUBLISH_RETAIN)
#define MQ_IS_ERR_SUBSCRIBE(code) \
  (code <= MQ_ERR_SUBSCRIBE && code > MQ_ERR_SUBSCRIBE_NOT_AUTHORIZED)
#define MQ_IS_ERR_SESSION(code) (code <= MQ_ERR_SESSION && code > MQ_ERR_SESSION_MANAGER_NOT_READY)
#define MQ_IS_ERR_TOPIC_TREE(code) \
  (code <= MQ_ERR_TOPIC_TREE && code > MQ_ERR_TOPIC_TREE_SUBSCRIBER_NOT_FOUND)

// Error code to string conversion
static inline const char* mqtt_error_string(int error_code)
{
  switch (error_code) {
    case MQ_SUCCESS:
      return "Success";

    // Socket errors
    case MQ_ERR_SOCKET:
      return "Socket error";
    case MQ_ERR_SOCKET_ALLOC:
      return "Socket allocation error";
    case MQ_ERR_SOCKET_BIND:
      return "Socket bind error";
    case MQ_ERR_SOCKET_LISTEN:
      return "Socket listen error";
    case MQ_ERR_SOCKET_ACCEPT:
      return "Socket accept error";
    case MQ_ERR_SOCKET_CONNECT:
      return "Socket connect error";
    case MQ_ERR_SOCKET_SEND:
      return "Socket send error";
    case MQ_ERR_SOCKET_RECV:
      return "Socket receive error";
    case MQ_ERR_SOCKET_ACCEPT_WOULDBLOCK:
      return "Socket accept would block";

    // Memory errors
    case MQ_ERR_MEMORY_ALLOC:
      return "Memory allocation error";
    case MQ_ERR_MEMORY_LIMIT:
      return "Memory limit exceeded";
    case MQ_ERR_MEMORY_PARENT_LIMIT:
      return "Parent memory limit exceeded";

    // Protocol errors
    case MQ_ERR_PROTOCOL:
      return "Protocol error";
    case MQ_ERR_PACKET_TOO_LARGE:
      return "Packet too large";
    case MQ_ERR_PACKET_INVALID:
      return "Invalid packet format";
    case MQ_ERR_PACKET_INCOMPLETE:
      return "Incomplete packet";
    case MQ_ERR_PACKET_TYPE:
      return "Invalid packet type";
    case MQ_ERR_PACKET_ID:
      return "Invalid packet ID";
    case MQ_ERR_PACKET_QOS:
      return "Invalid QoS level";

    // Connect errors
    case MQ_ERR_CONNECT:
      return "Connect error";
    case MQ_ERR_CONNECT_PROTOCOL:
      return "Unsupported protocol version";
    case MQ_ERR_CONNECT_CLIENT_ID:
      return "Invalid client ID";
    case MQ_ERR_CONNECT_CREDENTIALS:
      return "Invalid credentials";
    case MQ_ERR_CONNECT_NOT_AUTHORIZED:
      return "Not authorized";
    case MQ_ERR_CONNECT_SERVER_UNAVAILABLE:
      return "Server unavailable";

    // Publish errors
    case MQ_ERR_PUBLISH:
      return "Publish error";
    case MQ_ERR_PUBLISH_TOPIC:
      return "Invalid topic";
    case MQ_ERR_PUBLISH_PAYLOAD:
      return "Invalid payload";
    case MQ_ERR_PUBLISH_QOS:
      return "Invalid QoS";
    case MQ_ERR_PUBLISH_RETAIN:
      return "Invalid retain flag";

    // Subscribe errors
    case MQ_ERR_SUBSCRIBE:
      return "Subscribe error";
    case MQ_ERR_SUBSCRIBE_TOPIC:
      return "Invalid topic filter";
    case MQ_ERR_SUBSCRIBE_QOS:
      return "Invalid QoS";
    case MQ_ERR_SUBSCRIBE_NOT_AUTHORIZED:
      return "Not authorized to subscribe";

    // Session errors
    case MQ_ERR_SESSION:
      return "Session error";
    case MQ_ERR_SESSION_NOT_CONNECTED:
      return "Not connected";
    case MQ_ERR_SESSION_ALREADY_CONNECTED:
      return "Already connected";
    case MQ_ERR_SESSION_EXPIRED:
      return "Session expired";
    case MQ_ERR_SESSION_REGISTER:
      return "Failed to register session";
    case MQ_ERR_SESSION_UNREGISTER:
      return "Failed to unregister session";
    case MQ_ERR_SESSION_INVALID_HANDLER:
      return "Invalid session handler";
    case MQ_ERR_SESSION_THREAD_MISMATCH:
      return "Session thread mismatch";
    case MQ_ERR_SESSION_MANAGER_NOT_READY:
      return "Session manager not ready";

    // 通用错误
    case MQ_ERR_INVALID_ARGS:
      return "Invalid arguments";
    case MQ_ERR_TIMEOUT:
      return "Operation timeout";
    case MQ_ERR_PARAM_V2:
      return "Invalid parameter";
    case MQ_ERR_NOT_FOUND_V2:
      return "Resource not found";
    case MQ_ERR_TIMEOUT_V2:
      return "Operation timeout";

    // Topic tree errors
    case MQ_ERR_TOPIC_TREE:
      return "Topic tree error";
    case MQ_ERR_TOPIC_TREE_INVALID_TOPIC:
      return "Invalid topic format";
    case MQ_ERR_TOPIC_TREE_INVALID_CLIENT:
      return "Invalid client ID";
    case MQ_ERR_TOPIC_TREE_NODE_CREATE:
      return "Failed to create topic tree node";
    case MQ_ERR_TOPIC_TREE_MEMORY_ALLOC:
      return "Topic tree memory allocation failed";
    case MQ_ERR_TOPIC_TREE_CONCURRENT_MODIFY:
      return "Concurrent modification conflict";
    case MQ_ERR_TOPIC_TREE_SUBSCRIBER_EXISTS:
      return "Subscriber already exists";
    case MQ_ERR_TOPIC_TREE_SUBSCRIBER_NOT_FOUND:
      return "Subscriber not found";

    default:
      return "Unknown error";
  }
}
