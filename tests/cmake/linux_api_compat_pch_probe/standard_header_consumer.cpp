#include <array>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <utility>

#include "reflect_macro.h"

namespace {

using ScreenshotGRType = wave::array<unsigned int, 512>;

struct ScreenshotInstrGroup {
    unsigned int first = 0;

    void setConfig(const ScreenshotGRType& registers) {
        first = registers[0];
    }
};

void accept_wave_array_reference(const ScreenshotGRType& registers,
                                 unsigned int& result) {
    result = registers[511];
}

} // namespace

int main() {
    const double value = std::sqrt(81.0);
    const int digit = std::isdigit(static_cast<unsigned char>('7'));
    const std::uint32_t width = 32u;

    // Regression for the production isa::InstrGroupTcUma error: std::array
    // must bind to an API taking const wave::array<T,N>& even though the Linux
    // compatibility header itself deliberately does not include <array>.
    std::array<unsigned int, 512> std_registers = {};
    std_registers[0] = 0x1234u;
    std_registers[511] = 0x5678u;
    ScreenshotInstrGroup group;
    group.setConfig(std_registers);
    unsigned int tail = 0;
    accept_wave_array_reference(std_registers, tail);

    wave::array<unsigned int, 512> wave_registers(std_registers);
    std::array<unsigned int, 512> replacement = {};
    replacement[0] = 0x9abcu;
    wave_registers = replacement;

    // Exercise the rest of the API surface which can otherwise fail in the
    // same way when Linux business code mixes std::array and wave::array.
    std::array<unsigned int, 512> round_trip = wave_registers;
    const std::array<unsigned int, 512>& snapshot_reference = wave_registers;
    wave::array<unsigned int, 3> ordered{{1u, 2u, 4u}};
    wave::array<unsigned int, 3> larger{{1u, 3u, 0u}};
    const bool ordered_ok = ordered < larger && larger > ordered &&
                            ordered <= larger && larger >= ordered &&
                            ordered != larger;
    wave::swap(ordered, larger);
    const bool iterator_ok = *ordered.rbegin() == ordered[2] &&
                             ordered.rend() - ordered.rbegin() == 3;
    const unsigned int moved_get = wave::get<0>(
        wave::array<unsigned int, 3>{{7u, 8u, 9u}});

    using StdMoveArray = std::array<std::unique_ptr<unsigned int>, 2>;
    using WaveMoveArray = wave::array<std::unique_ptr<unsigned int>, 2>;
    StdMoveArray move_source = {};
    move_source[0].reset(new unsigned int(0x55u));
    WaveMoveArray move_target(std::move(move_source));

    bool nested_ok = true;
#if !defined(_WIN32)
    // The lightweight surface recursively converts the inner arrays without
    // importing <array> into reflect_macro.h.
    wave::array<wave::array<unsigned int, 2>, 2> nested =
        std::array<std::array<unsigned int, 2>, 2>{{{{1u, 2u}}, {{3u, 4u}}}};
    std::array<std::array<unsigned int, 2>, 2> nested_round_trip = nested;
    nested_ok = nested_round_trip[1][1] == 4u;
#endif

    return value == 9.0 && digit != 0 && width == 32u &&
                   group.first == 0x1234u && tail == 0x5678u &&
                   wave_registers[0] == 0x9abcu &&
                   round_trip[0] == 0x9abcu &&
                   snapshot_reference[0] == 0x9abcu &&
                   ordered_ok && iterator_ok && moved_get == 7u &&
                   move_target[0] && *move_target[0] == 0x55u &&
                   nested_ok
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}
