#include <wvz4_writer_typed.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <queue>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::uint32_t kSignalCount = 32u;
constexpr std::uint64_t kWriterTicksPerBusinessCycle = 10u;

void add_bit_signal(wvz4::Layout& layout,
                    std::uint32_t signal_index,
                    const std::string& name) {
    const std::uint32_t node_id = signal_index + 2u;
    const std::uint32_t name_id = signal_index + 2u;
    wvz4::add_layout_name_blob_record(
        layout, name_id, name.data(), static_cast<std::uint32_t>(name.size()));

    wvz4::NodeRecord node;
    node.node_id = node_id;
    node.parent_id = 1u;
    node.name_id = name_id;
    node.kind = wvz4::NodeKind::SignalLeaf;
    node.next_sibling = signal_index + 1u < kSignalCount ? node_id + 1u : 0u;
    layout.nodes.push_back(node);

    wvz4::SignalDefinition signal;
    signal.signal_id = signal_index + 1u;
    signal.node_id = node_id;
    signal.type = wvz4::ValueType::Bool;
    signal.bit_width = 1u;
    signal.radix = wvz4::Radix::Bin;
    layout.signals.push_back(signal);
}

bool append_bit(wvz4::CycleSubmission& submission,
                std::uint32_t signal_id,
                bool value) {
    const std::uint8_t raw = value ? 1u : 0u;
    return submission.append_grouped_raw(1u, signal_id, &raw);
}

struct OneShot {
    std::uint64_t cycle = 0;
    std::uint32_t signal_id = 0;
    bool value = false;
};

