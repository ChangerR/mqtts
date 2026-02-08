#include <iostream>
#include <cstdlib>

int main() {
    std::cout << "Testing basic functionality..." << std::endl;
    
    // Test if we can execute the test binaries
    std::cout << "Attempting to run test_connect..." << std::endl;
    int result = system("cd /home/changer/projects/mqtts/build/unittest && timeout 10 ./test_connect 2>&1");
    std::cout << "test_connect exit code: " << WEXITSTATUS(result) << std::endl;
    
    std::cout << "Attempting to run test_mqtt_parser..." << std::endl;
    result = system("cd /home/changer/projects/mqtts/build/unittest && timeout 10 ./test_mqtt_parser 2>&1");
    std::cout << "test_mqtt_parser exit code: " << WEXITSTATUS(result) << std::endl;
    
    std::cout << "Attempting to run test_mqtt_router_service..." << std::endl;
    result = system("cd /home/changer/projects/mqtts/build/unittest && timeout 10 ./test_mqtt_router_service 2>&1");
    std::cout << "test_mqtt_router_service exit code: " << WEXITSTATUS(result) << std::endl;
    
    return 0;
}