#pragma once
#include <vector>
#include "edgeflow/event.hpp"

namespace edgeflow {

class Sink {
public:
    virtual ~Sink() = default;
    virtual void consume(const std::vector<Event>& batch) = 0;
};

} // namespace edgeflow
