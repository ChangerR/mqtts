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

#define MQ_LIKELY(x)       __builtin_expect(!!(x),!!1)
#define MQ_UNLIKELY(x)     __builtin_expect(!!(x),!!0)

// Helper macros
#define MQ_SUCC(arg) (MQ_LIKELY(MQ_SUCCESS == (ret = (arg))))
#define MQ_FAIL(arg) (MQ_UNLIKELY(MQ_SUCCESS != (ret = (arg))))

#define MQ_ISNULL(arg) (MQ_UNLIKELY(NULL == (arg)))
#define MQ_NOT_NULL(arg) (MQ_LIKELY(NULL != (arg)))

// Error code check macros
#define MQ_IS_ERR_SOCKET(code) (code <= MQ_ERR_SOCKET && code > MQ_ERR_SOCKET_LISTEN)
