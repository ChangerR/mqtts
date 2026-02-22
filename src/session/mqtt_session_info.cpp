#include "mqtt_session_info.h"
#include "mqtt_protocol_handler.h"

namespace mqtt {

//==============================================================================
// SafeHandlerRef实现 - 安全的handler引用包装器
//==============================================================================

SafeHandlerRef::SafeHandlerRef(SessionInfo* info) : session_info_(info), is_acquired_(false)
{
  if (info && info->is_valid.load()) {
    acquire();
  }
}

SafeHandlerRef::~SafeHandlerRef()
{
  internal_release();
}

SafeHandlerRef::SafeHandlerRef(SafeHandlerRef&& other) noexcept
    : handler_(other.handler_), session_info_(other.session_info_), is_acquired_(other.is_acquired_)
{
  // 转移所有权
  other.handler_ = nullptr;
  other.session_info_ = nullptr;
  other.is_acquired_ = false;
}

SafeHandlerRef& SafeHandlerRef::operator=(SafeHandlerRef&& other) noexcept
{
  if (this != &other) {
    // 释放当前引用
    internal_release();

    // 转移所有权
    handler_ = other.handler_;
    session_info_ = other.session_info_;
    is_acquired_ = other.is_acquired_;

    other.handler_ = nullptr;
    other.session_info_ = nullptr;
    other.is_acquired_ = false;
  }
  return *this;
}

void SafeHandlerRef::acquire()
{
  if (session_info_ && session_info_->is_valid.load() && !session_info_->pending_removal.load()) {
    // 原子增加引用计数
    session_info_->ref_count.fetch_add(1);
    handler_ = session_info_->handler;
    is_acquired_ = true;
  }
}

void SafeHandlerRef::release()
{
  internal_release();
}

void SafeHandlerRef::internal_release()
{
  if (is_acquired_ && session_info_) {
    // 原子减少引用计数
    int old_count = session_info_->ref_count.fetch_sub(1);
    if (old_count == 1) {
      // 最后一个引用，通知等待的协程
      session_info_->notify_zero_refs();
    }

    handler_ = nullptr;
    session_info_ = nullptr;
    is_acquired_ = false;
  }
}

}  // namespace mqtt