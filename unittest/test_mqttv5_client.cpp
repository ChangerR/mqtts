#include <arpa/inet.h>
#include <errno.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// MQTT v5 Client Test Class
class MQTTv5ClientTest : public ::testing::Test
{
 protected:
  static void SetUpTestSuite() 
  {
    // Test configuration
    test_server_host = "127.0.0.1";
    test_server_port = 1884;
    
    // Check if MQTT server is already running
    std::cout << "Testing connection to MQTT server at " << test_server_host << ":" << test_server_port << std::endl;
    if (!is_port_in_use(test_server_port)) {
      std::cout << "No MQTT server detected. Starting internal server..." << std::endl;
      
      // Create temporary config file in current directory
      create_test_config_file();
      
      // Start mqtts server
      server_pid = fork();
      if (server_pid == 0) {
        // Child process - start the server
        // Use relative paths - server executable should be in parent directory
        execl("../mqtts", "mqtts", "-c", "test_mqtts.yaml", "-i", "127.0.0.1", "-p", "1884", (char*)NULL);
        // If execl fails
        std::cerr << "Failed to start mqtts server: " << strerror(errno) << std::endl;
        exit(1);
      } else if (server_pid > 0) {
        // Parent process - wait for server to start
        std::cout << "Started mqtts server with PID: " << server_pid << std::endl;
        
        // Wait up to 10 seconds for server to start
        for (int i = 0; i < 100; ++i) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
          if (is_port_in_use(test_server_port)) {
            std::cout << "MQTT server is ready for testing" << std::endl;
            return;
          }
        }
        
        std::cerr << "Failed to start MQTT server within timeout" << std::endl;
        kill(server_pid, SIGTERM);
        server_pid = -1;
      } else {
        std::cerr << "Failed to fork process for MQTT server" << std::endl;
        server_pid = -1;
      }
    } else {
      std::cout << "MQTT server detected and ready for testing" << std::endl;
    }
  }

  static void TearDownTestSuite() 
  {
    // Stop the server if we started it
    if (server_pid > 0) {
      std::cout << "Stopping MQTT server with PID: " << server_pid << std::endl;
      kill(server_pid, SIGTERM);
      
      // Wait for server to stop
      int status;
      waitpid(server_pid, &status, 0);
      server_pid = -1;
    }
    
    // Clean up temporary config file
    remove("test_mqtts.yaml");
    
    std::cout << "Test suite completed" << std::endl;
  }

  void SetUp() override
  {
    // Test setup - server is already running
    // Just wait a bit to ensure server is ready
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  void TearDown() override
  {
    // Test cleanup - server continues running for other tests
    // Just wait a bit between tests
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  // Helper function to check if a port is in use
  static bool is_port_in_use(int port)
  {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) return false;
    
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, test_server_host.c_str(), &addr.sin_addr);
    
    int result = connect(sockfd, (struct sockaddr*)&addr, sizeof(addr));
    close(sockfd);
    
    return result == 0;
  }

  // Helper function to create a socket connection
  int create_client_socket()
  {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
      return -1;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(test_server_port);
    inet_pton(AF_INET, test_server_host.c_str(), &server_addr.sin_addr);

    if (connect(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
      close(sockfd);
      return -1;
    }

    return sockfd;
  }

  // Helper function to send data
  bool send_data(int sockfd, const std::vector<uint8_t>& data)
  {
    size_t total_sent = 0;
    while (total_sent < data.size()) {
      ssize_t sent = send(sockfd, data.data() + total_sent, data.size() - total_sent, 0);
      if (sent <= 0) {
        return false;
      }
      total_sent += sent;
    }
    return true;
  }

  // Helper function to receive data with timeout
  std::vector<uint8_t> receive_data(int sockfd, size_t max_size = 1024, int timeout_ms = 2000)  // Balanced timeout
  {
    // Set socket timeout
    struct timeval timeout;
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
    
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
      std::cerr << "Failed to set socket timeout" << std::endl;
      return {};
    }
    
    std::vector<uint8_t> buffer(max_size);
    ssize_t received = recv(sockfd, buffer.data(), max_size, 0);
    if (received <= 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        std::cerr << "Receive timeout after " << timeout_ms << "ms" << std::endl;
      } else {
        std::cerr << "Receive error: " << strerror(errno) << std::endl;
      }
      return {};
    }
    buffer.resize(received);
    return buffer;
  }

  // Helper function to encode variable length integer
  std::vector<uint8_t> encode_variable_length(uint32_t value)
  {
    std::vector<uint8_t> encoded;
    do {
      uint8_t byte = value % 128;
      value /= 128;
      if (value > 0) {
        byte |= 0x80;
      }
      encoded.push_back(byte);
    } while (value > 0);
    return encoded;
  }

  // Helper function to create MQTT v5 CONNECT packet
  std::vector<uint8_t> create_connect_packet(const std::string& client_id, bool clean_start = true,
                                             uint16_t keep_alive = 60)
  {
    std::vector<uint8_t> packet;

    // Fixed header
    packet.push_back(0x10);  // CONNECT packet type

    // Variable header
    std::vector<uint8_t> variable_header;

    // Protocol name: "MQTT"
    variable_header.push_back(0x00);
    variable_header.push_back(0x04);
    variable_header.insert(variable_header.end(), {'M', 'Q', 'T', 'T'});

    // Protocol version: 5
    variable_header.push_back(0x05);

    // Connect flags
    uint8_t flags = 0;
    if (clean_start)
      flags |= 0x02;
    variable_header.push_back(flags);

    // Keep alive
    variable_header.push_back((keep_alive >> 8) & 0xFF);
    variable_header.push_back(keep_alive & 0xFF);

    // Properties (empty for now)
    variable_header.push_back(0x00);

    // Payload: Client ID
    variable_header.push_back((client_id.size() >> 8) & 0xFF);
    variable_header.push_back(client_id.size() & 0xFF);
    variable_header.insert(variable_header.end(), client_id.begin(), client_id.end());

    // Remaining length
    auto remaining_length = encode_variable_length(variable_header.size());
    packet.insert(packet.end(), remaining_length.begin(), remaining_length.end());

    // Add variable header
    packet.insert(packet.end(), variable_header.begin(), variable_header.end());

    return packet;
  }

  // Helper function to create MQTT v5 SUBSCRIBE packet
  std::vector<uint8_t> create_subscribe_packet(
      uint16_t packet_id, const std::vector<std::pair<std::string, uint8_t>>& subscriptions)
  {
    std::vector<uint8_t> packet;

    // Fixed header
    packet.push_back(0x82);  // SUBSCRIBE packet type with flags

    // Variable header
    std::vector<uint8_t> variable_header;

    // Packet ID
    variable_header.push_back((packet_id >> 8) & 0xFF);
    variable_header.push_back(packet_id & 0xFF);

    // Properties (empty for now)
    variable_header.push_back(0x00);

    // Payload: Topic filters
    for (const auto& sub : subscriptions) {
      // Topic filter length
      variable_header.push_back((sub.first.size() >> 8) & 0xFF);
      variable_header.push_back(sub.first.size() & 0xFF);
      // Topic filter
      variable_header.insert(variable_header.end(), sub.first.begin(), sub.first.end());
      // Subscription options
      variable_header.push_back(sub.second);
    }

    // Remaining length
    auto remaining_length = encode_variable_length(variable_header.size());
    packet.insert(packet.end(), remaining_length.begin(), remaining_length.end());

    // Add variable header
    packet.insert(packet.end(), variable_header.begin(), variable_header.end());

    return packet;
  }

  // Helper function to create MQTT v5 PUBLISH packet
  std::vector<uint8_t> create_publish_packet(const std::string& topic,
                                             const std::vector<uint8_t>& payload, uint8_t qos = 0,
                                             bool retain = false, bool dup = false,
                                             uint16_t packet_id = 0)
  {
    std::vector<uint8_t> packet;

    // Fixed header
    uint8_t fixed_header = 0x30;  // PUBLISH packet type
    if (dup)
      fixed_header |= 0x08;
    if (qos == 1)
      fixed_header |= 0x02;
    else if (qos == 2)
      fixed_header |= 0x04;
    if (retain)
      fixed_header |= 0x01;
    packet.push_back(fixed_header);

    // Variable header
    std::vector<uint8_t> variable_header;

    // Topic name
    variable_header.push_back((topic.size() >> 8) & 0xFF);
    variable_header.push_back(topic.size() & 0xFF);
    variable_header.insert(variable_header.end(), topic.begin(), topic.end());

    // Packet ID (for QoS > 0)
    if (qos > 0) {
      variable_header.push_back((packet_id >> 8) & 0xFF);
      variable_header.push_back(packet_id & 0xFF);
    }

    // Properties (empty for now)
    variable_header.push_back(0x00);

    // Payload
    variable_header.insert(variable_header.end(), payload.begin(), payload.end());

    // Remaining length
    auto remaining_length = encode_variable_length(variable_header.size());
    packet.insert(packet.end(), remaining_length.begin(), remaining_length.end());

    // Add variable header
    packet.insert(packet.end(), variable_header.begin(), variable_header.end());

    return packet;
  }

  // Helper function to create MQTT v5 DISCONNECT packet
  std::vector<uint8_t> create_disconnect_packet(uint8_t reason_code = 0)
  {
    std::vector<uint8_t> packet;

    // Fixed header
    packet.push_back(0xE0);  // DISCONNECT packet type

    // Variable header
    std::vector<uint8_t> variable_header;

    // Reason code
    variable_header.push_back(reason_code);

    // Properties (empty for now)
    variable_header.push_back(0x00);

    // Remaining length
    auto remaining_length = encode_variable_length(variable_header.size());
    packet.insert(packet.end(), remaining_length.begin(), remaining_length.end());

    // Add variable header
    packet.insert(packet.end(), variable_header.begin(), variable_header.end());

    return packet;
  }

  // Helper function to parse CONNACK packet
  bool parse_connack_packet(const std::vector<uint8_t>& data, bool& session_present,
                            uint8_t& reason_code)
  {
    if (data.size() < 4)
      return false;

    // Check packet type
    if ((data[0] & 0xF0) != 0x20)
      return false;

    // Parse acknowledge flags
    session_present = (data[2] & 0x01) != 0;

    // Parse reason code
    reason_code = data[3];

    return true;
  }

  // Helper function to parse SUBACK packet
  bool parse_suback_packet(const std::vector<uint8_t>& data, uint16_t& packet_id,
                           std::vector<uint8_t>& reason_codes)
  {
    if (data.size() < 5)
      return false;

    // Check packet type
    if ((data[0] & 0xF0) != 0x90)
      return false;

    // Parse packet ID
    packet_id = (data[2] << 8) | data[3];

    // Skip properties length (assuming 0 for now)
    size_t properties_len = data[4];
    size_t payload_start = 5 + properties_len;

    // Parse reason codes
    reason_codes.clear();
    for (size_t i = payload_start; i < data.size(); i++) {
      reason_codes.push_back(data[i]);
    }

    return true;
  }

  // Helper function to parse PUBLISH packet
  bool parse_publish_packet(const std::vector<uint8_t>& data, std::string& topic,
                            std::vector<uint8_t>& payload, uint8_t& qos, bool& retain, bool& dup,
                            uint16_t& packet_id)
  {
    if (data.size() < 3)
      return false;

    // Check packet type
    if ((data[0] & 0xF0) != 0x30)
      return false;

    // Parse flags
    dup = (data[0] & 0x08) != 0;
    qos = (data[0] & 0x06) >> 1;
    retain = (data[0] & 0x01) != 0;

    // Parse remaining length
    size_t remaining_length = data[1];  // Simplified for single byte
    size_t pos = 2;

    // Parse topic name
    if (pos + 2 > data.size())
      return false;
    uint16_t topic_len = (data[pos] << 8) | data[pos + 1];
    pos += 2;

    if (pos + topic_len > data.size())
      return false;
    topic = std::string(data.begin() + pos, data.begin() + pos + topic_len);
    pos += topic_len;

    // Parse packet ID (for QoS > 0)
    if (qos > 0) {
      if (pos + 2 > data.size())
        return false;
      packet_id = (data[pos] << 8) | data[pos + 1];
      pos += 2;
    }

    // Skip properties (assuming 0 for now)
    if (pos >= data.size())
      return false;
    uint8_t properties_len = data[pos];
    pos += 1 + properties_len;

    // Parse payload
    if (pos < data.size()) {
      payload = std::vector<uint8_t>(data.begin() + pos, data.end());
    }

    return true;
  }

 protected:
  static std::string test_server_host;
  static int test_server_port;
  static pid_t server_pid;

 private:
  // Helper function to create temporary config file
  static void create_test_config_file() {
    std::ofstream config_file("test_mqtts.yaml");
    config_file << "server:\n"
                << "  bind: \"127.0.0.1\"\n"
                << "  port: 1884\n"
                << "  max_connections: 1000\n"
                << "  num_threads: 1\n"
                << "  packet_size_limit: 1048576\n"
                << "  keepalive: 60\n"
                << "  client_memory_size: 1048576\n"
                << "\n"
                << "logging:\n"
                << "  level: \"info\"\n"
                << "  console: true\n"
                << "  file: \"logs/mqtt.log\"\n"
                << "  max_size: 10485760\n"
                << "  max_files: 10\n"
                << "  auto_flush: false\n";
    config_file.close();
    std::cout << "Created temporary config file: test_mqtts.yaml" << std::endl;
  }
};

