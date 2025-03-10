#pragma once

#include <cstdint>

namespace kuzu {
namespace common {

class Reader {
public:
    virtual void read(uint8_t* data, uint64_t size) = 0;
    virtual ~Reader() = default;

    virtual bool finished() = 0;
};

} // namespace common
} // namespace kuzu
