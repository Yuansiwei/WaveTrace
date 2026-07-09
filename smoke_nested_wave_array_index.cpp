#include "reflect_macro.h"

#include <cstdint>
#include <iostream>

namespace {

int notify_count = 0;
std::size_t last_index = 0;
const void* last_type_tag = NULL;
std::size_t last_element_size = 0;

void reset_notify_state() {
    notify_count = 0;
    last_index = 0;
    last_type_tag = NULL;
    last_element_size = 0;
}

void index_notify(std::size_t index,
                  const void*,
                  const void* element_type_tag,
                  std::size_t element_size) {
    ++notify_count;
    last_index = index;
    last_type_tag = element_type_tag;
    last_element_size = element_size;
}

int fail(const char* message) {
    std::cerr << message << "\n";
    return 1;
}

}

int main() {
    wave::detail::set_wave_array_index_notify_fn(&index_notify);
    wave::detail::set_wave_array_bulk_notify_fn(NULL);
    wave::detail::set_wave_array_bulk_notify_epoch_fn(NULL);

    wave::array<wave::array<std::uint32_t, 2>, 2> nested;

    reset_notify_state();
    nested[0][1] = 0x12345678u;
    if (notify_count != 1) return fail("nested[0][1] should notify exactly one scalar element");
    if (last_index != 1) return fail("nested[0][1] should notify inner index 1");
    if (last_type_tag != reflect::type_tag_of<std::uint32_t>()) {
        return fail("nested[0][1] should notify uint32_t type, not row array type");
    }
    if (last_element_size != sizeof(std::uint32_t)) {
        return fail("nested[0][1] should notify uint32_t element size");
    }

    reset_notify_state();
    (void)nested[1];
    if (notify_count != 0) return fail("nested[1] row access should be transparent");

    reset_notify_state();
    nested[1].data()[0] = 0x87654321u;
    if (notify_count != 0) return fail("row data() should use bulk notify, not index notify");

    std::cout << "nested_wave_array_index_ok\n";
    return 0;
}