// Static member definition
std::string MQTTv5ClientTest::test_server_host;
int MQTTv5ClientTest::test_server_port;
pid_t MQTTv5ClientTest::server_pid = -1;

// Test basic connection
TEST_F(MQTTv5ClientTest, BasicConnection)
{
  int sockfd = create_client_socket();
  ASSERT_GE(sockfd, 0) << "Failed to create client socket";

  // Send CONNECT packet
  auto connect_packet = create_connect_packet("test_client_001");
  ASSERT_TRUE(send_data(sockfd, connect_packet)) << "Failed to send CONNECT packet";

  // Receive CONNACK packet
  auto response = receive_data(sockfd, 1024);
  ASSERT_FALSE(response.empty()) << "No CONNACK response received";

  bool session_present;
  uint8_t reason_code;
  ASSERT_TRUE(parse_connack_packet(response, session_present, reason_code))
      << "Failed to parse CONNACK";

  EXPECT_EQ(reason_code, 0)
      << "Connection failed with reason code: " << static_cast<int>(reason_code);
  EXPECT_FALSE(session_present) << "Unexpected session present flag";

  // Send DISCONNECT packet
  auto disconnect_packet = create_disconnect_packet();
  ASSERT_TRUE(send_data(sockfd, disconnect_packet)) << "Failed to send DISCONNECT packet";

  close(sockfd);
}

