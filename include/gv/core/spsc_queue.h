#pragma once

#include <rigtorp/SPSCQueue.h>

namespace gv::core {

// Single-producer single-consumer bounded ring buffer. Lock-free, wait-free
// per the rigtorp implementation. Used between pipeline stages (capture →
// vision → ocr → parse → ui) where each stage has exactly one producer and
// one consumer thread.
template <typename T>
using SpscQueue = rigtorp::SPSCQueue<T>;

} // namespace gv::core
