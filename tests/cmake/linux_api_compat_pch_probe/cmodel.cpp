#include "consumer_header.h"

#include <array>

namespace {

using ProductionGRType = wave::array<unsigned int, 512>;

unsigned int consume_production_gr(const ProductionGRType& registers) {
    return registers[0] ^ registers[511];
}

unsigned int consume_standard_gr(const std::array<unsigned int, 512>& registers) {
    return registers[0] ^ registers[511];
}

} // namespace

int cmodel_probe() {
    ConsumerBusinessType value;
    // reflect_macro.h is already in cmodel's PCH before <array> is parsed.
    // This reproduces the production include order from the failing ISA call.
    std::array<unsigned int, 512> registers = {};
    registers[0] = 0x12u;
    registers[511] = 0x34u;
    ProductionGRType wave_registers = registers;
    return value.one == nullptr && value.many == nullptr &&
                   consume_production_gr(registers) == (0x12u ^ 0x34u) &&
                   consume_standard_gr(wave_registers) == (0x12u ^ 0x34u)
               ? 0
               : 1;
}
