#!/usr/bin/env python3

import subprocess
import os
import sys

def run_test(test_path):
    """Run a single test and return result"""
    try:
        result = subprocess.run([test_path], 
                              capture_output=True, 
                              text=True, 
                              timeout=30,
                              cwd=os.path.dirname(test_path))
        return result.returncode, result.stdout, result.stderr
    except subprocess.TimeoutExpired:
        return -1, "", "Test timed out"
    except Exception as e:
        return -2, "", str(e)

def main():
    build_dir = "/home/changer/projects/mqtts/build/unittest"
    
    # List of test executables to run
    tests = [
        "test_connect",
        "test_mqtt_parser", 
        "test_mqtt_allocator",
        "test_mqtt_router_service",
        "test_mqtt_router_rpc_client",
        "test_mqtt_session_manager_router"
    ]
    
    print("Running unittest suite...")
    print("=" * 50)
    
    passed = 0
    failed = 0
    
    for test in tests:
        test_path = os.path.join(build_dir, test)
        if not os.path.exists(test_path):
            print(f"SKIP {test}: Executable not found")
            continue
            
        print(f"Running {test}...")
        returncode, stdout, stderr = run_test(test_path)
        
        if returncode == 0:
            print(f"PASS {test}")
            passed += 1
        else:
            print(f"FAIL {test} (exit code: {returncode})")
            if stdout:
                print(f"STDOUT:\n{stdout[:500]}")
            if stderr:
                print(f"STDERR:\n{stderr[:500]}")
            failed += 1
        print("-" * 30)
    
    print(f"\nTest Summary:")
    print(f"Passed: {passed}")
    print(f"Failed: {failed}")
    print(f"Total: {passed + failed}")
    
    return 0 if failed == 0 else 1

if __name__ == "__main__":
    sys.exit(main())