#ifndef COROUTINE_TEST_HELPER_H
#define COROUTINE_TEST_HELPER_H

#include "../3rd/libco/co_routine.h"
#include "../3rd/libco/co_comm.h"
#include <memory>
#include <functional>
#include <atomic>

namespace mqtt {
namespace test {

/**
 * @brief 协程测试辅助类，用于在测试环境中正确初始化和运行协程代码
 */
class CoroutineTestHelper {
public:
    CoroutineTestHelper() : main_co_(nullptr), test_co_(nullptr) {
        // 启用系统调用hook，这对协程正常工作很重要
        co_enable_hook_sys();
        
        // 获取当前线程的epoll上下文，这会初始化协程环境
        epoll_ctx_ = co_get_epoll_ct();
    }
    
    ~CoroutineTestHelper() {
        if (test_co_) {
            co_release(test_co_);
            test_co_ = nullptr;
        }
        co_disable_hook_sys();
    }
    
    /**
     * @brief 在协程环境中运行测试函数
     * @param test_func 要运行的测试函数
     * @return true 如果测试成功运行，false 如果无法创建协程
     */
    bool run_in_coroutine(std::function<void()> test_func) {
        test_function_ = test_func;
        
        // 创建测试协程
        int ret = co_create(&test_co_, nullptr, &CoroutineTestHelper::coroutine_entry, this);
        if (ret != 0) {
            return false;
        }
        
        // 启动协程
        co_resume(test_co_);
        
        return true;
    }
    
    /**
     * @brief 检查当前环境是否支持协程
     * @return true 如果支持协程，false 如果不支持
     */
    static bool is_coroutine_supported() {
        // 尝试获取epoll上下文来测试协程是否可用
        stCoEpoll_t* ctx = co_get_epoll_ct();
        return ctx != nullptr;
    }
    
    /**
     * @brief 简单的协程yield操作，用于测试协程切换
     */
    void yield() {
        co_yield_ct();
    }

private:
    static void* coroutine_entry(void* arg) {
        CoroutineTestHelper* helper = static_cast<CoroutineTestHelper*>(arg);
        if (helper && helper->test_function_) {
            helper->test_function_();
        }
        return nullptr;
    }
    
    stCoRoutine_t* main_co_;
    stCoRoutine_t* test_co_;
    stCoEpoll_t* epoll_ctx_;
    std::function<void()> test_function_;
};

/**
 * @brief RAII包装器，用于在测试中安全地使用协程环境
 */
class CoroutineTestScope {
public:
    CoroutineTestScope() : epoll_ctx_(nullptr), dummy_co_(nullptr) {
        // 启用协程环境
        co_enable_hook_sys();
        
        // 获取epoll上下文
        epoll_ctx_ = co_get_epoll_ct();
        
        // 创建一个简单的dummy协程来初始化环境
        int ret = co_create(&dummy_co_, nullptr, &dummy_routine, nullptr);
        if (ret == 0 && dummy_co_) {
            // 启动并立即完成协程
            co_resume(dummy_co_);
        }
    }
    
    ~CoroutineTestScope() {
        // 清理dummy协程
        if (dummy_co_) {
            co_release(dummy_co_);
            dummy_co_ = nullptr;
        }
        
        // 清理协程环境
        co_disable_hook_sys();
    }
    
    bool is_available() const {
        return epoll_ctx_ != nullptr && dummy_co_ != nullptr;
    }
    
private:
    static void* dummy_routine(void* arg) {
        // 什么都不做，只是为了初始化协程环境
        return nullptr;
    }
    
    stCoEpoll_t* epoll_ctx_;
    stCoRoutine_t* dummy_co_;
};

} // namespace test
} // namespace mqtt

#endif // COROUTINE_TEST_HELPER_H