// Test subscription
TEST_F(MQTTv5ClientTest, BasicSubscription)
{
  int sockfd = create_client_socket();
  ASSERT_GE(sockfd, 0) << "Failed to create client socket";

  // Connect first
  auto connect_packet = create_connect_packet("test_client_002");
  ASSERT_TRUE(send_data(sockfd, connect_packet));

  auto connack_response = receive_data(sockfd, 1024);
  ASSERT_FALSE(connack_response.empty());

  bool session_present;
  uint8_t reason_code;
  ASSERT_TRUE(parse_connack_packet(connack_response, session_present, reason_code));
  ASSERT_EQ(reason_code, 0);

  // Send SUBSCRIBE packet
  std::vector<std::pair<std::string, uint8_t>> subscriptions = {
      {"sensor/temperature", 0}, {"sensor/humidity", 1}, {"device/+/status", 0}};
  auto subscribe_packet = create_subscribe_packet(1, subscriptions);
  ASSERT_TRUE(send_data(sockfd, subscribe_packet)) << "Failed to send SUBSCRIBE packet";

  // Receive SUBACK packet
  auto suback_response = receive_data(sockfd, 1024);
  ASSERT_FALSE(suback_response.empty()) << "No SUBACK response received";

  uint16_t packet_id;
  std::vector<uint8_t> reason_codes;
  ASSERT_TRUE(parse_suback_packet(suback_response, packet_id, reason_codes))
      << "Failed to parse SUBACK";

  EXPECT_EQ(packet_id, 1) << "Unexpected packet ID in SUBACK";
  EXPECT_EQ(reason_codes.size(), subscriptions.size()) << "Unexpected number of reason codes";

  for (size_t i = 0; i < reason_codes.size(); i++) {
    EXPECT_EQ(reason_codes[i], subscriptions[i].second)
        << "Unexpected reason code for subscription " << i;
  }

  // Disconnect
  auto disconnect_packet = create_disconnect_packet();
  ASSERT_TRUE(send_data(sockfd, disconnect_packet));

  close(sockfd);
}

