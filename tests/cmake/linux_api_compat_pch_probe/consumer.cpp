#include "consumer_header.h"

int main() {
    ConsumerBusinessType value;
    PeekBusinessType peek_source;
    DynamicBusinessType dynamic_source;
    return value.one == nullptr && value.many == nullptr &&
                   value.values[1] == 2 && value.scalar.read() == 3 &&
                   peek_source.wave_trace_peek_ptr() == &peek_source.sampled &&
                   peek_source.wave_trace_peek_byte_width() == sizeof(int) &&
                   peek_source.wave_trace_peek_dynamic_expander() == nullptr &&
                   peek_source.wave_trace_peek_dirty_hook() != nullptr &&
                   dynamic_source.wave_trace_target_ptr() == &dynamic_source &&
                   dynamic_source.wave_trace_target_byte_width() == sizeof(DynamicBusinessType) &&
                   dynamic_source.wave_trace_dynamic_expander() == nullptr &&
                   dynamic_source.wave_trace_dirty_hook() != nullptr
               ? 0
               : 1;
}
