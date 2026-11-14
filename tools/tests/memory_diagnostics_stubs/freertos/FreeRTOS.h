#pragma once
#include <atomic>
// 私有 ESP-IDF 临界区自旋锁的测试实现。
// 采用静态存储，绝不构造动态互斥锁或分配内存。
struct portMUX_TYPE { std::atomic_flag flag; };
#define portMUX_INITIALIZER_UNLOCKED { ATOMIC_FLAG_INIT }
void memoryTestEnterCritical(portMUX_TYPE *);
void memoryTestExitCritical(portMUX_TYPE *);
#define portENTER_CRITICAL_SAFE(lock) memoryTestEnterCritical(lock)
#define portEXIT_CRITICAL_SAFE(lock) memoryTestExitCritical(lock)