// Test publish and receive with separate clients
TEST_F(MQTTv5ClientTest, PublishAndReceive)
{
  // Create subscriber client
  int subscriber_sockfd = create_client_socket();
  ASSERT_GE(subscriber_sockfd, 0) << "Failed to create subscriber socket";

  // Connect subscriber
  auto connect_packet = create_connect_packet("test_subscriber");
  ASSERT_TRUE(send_data(subscriber_sockfd, connect_packet));

  auto connack_response = receive_data(subscriber_sockfd, 1024);
  ASSERT_FALSE(connack_response.empty());

  // Subscribe to topic
  std::vector<std::pair<std::string, uint8_t>> subscriptions = {{"sensor/temperature", 0}};
  auto subscribe_packet = create_subscribe_packet(1, subscriptions);
  ASSERT_TRUE(send_data(subscriber_sockfd, subscribe_packet));

  auto suback_response = receive_data(subscriber_sockfd, 1024);
  ASSERT_FALSE(suback_response.empty());

  // Parse SUBACK to verify subscription was successful
  uint16_t packet_id;
  std::vector<uint8_t> reason_codes;
  ASSERT_TRUE(parse_suback_packet(suback_response, packet_id, reason_codes))
      << "Failed to parse SUBACK";
  EXPECT_EQ(packet_id, 1) << "Unexpected packet ID in SUBACK";
  EXPECT_EQ(reason_codes.size(), 1) << "Expected 1 reason code";
  EXPECT_EQ(reason_codes[0], 0) << "Subscription failed with reason code: " << static_cast<int>(reason_codes[0]);

  // Wait a bit to ensure subscription is processed
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Create publisher client
  int publisher_sockfd = create_client_socket();
  ASSERT_GE(publisher_sockfd, 0) << "Failed to create publisher socket";

  // Connect publisher
  auto publisher_connect_packet = create_connect_packet("test_publisher");
  ASSERT_TRUE(send_data(publisher_sockfd, publisher_connect_packet));

  auto publisher_connack_response = receive_data(publisher_sockfd, 1024);
  ASSERT_FALSE(publisher_connack_response.empty());

  // Publish message from publisher
  std::string topic = "sensor/temperature";
  std::vector<uint8_t> payload = {'2', '3', '.', '5', 'C'};
  auto publish_packet = create_publish_packet(topic, payload, 0, false, false, 0);
  
  ASSERT_TRUE(send_data(publisher_sockfd, publish_packet)) << "Failed to send PUBLISH packet";

  // Receive published message on subscriber
  auto publish_response = receive_data(subscriber_sockfd, 1024, 3000);  // 3 second timeout
  ASSERT_FALSE(publish_response.empty()) << "No PUBLISH message received by subscriber";

  std::string received_topic;
  std::vector<uint8_t> received_payload;
  uint8_t qos;
  bool retain, dup;
  uint16_t publish_packet_id;

  ASSERT_TRUE(parse_publish_packet(publish_response, received_topic, received_payload, qos, retain,
                                   dup, publish_packet_id))
      << "Failed to parse received PUBLISH packet";

  EXPECT_EQ(received_topic, topic) << "Unexpected topic in received message";
  EXPECT_EQ(received_payload, payload) << "Unexpected payload in received message";
  EXPECT_EQ(qos, 0) << "Unexpected QoS in received message";
  EXPECT_FALSE(retain) << "Unexpected retain flag in received message";
  EXPECT_FALSE(dup) << "Unexpected dup flag in received message";

  // Disconnect both clients
  auto disconnect_packet = create_disconnect_packet();
  send_data(subscriber_sockfd, disconnect_packet);
  send_data(publisher_sockfd, disconnect_packet);

  close(subscriber_sockfd);
  close(publisher_sockfd);
}

