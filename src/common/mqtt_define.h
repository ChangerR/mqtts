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
#define MQ_ERR_PUBLISH_NOT_AUTHORIZED -405

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
#define MQ_ERR_AUTH -708
#define MQ_ERR_DATABASE -709
#define MQ_ERR_AUTH_TOKEN_INVALID -710
#define MQ_ERR_AUTH_TOKEN_EXPIRED -711
#define MQ_ERR_AUTH_NONCE_REPLAY -712

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

// Router error codes (-900 to -919)
#define MQ_ERR_ROUTER -900
#define MQ_ERR_ROUTER_NOT_ENABLED -901
#define MQ_ERR_ROUTER_NOT_CONNECTED -902
#define MQ_ERR_ROUTER_AUTH_FAILED -903
#define MQ_ERR_ROUTER_SESSION_INVALID -904
#define MQ_ERR_ROUTER_SERVER_ID_MISMATCH -905
#define MQ_ERR_ROUTER_NODE_NOT_FOUND -906
#define MQ_ERR_ROUTER_NODE_INACTIVE -907
#define MQ_ERR_ROUTER_PROTOCOL -908
#define MQ_ERR_ROUTER_ROUTE_EMPTY -909
#define MQ_ERR_ROUTER_SNAPSHOT -910
#define MQ_ERR_ROUTER_REDO -911

// Forwarding error codes (-920 to -939)
#define MQ_ERR_FORWARD -920
#define MQ_ERR_FORWARD_CONNECT -921
#define MQ_ERR_FORWARD_SEND -922
#define MQ_ERR_FORWARD_RECV -923
#define MQ_ERR_FORWARD_LOOP -924
#define MQ_ERR_FORWARD_TARGET_INVALID -925

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
#define MQ_IS_ERR_ROUTER(code) (code <= MQ_ERR_ROUTER && code > MQ_ERR_ROUTER_REDO)
#define MQ_IS_ERR_FORWARD(code) (code <= MQ_ERR_FORWARD && code > MQ_ERR_FORWARD_TARGET_INVALID)

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
    case MQ_ERR_PUBLISH_NOT_AUTHORIZED:
      return "Not authorized to publish";

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
    case MQ_ERR_AUTH:
      return "Authentication failed";
    case MQ_ERR_DATABASE:
      return "Database operation failed";
    case MQ_ERR_AUTH_TOKEN_INVALID:
      return "Token is invalid";
    case MQ_ERR_AUTH_TOKEN_EXPIRED:
      return "Token has expired";
    case MQ_ERR_AUTH_NONCE_REPLAY:
      return "Token nonce replay detected";

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
    case MQ_ERR_ROUTER:
      return "Router error";
    case MQ_ERR_ROUTER_NOT_ENABLED:
      return "Router is not enabled";
    case MQ_ERR_ROUTER_NOT_CONNECTED:
      return "Router is not connected";
    case MQ_ERR_ROUTER_AUTH_FAILED:
      return "Router authentication failed";
    case MQ_ERR_ROUTER_SESSION_INVALID:
      return "Router session is invalid";
    case MQ_ERR_ROUTER_SERVER_ID_MISMATCH:
      return "Router server id mismatch";
    case MQ_ERR_ROUTER_NODE_NOT_FOUND:
      return "Router node not found";
    case MQ_ERR_ROUTER_NODE_INACTIVE:
      return "Router node is inactive";
    case MQ_ERR_ROUTER_PROTOCOL:
      return "Router protocol error";
    case MQ_ERR_ROUTER_ROUTE_EMPTY:
      return "Router route target is empty";
    case MQ_ERR_ROUTER_SNAPSHOT:
      return "Router snapshot error";
    case MQ_ERR_ROUTER_REDO:
      return "Router redo log error";
    case MQ_ERR_FORWARD:
      return "Forwarding error";
    case MQ_ERR_FORWARD_CONNECT:
      return "Forwarding connect error";
    case MQ_ERR_FORWARD_SEND:
      return "Forwarding send error";
    case MQ_ERR_FORWARD_RECV:
      return "Forwarding receive error";
    case MQ_ERR_FORWARD_LOOP:
      return "Forwarding loop detected";
    case MQ_ERR_FORWARD_TARGET_INVALID:
      return "Forwarding target is invalid";

    default:
      return "Unknown error";
  }
}