struct Periodic {
    std::uint32_t signal_id = 0;
    std::uint64_t period = 1;
    std::uint64_t next = 1;
    std::uint64_t end = 0;
    bool value = false;
};

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: bit_state_big_writer <output.wvz4>\n";
        return 2;
    }

    constexpr std::uint64_t kBusinessCycles = 10000000ull;
    constexpr std::uint64_t kDenseCycles = 2000000ull;
    wvz4::Layout layout;
    wvz4::add_layout_name_blob_record(layout, 1u, "top", 3u);

    wvz4::NodeRecord root;
    root.node_id = 1u;
    root.name_id = 1u;
    root.kind = wvz4::NodeKind::Root;
    root.first_child = 2u;
    layout.nodes.push_back(root);

    const std::vector<std::string> fixedNames = {
        "zero_then_one_at_20_percent",
        "one_then_zero_at_20_percent",
        "always_zero",
        "always_one",
        "two_long_high_pulses",
        "slow_toggle_every_100k",
        "medium_toggle_every_1k",
        "every_cycle_first_2m_then_one",
        "zero_then_one_at_cycle_1"
    };
    for (std::uint32_t i = 0; i < fixedNames.size(); ++i) {
        add_bit_signal(layout, i, fixedNames[i]);
    }

    const std::uint64_t periods[] = {
        10ull, 20ull, 50ull, 100ull, 200ull, 500ull,
        1000ull, 2000ull, 5000ull, 10000ull, 20000ull,
        50000ull, 100000ull, 200000ull, 500000ull, 1000000ull,
        125ull, 1250ull, 12500ull, 125000ull, 250000ull, 750000ull, 1500000ull
    };
    for (std::uint32_t i = 9u; i < kSignalCount; ++i) {
        const std::uint64_t period = periods[i - 9u];
        add_bit_signal(layout, i, "periodic_toggle_" + std::to_string(period));
    }

    wvz4::WriterOptions options;
    options.compression = wvz4::Compression::Zstd;
    options.zstd_level = 1;
    options.enable_block_pipeline = true;
    options.enable_lod_tables = true;
    options.lod_bucket_cycle_scale = 1u;
    options.target_block_span = 4096u;

    std::string error;
    wvz4::Writer writer;
    if (!writer.open(argv[1], std::move(layout), options, error)) {
        std::cerr << error << '\n';
        return 3;
    }

    wvz4::CycleSubmission initial;
    initial.cycle = 0;
    for (std::uint32_t sid = 1u; sid <= kSignalCount; ++sid) {
        const bool value = sid == 2u || sid == 4u;
        if (!append_bit(initial, sid, value)) return 4;
    }
    if (!writer.submit_cycle(initial, error)) {
        std::cerr << error << '\n';
        return 5;
    }

    std::vector<OneShot> oneShots = {
        { 1ull, 9u, true },
        { 1500000ull, 5u, true },
        { 2000000ull, 1u, true },
        { 2000000ull, 2u, false },
        { 2500000ull, 5u, false },
        { 5500000ull, 5u, true },
        { 6500000ull, 5u, false }
    };
    std::sort(oneShots.begin(), oneShots.end(), [](const OneShot& a, const OneShot& b) {
        if (a.cycle != b.cycle) return a.cycle < b.cycle;
        return a.signal_id < b.signal_id;
    });

    std::vector<Periodic> periodic;
    periodic.push_back(Periodic{ 6u, 100000ull, 100000ull, kBusinessCycles, false });
    periodic.push_back(Periodic{ 7u, 1000ull, 1000ull, kBusinessCycles, false });
    periodic.push_back(Periodic{ 8u, 1ull, 1ull, kDenseCycles, false });
    for (std::uint32_t sid = 10u; sid <= kSignalCount; ++sid) {
        const std::uint64_t period = periods[sid - 10u];
        periodic.push_back(Periodic{ sid, period, period, kBusinessCycles, false });
    }

    using QueueItem = std::pair<std::uint64_t, std::size_t>;
    std::priority_queue<QueueItem, std::vector<QueueItem>, std::greater<QueueItem>> queue;
    for (std::size_t i = 0; i < periodic.size(); ++i) queue.push(QueueItem(periodic[i].next, i));

    std::size_t oneShotIndex = 0;
    std::uint64_t submissionCount = 1;
    std::uint64_t updateCount = kSignalCount;
    while (oneShotIndex < oneShots.size() || !queue.empty()) {
        const std::uint64_t nextOneShot = oneShotIndex < oneShots.size()
            ? oneShots[oneShotIndex].cycle
            : (std::numeric_limits<std::uint64_t>::max)();
        const std::uint64_t nextPeriodic = queue.empty()
            ? (std::numeric_limits<std::uint64_t>::max)()
            : queue.top().first;
        const std::uint64_t businessCycle = (std::min)(nextOneShot, nextPeriodic);
        if (businessCycle >= kBusinessCycles) break;

        wvz4::CycleSubmission submission;
        submission.cycle = static_cast<wvz4::i64>(businessCycle * kWriterTicksPerBusinessCycle);
        while (oneShotIndex < oneShots.size() && oneShots[oneShotIndex].cycle == businessCycle) {
            const OneShot& event = oneShots[oneShotIndex++];
            if (!append_bit(submission, event.signal_id, event.value)) return 4;
            ++updateCount;
        }
        while (!queue.empty() && queue.top().first == businessCycle) {
            const std::size_t index = queue.top().second;
            queue.pop();
            Periodic& pattern = periodic[index];
            pattern.value = !pattern.value;
            if (!append_bit(submission, pattern.signal_id, pattern.value)) return 4;
            ++updateCount;
            if (pattern.next <= pattern.end - pattern.period) {
                pattern.next += pattern.period;
                queue.push(QueueItem(pattern.next, index));
            }
        }
        if (!writer.submit_cycle(submission, error)) {
            std::cerr << error << '\n';
            return 5;
        }
        ++submissionCount;
    }

    wvz4::CycleSubmission finalSubmission;
    finalSubmission.cycle = static_cast<wvz4::i64>(
        (kBusinessCycles - 1ull) * kWriterTicksPerBusinessCycle);
    if (!writer.submit_cycle(finalSubmission, error) || !writer.close(error)) {
        std::cerr << error << '\n';
        return 6;
    }

    std::cout << "generated=" << argv[1]
              << " business_cycles=" << kBusinessCycles
              << " signals=" << kSignalCount
              << " submissions=" << submissionCount
              << " updates=" << updateCount << '\n';
    return 0;
}
