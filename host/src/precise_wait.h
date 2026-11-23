// Short waits that really are short (see precise_wait.cpp).
#pragma once
#include <chrono>
#include <cstdint>
void PreciseWait(int64_t microseconds);
void PreciseSleepUntil(std::chrono::steady_clock::time_point t);   // same, until a point in time