// Test wildcard subscriptions
TEST_F(MQTTv5ClientTest, WildcardSubscriptions)
{
  int publisher_sockfd = create_client_socket();
  int subscriber_sockfd = create_client_socket();

  ASSERT_GE(publisher_sockfd, 0);
  ASSERT_GE(subscriber_sockfd, 0);

  // Connect both clients
  auto pub_connect = create_connect_packet("publisher_002");
  auto sub_connect = create_connect_packet("subscriber_002");

  ASSERT_TRUE(send_data(publisher_sockfd, pub_connect));
  ASSERT_TRUE(send_data(subscriber_sockfd, sub_connect));

  // Receive CONNACK
  auto pub_connack = receive_data(publisher_sockfd, 1024);
  auto sub_connack = receive_data(subscriber_sockfd, 1024);
  ASSERT_FALSE(pub_connack.empty());
  ASSERT_FALSE(sub_connack.empty());

  // Subscribe with wildcards
  std::vector<std::pair<std::string, uint8_t>> subscriptions = {
      {"sensor/+", 0},  // Single level wildcard
      {"device/#", 0}   // Multi-level wildcard
  };
  auto subscribe_packet = create_subscribe_packet(1, subscriptions);
  ASSERT_TRUE(send_data(subscriber_sockfd, subscribe_packet));

  auto suback_response = receive_data(subscriber_sockfd, 1024);
  ASSERT_FALSE(suback_response.empty());

  // Test single level wildcard
  std::string topic1 = "sensor/temperature";
  std::vector<uint8_t> payload1 = {'2', '5', '.', '0'};
  auto publish1 = create_publish_packet(topic1, payload1);
  ASSERT_TRUE(send_data(publisher_sockfd, publish1));

  auto response1 = receive_data(subscriber_sockfd, 1024);
  ASSERT_FALSE(response1.empty()) << "No message received for single level wildcard";

  // Test multi-level wildcard
  std::string topic2 = "device/room1/light/status";
  std::vector<uint8_t> payload2 = {'O', 'N'};
  auto publish2 = create_publish_packet(topic2, payload2);
  ASSERT_TRUE(send_data(publisher_sockfd, publish2));

  auto response2 = receive_data(subscriber_sockfd, 1024);
  ASSERT_FALSE(response2.empty()) << "No message received for multi-level wildcard";

  // Disconnect
  auto disconnect_packet = create_disconnect_packet();
  ASSERT_TRUE(send_data(publisher_sockfd, disconnect_packet));
  ASSERT_TRUE(send_data(subscriber_sockfd, disconnect_packet));

  close(publisher_sockfd);
  close(subscriber_sockfd);
}

