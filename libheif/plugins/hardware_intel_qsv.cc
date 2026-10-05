#include "hardware_intel_qsv.h"
#include <shared_mutex>

static std::mutex IntelQSVMutex;

IntelQSVLockGuard::IntelQSVLockGuard() {
    IntelQSVMutex.lock();
}

IntelQSVLockGuard::~IntelQSVLockGuard() {
    IntelQSVMutex.unlock();
}