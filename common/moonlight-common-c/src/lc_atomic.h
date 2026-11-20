#pragma once
// NyxPlay: <stdatomic.h> is not usable from C++20, which the host compiles these headers with.
// The headers may be included inside an extern "C" block, so the C++ library header is pulled in
// with C++ linkage.
#ifdef __cplusplus
extern "C++" {
#include <atomic>
}
using std::atomic_bool;
using std::atomic_int;
using std::atomic_uint;
using std::atomic_uint_fast32_t;
#else
#include <stdatomic.h>
#endif