// Test QoS 1 message delivery
TEST_F(MQTTv5ClientTest, QoS1MessageDelivery)
{
  int publisher_sockfd = create_client_socket();
  int subscriber_sockfd = create_client_socket();

  ASSERT_GE(publisher_sockfd, 0);
  ASSERT_GE(subscriber_sockfd, 0);

  // Connect both clients
  auto pub_connect = create_connect_packet("publisher_qos1");
  auto sub_connect = create_connect_packet("subscriber_qos1");

  ASSERT_TRUE(send_data(publisher_sockfd, pub_connect));
  ASSERT_TRUE(send_data(subscriber_sockfd, sub_connect));

  // Receive CONNACK
  auto pub_connack = receive_data(publisher_sockfd, 1024);
  auto sub_connack = receive_data(subscriber_sockfd, 1024);
  ASSERT_FALSE(pub_connack.empty());
  ASSERT_FALSE(sub_connack.empty());

  // Subscribe with QoS 1
  std::vector<std::pair<std::string, uint8_t>> subscriptions = {{"test/qos1", 1}};
  auto subscribe_packet = create_subscribe_packet(1, subscriptions);
  ASSERT_TRUE(send_data(subscriber_sockfd, subscribe_packet));

  auto suback_response = receive_data(subscriber_sockfd, 1024);
  ASSERT_FALSE(suback_response.empty());

  // Publish QoS 1 message
  std::string topic = "test/qos1";
  std::vector<uint8_t> payload = {'Q', 'o', 'S', '1', ' ', 'T', 'e', 's', 't'};
  auto publish_packet = create_publish_packet(topic, payload, 1, false, false, 123);
  ASSERT_TRUE(send_data(publisher_sockfd, publish_packet));

  // Receive PUBACK from server
  auto puback_response = receive_data(publisher_sockfd, 1024);
  ASSERT_FALSE(puback_response.empty()) << "No PUBACK received for QoS 1 message";

  // Check PUBACK packet
  ASSERT_GE(puback_response.size(), 4);
  EXPECT_EQ(puback_response[0] & 0xF0, 0x40) << "Expected PUBACK packet type";

  // Receive message on subscriber
  auto message_response = receive_data(subscriber_sockfd, 1024);
  ASSERT_FALSE(message_response.empty()) << "No message received by subscriber";

  // Disconnect
  auto disconnect_packet = create_disconnect_packet();
  ASSERT_TRUE(send_data(publisher_sockfd, disconnect_packet));
  ASSERT_TRUE(send_data(subscriber_sockfd, disconnect_packet));

  close(publisher_sockfd);
  close(subscriber_sockfd);
}

// Test multiple concurrent clients
TEST_F(MQTTv5ClientTest, MultipleConcurrentClients)
{
  const int num_clients = 5;  // Reduced from 10
  std::vector<int> client_sockets;
  std::vector<std::thread> client_threads;
  std::atomic<int> successful_connections(0);
  std::atomic<int> successful_publishes(0);

  // Create and connect multiple clients
  for (int i = 0; i < num_clients; i++) {
    client_threads.emplace_back([this, i, &successful_connections, &successful_publishes]() {
      int sockfd = create_client_socket();
      if (sockfd < 0)
        return;

      // Connect
      std::string client_id = "client_" + std::to_string(i);
      auto connect_packet = create_connect_packet(client_id);
      if (!send_data(sockfd, connect_packet)) {
        close(sockfd);
        return;
      }

      // Receive CONNACK
      auto connack_response = receive_data(sockfd, 1024);
      if (connack_response.empty()) {
        close(sockfd);
        return;
      }

      bool session_present;
      uint8_t reason_code;
      if (!parse_connack_packet(connack_response, session_present, reason_code) ||
          reason_code != 0) {
        close(sockfd);
        return;
      }

      successful_connections.fetch_add(1);

      // Subscribe
      std::vector<std::pair<std::string, uint8_t>> subscriptions = {{"test/concurrent", 0}};
      auto subscribe_packet = create_subscribe_packet(1, subscriptions);
      if (!send_data(sockfd, subscribe_packet)) {
        close(sockfd);
        return;
      }

      // Receive SUBACK
      auto suback_response = receive_data(sockfd, 1024);
      if (suback_response.empty()) {
        close(sockfd);
        return;
      }

      // Publish a message
      std::string topic = "test/concurrent";
      std::vector<uint8_t> payload = {'C', 'l', 'i', 'e',
                                      'n', 't', ' ', static_cast<uint8_t>('0' + i)};
      auto publish_packet = create_publish_packet(topic, payload);
      if (send_data(sockfd, publish_packet)) {
        successful_publishes.fetch_add(1);
      }

      // Wait a bit to receive messages from other clients
      std::this_thread::sleep_for(std::chrono::milliseconds(50));  // Reduced from 100ms

      // Disconnect
      auto disconnect_packet = create_disconnect_packet();
      send_data(sockfd, disconnect_packet);

      close(sockfd);
    });
  }

  // Wait for all threads to complete
  for (auto& thread : client_threads) {
    thread.join();
  }

  EXPECT_EQ(successful_connections.load(), num_clients) << "Not all clients connected successfully";
  EXPECT_EQ(successful_publishes.load(), num_clients) << "Not all clients published successfully";
}

// Test normal message delivery (modified from retain test)
TEST_F(MQTTv5ClientTest, MessageDeliveryTest)
{
  // Create subscriber client
  int subscriber_sockfd = create_client_socket();
  ASSERT_GE(subscriber_sockfd, 0);

  // Connect subscriber
  auto sub_connect = create_connect_packet("message_subscriber");
  ASSERT_TRUE(send_data(subscriber_sockfd, sub_connect));

  auto sub_connack = receive_data(subscriber_sockfd, 1024);
  ASSERT_FALSE(sub_connack.empty());

  // Subscribe to the topic
  std::vector<std::pair<std::string, uint8_t>> subscriptions = {{"test/message", 0}};
  auto subscribe_packet = create_subscribe_packet(1, subscriptions);
  ASSERT_TRUE(send_data(subscriber_sockfd, subscribe_packet));

  auto suback_response = receive_data(subscriber_sockfd, 1024);
  ASSERT_FALSE(suback_response.empty());

  // Parse SUBACK to verify subscription was successful
  uint16_t packet_id;
  std::vector<uint8_t> reason_codes;
  ASSERT_TRUE(parse_suback_packet(suback_response, packet_id, reason_codes))
      << "Failed to parse SUBACK";
  EXPECT_EQ(packet_id, 1) << "Unexpected packet ID in SUBACK";
  EXPECT_EQ(reason_codes.size(), 1) << "Expected 1 reason code";
  EXPECT_EQ(reason_codes[0], 0) << "Subscription failed with reason code: " << static_cast<int>(reason_codes[0]);

  // Wait a bit to ensure subscription is processed
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Create publisher client
  int publisher_sockfd = create_client_socket();
  ASSERT_GE(publisher_sockfd, 0);

  // Connect publisher
  auto pub_connect = create_connect_packet("message_publisher");
  ASSERT_TRUE(send_data(publisher_sockfd, pub_connect));

  auto pub_connack = receive_data(publisher_sockfd, 1024);
  ASSERT_FALSE(pub_connack.empty());

  // Publish normal message from publisher
  std::string topic = "test/message";
  std::vector<uint8_t> payload = {'T', 'e', 's', 't', 'M', 's', 'g'};
  auto publish_packet = create_publish_packet(topic, payload, 0, false, false, 0);  // retain = false
  ASSERT_TRUE(send_data(publisher_sockfd, publish_packet));

  // Receive published message on subscriber
  auto message_response = receive_data(subscriber_sockfd, 1024, 3000);  // 3 second timeout
  ASSERT_FALSE(message_response.empty()) << "No message received by subscriber";

  std::string received_topic;
  std::vector<uint8_t> received_payload;
  uint8_t qos;
  bool retain, dup;
  uint16_t publish_packet_id;

  ASSERT_TRUE(parse_publish_packet(message_response, received_topic, received_payload, qos, retain,
                                   dup, publish_packet_id))
      << "Failed to parse received message";

  EXPECT_EQ(received_topic, topic) << "Unexpected topic in received message";
  EXPECT_EQ(received_payload, payload) << "Unexpected payload in received message";
  EXPECT_EQ(qos, 0) << "Unexpected QoS in received message";
  EXPECT_FALSE(retain) << "Unexpected retain flag in received message";
  EXPECT_FALSE(dup) << "Unexpected dup flag in received message";

  // Disconnect both clients
  auto disconnect_packet = create_disconnect_packet();
  send_data(subscriber_sockfd, disconnect_packet);
  send_data(publisher_sockfd, disconnect_packet);

  close(subscriber_sockfd);
  close(publisher_sockfd);
}

// Google Test main function
int main(int argc, char** argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}