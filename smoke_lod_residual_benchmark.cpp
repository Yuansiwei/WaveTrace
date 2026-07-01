#include "wvz4_lod_residual_experiment.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace {

using wvz4_lod_exp::Event;
using wvz4_lod_exp::i64;
using wvz4_lod_exp::u64;

enum class Pattern {
    All,
    Sparse,
    DenseThenSparse,
    Burst
};

struct Options {
    u64 cycles = 1000000;
    u64 signals = 10;
    Pattern pattern = Pattern::All;
    u64 pixels = 1600;
    std::size_t residual_levels = 7;
    std::size_t old_levels = 7;
};

struct QueryResult {
    std::string mode;
    u64 bucket = 0;
    u64 events = 0;
    u64 checksum = 0;
    double elapsed_ms = 0.0;
};

u64 mix64(u64 x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ull;
    x ^= x >> 33;
    return x;
}

bool parse_u64_arg(const char* text, u64& out) {
    if (!text || !text[0]) return false;
    char* end = NULL;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (end == text || *end != '\0') return false;
    out = static_cast<u64>(value);
    return true;
}

Pattern parse_pattern(const std::string& text) {
    if (text == "all" || text == "dense") return Pattern::All;
    if (text == "sparse") return Pattern::Sparse;
    if (text == "dense_then_sparse" || text == "mixed") return Pattern::DenseThenSparse;
    if (text == "burst" || text == "bursty") return Pattern::Burst;
    return Pattern::All;
}

std::string pattern_name(Pattern p) {
    switch (p) {
    case Pattern::All: return "all";
    case Pattern::Sparse: return "sparse";
    case Pattern::DenseThenSparse: return "dense_then_sparse";
    case Pattern::Burst: return "burst";
    }
    return "all";
}

Event make_event(u64 signal, u64 cycle) {
    Event e;
    e.cycle = static_cast<i64>((std::min)(cycle, static_cast<u64>((std::numeric_limits<i64>::max)())));
    e.value = mix64(cycle ^ (signal * 0x9e3779b97f4a7c15ull));
    return e;
}

template <typename Fn>
void for_each_event(u64 signal, u64 cycles, Pattern pattern, Fn fn) {
    if (cycles == 0) return;
    switch (pattern) {
    case Pattern::All:
        for (u64 t = 0; t < cycles; ++t) fn(make_event(signal, t));
        break;
    case Pattern::Sparse: {
        const u64 stride = 1000;
        const u64 offset = signal % stride;
        for (u64 t = offset; t < cycles; t += stride) fn(make_event(signal, t));
        break;
    }
    case Pattern::DenseThenSparse: {
        const u64 half = cycles / 2u;
        for (u64 t = 0; t < half; ++t) fn(make_event(signal, t));
        const u64 stride = 1000;
        u64 t = half + (signal % stride);
        for (; t < cycles; t += stride) fn(make_event(signal, t));
        break;
    }
    case Pattern::Burst: {
        const u64 period = 10000;
        const u64 burst = 100;
        for (u64 base = 0; base < cycles; base += period) {
            const u64 end = (std::min)(cycles, base + burst);
            for (u64 t = base; t < end; ++t) fn(make_event(signal, t));
        }
        break;
    }
    }
}

u64 count_arithmetic_progression(u64 offset, u64 stride, u64 start, u64 end) {
    if (end <= start || stride == 0) return 0;
    u64 first = offset;
    if (first < start) {
        const u64 delta = start - first;
        first += ((delta + stride - 1u) / stride) * stride;
    }
    if (first >= end) return 0;
    return ((end - 1u - first) / stride) + 1u;
}

u64 raw_count_for_signal(u64 signal, u64 cycles, Pattern pattern, u64 start, u64 end) {
    if (start >= cycles) return 0;
    end = (std::min)(end, cycles);
    if (end <= start) return 0;
    switch (pattern) {
    case Pattern::All:
        return end - start;
    case Pattern::Sparse:
        return count_arithmetic_progression(signal % 1000u, 1000u, start, end);
    case Pattern::DenseThenSparse: {
        const u64 half = cycles / 2u;
        u64 count = 0;
        if (start < half) count += (std::min)(end, half) - start;
        if (end > half) count += count_arithmetic_progression(half + (signal % 1000u), 1000u,
                                                              (std::max)(start, half), end);
        return count;
    }
    case Pattern::Burst: {
        const u64 period = 10000;
        const u64 burst = 100;
        u64 count = 0;
        const u64 first_base = (start / period) * period;
        for (u64 base = first_base; base < end; base += period) {
            const u64 burst_start = base;
            const u64 burst_end = (std::min)(cycles, base + burst);
            const u64 s = (std::max)(start, burst_start);
            const u64 e = (std::min)(end, burst_end);
            if (e > s) count += e - s;
            if (period > (std::numeric_limits<u64>::max)() - base) break;
        }
        return count;
    }
    }
    return 0;
}

double elapsed_ms_since(const std::chrono::high_resolution_clock::time_point& start) {
    const std::chrono::duration<double, std::milli> d =
        std::chrono::high_resolution_clock::now() - start;
    return d.count();
}

std::string fmt_pct(u64 part, u64 total) {
    std::ostringstream s;
    s << std::fixed << std::setprecision(4);
    if (total == 0) {
        s << 0.0;
    } else {
        s << (100.0 * static_cast<double>(part) / static_cast<double>(total));
    }
    return s.str();
}

Options parse_options(int argc, char** argv) {
    Options opt;
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i] ? argv[i] : "";
        if (arg == "--pixels") {
            if (i + 1 < argc) parse_u64_arg(argv[++i], opt.pixels);
        } else if (arg == "--residual-levels") {
            u64 v = 0;
            if (i + 1 < argc && parse_u64_arg(argv[++i], v)) opt.residual_levels = static_cast<std::size_t>(v);
        } else if (arg == "--old-levels") {
            u64 v = 0;
            if (i + 1 < argc && parse_u64_arg(argv[++i], v)) opt.old_levels = static_cast<std::size_t>(v);
        } else if (arg == "--help" || arg == "-h") {
            std::cout
                << "usage: smoke_lod_residual_benchmark [cycles] [signals] [pattern]\n"
                << "patterns: all, sparse, dense_then_sparse, burst\n"
                << "options: --pixels N --residual-levels N --old-levels N\n";
            std::exit(0);
        } else {
            positional.push_back(arg);
        }
    }
    if (positional.size() >= 1) parse_u64_arg(positional[0].c_str(), opt.cycles);
    if (positional.size() >= 2) parse_u64_arg(positional[1].c_str(), opt.signals);
    if (positional.size() >= 3) opt.pattern = parse_pattern(positional[2]);
    if (opt.cycles == 0) opt.cycles = 1;
    if (opt.signals == 0) opt.signals = 1;
    if (opt.pixels == 0) opt.pixels = 1600;
    return opt;
}

QueryResult run_residual_query(const std::vector<wvz4_lod_exp::ResidualLodBuilder>& builders,
                               const Options& opt,
                               double cycles_per_pixel,
                               u64 start,
                               u64 end) {
    QueryResult result;
    const std::vector<wvz4_lod_exp::LevelEvents>& first_levels = builders.front().levels();
    const int target = wvz4_lod_exp::choose_largest_bucket_level(first_levels, cycles_per_pixel);
    result.mode = target < 0 ? "raw" : "residual_merge";
    result.bucket = target < 0 ? 1u : first_levels[static_cast<std::size_t>(target)].bucket;
    const auto t0 = std::chrono::high_resolution_clock::now();
    for (u64 signal = 0; signal < opt.signals; ++signal) {
        if (target < 0) {
            result.events += raw_count_for_signal(signal, opt.cycles, opt.pattern, start, end);
            result.checksum ^= result.events + signal * 1315423911ull;
        } else {
            const wvz4_lod_exp::QueryMetrics m =
                wvz4_lod_exp::merge_residual_levels(builders[static_cast<std::size_t>(signal)].levels(),
                                                    target,
                                                    static_cast<i64>(start),
                                                    static_cast<i64>(end));
            result.events += m.events;
            result.checksum ^= m.checksum + signal * 1315423911ull;
        }
    }
    result.elapsed_ms = elapsed_ms_since(t0);
    return result;
}

QueryResult run_old_query(const std::vector<wvz4_lod_exp::OldIndependentLodBuilder>& builders,
                          const std::vector<std::vector<std::size_t> >& selected,
                          const Options& opt,
                          double cycles_per_pixel,
                          u64 start,
                          u64 end) {
    QueryResult result;
    const int target = wvz4_lod_exp::choose_largest_bucket_level_from_selected(
        builders.front().levels(), selected.front(), cycles_per_pixel);
    result.mode = target < 0 ? "raw" : "old_single_level";
    result.bucket = target < 0 ? 1u : builders.front().levels()[static_cast<std::size_t>(target)].bucket;
    const auto t0 = std::chrono::high_resolution_clock::now();
    for (u64 signal = 0; signal < opt.signals; ++signal) {
        if (target < 0) {
            result.events += raw_count_for_signal(signal, opt.cycles, opt.pattern, start, end);
            result.checksum ^= result.events + signal * 2654435761ull;
        } else {
            const std::vector<Event>& evs =
                builders[static_cast<std::size_t>(signal)].levels()[static_cast<std::size_t>(target)].events;
            const wvz4_lod_exp::QueryMetrics m =
                wvz4_lod_exp::scan_single_level(evs, static_cast<i64>(start), static_cast<i64>(end));
            result.events += m.events;
            result.checksum ^= m.checksum + signal * 2654435761ull;
        }
    }
    result.elapsed_ms = elapsed_ms_since(t0);
    return result;
}

} // namespace

int main(int argc, char** argv) {
    const Options opt = parse_options(argc, argv);
    const std::vector<u64> residual_buckets =
        wvz4_lod_exp::make_decimal_buckets(10, opt.residual_levels);
    const std::vector<u64> old_buckets =
        wvz4_lod_exp::make_current_like_buckets(256, opt.old_levels);

    std::vector<wvz4_lod_exp::ResidualLodBuilder> residual_builders;
    std::vector<wvz4_lod_exp::OldIndependentLodBuilder> old_builders;
    residual_builders.reserve(static_cast<std::size_t>(opt.signals));
    old_builders.reserve(static_cast<std::size_t>(opt.signals));
    for (u64 signal = 0; signal < opt.signals; ++signal) {
        residual_builders.push_back(wvz4_lod_exp::ResidualLodBuilder(residual_buckets));
        old_builders.push_back(wvz4_lod_exp::OldIndependentLodBuilder(old_buckets));
    }

    u64 raw_events = 0;
    const auto build_start = std::chrono::high_resolution_clock::now();
    for (u64 signal = 0; signal < opt.signals; ++signal) {
        for_each_event(signal, opt.cycles, opt.pattern, [&](const Event& e) {
            residual_builders[static_cast<std::size_t>(signal)].feed(e);
            old_builders[static_cast<std::size_t>(signal)].feed(e);
            ++raw_events;
        });
    }
    const double feed_ms = elapsed_ms_since(build_start);

    const auto finish_start = std::chrono::high_resolution_clock::now();
    for (u64 signal = 0; signal < opt.signals; ++signal) {
        residual_builders[static_cast<std::size_t>(signal)].finish();
        old_builders[static_cast<std::size_t>(signal)].finish();
    }
    const double finish_ms = elapsed_ms_since(finish_start);

    u64 residual_events = 0;
    u64 residual_raw_sink = 0;
    u64 residual_accounted = 0;
    u64 old_all_events = 0;
    u64 old_selected_events = 0;
    std::vector<std::vector<std::size_t> > old_selected;
    old_selected.reserve(static_cast<std::size_t>(opt.signals));

    for (u64 signal = 0; signal < opt.signals; ++signal) {
        const std::size_t index = static_cast<std::size_t>(signal);
        residual_events += wvz4_lod_exp::total_level_events(residual_builders[index].levels());
        residual_raw_sink += residual_builders[index].raw_sink_count();
        residual_accounted += residual_builders[index].raw_sink_count() +
                              wvz4_lod_exp::total_level_events(residual_builders[index].levels());
        old_all_events += wvz4_lod_exp::total_level_events(old_builders[index].levels());
        old_selected.push_back(old_builders[index].selected_levels());
        for (std::size_t i = 0; i < old_selected.back().size(); ++i) {
            old_selected_events += static_cast<u64>(
                old_builders[index].levels()[old_selected.back()[i]].events.size());
        }
    }

    std::cout << "scenario,pattern," << pattern_name(opt.pattern)
              << ",cycles," << opt.cycles
              << ",signals," << opt.signals
              << ",pixels," << opt.pixels
              << ",raw_events," << raw_events << "\n";
    std::cout << "build,feed_ms," << std::fixed << std::setprecision(3) << feed_ms
              << ",finish_ms," << finish_ms << "\n";
    std::cout << "storage,residual_events," << residual_events
              << ",residual_pct_raw," << fmt_pct(residual_events, raw_events)
              << ",residual_raw_sink," << residual_raw_sink
              << ",residual_accounted," << residual_accounted
              << ",account_ok," << (residual_accounted == raw_events ? 1 : 0) << "\n";
    std::cout << "storage,old_all_level_events," << old_all_events
              << ",old_all_pct_raw," << fmt_pct(old_all_events, raw_events)
              << ",old_selected_events," << old_selected_events
              << ",old_selected_pct_raw," << fmt_pct(old_selected_events, raw_events) << "\n";

    if (!residual_builders.empty()) {
        const std::vector<wvz4_lod_exp::LevelEvents>& levels = residual_builders.front().levels();
        for (std::size_t i = 0; i < levels.size(); ++i) {
            u64 total = 0;
            for (u64 signal = 0; signal < opt.signals; ++signal) {
                total += static_cast<u64>(residual_builders[static_cast<std::size_t>(signal)].levels()[i].events.size());
            }
            std::cout << "level,residual," << i << ",bucket," << levels[i].bucket
                      << ",events," << total
                      << ",pct_raw," << fmt_pct(total, raw_events) << "\n";
        }
    }
    if (!old_builders.empty()) {
        const std::vector<wvz4_lod_exp::LevelEvents>& levels = old_builders.front().levels();
        for (std::size_t i = 0; i < levels.size(); ++i) {
            u64 total = 0;
            u64 selected_total = 0;
            for (u64 signal = 0; signal < opt.signals; ++signal) {
                const std::size_t s = static_cast<std::size_t>(signal);
                total += static_cast<u64>(old_builders[s].levels()[i].events.size());
                if (std::find(old_selected[s].begin(), old_selected[s].end(), i) != old_selected[s].end()) {
                    selected_total += static_cast<u64>(old_builders[s].levels()[i].events.size());
                }
            }
            std::cout << "level,old," << i << ",bucket," << levels[i].bucket
                      << ",events," << total
                      << ",selected_events," << selected_total
                      << ",pct_raw," << fmt_pct(total, raw_events) << "\n";
        }
    }

    std::vector<double> cpp_values;
    cpp_values.push_back(10.0);
    cpp_values.push_back(100.0);
    cpp_values.push_back(1000.0);
    cpp_values.push_back(10000.0);
    cpp_values.push_back(100000.0);
    cpp_values.push_back(static_cast<double>(opt.cycles) / static_cast<double>(opt.pixels));

    for (std::size_t i = 0; i < cpp_values.size(); ++i) {
        const double cpp = cpp_values[i];
        u64 span = static_cast<u64>(cpp * static_cast<double>(opt.pixels));
        if (span == 0) span = 1;
        if (span > opt.cycles) span = opt.cycles;
        const u64 start = (opt.cycles > span) ? ((opt.cycles - span) / 2u) : 0u;
        const u64 end = start + span;
        const QueryResult old_result = run_old_query(old_builders, old_selected, opt, cpp, start, end);
        const QueryResult residual_result = run_residual_query(residual_builders, opt, cpp, start, end);
        std::cout << "query,cpp," << std::fixed << std::setprecision(3) << cpp
                  << ",start," << start
                  << ",end," << end
                  << ",old_mode," << old_result.mode
                  << ",old_bucket," << old_result.bucket
                  << ",old_events," << old_result.events
                  << ",old_ms," << old_result.elapsed_ms
                  << ",residual_mode," << residual_result.mode
                  << ",residual_bucket," << residual_result.bucket
                  << ",residual_events," << residual_result.events
                  << ",residual_ms," << residual_result.elapsed_ms
                  << ",event_ratio_residual_to_old,"
                  << (old_result.events == 0 ? 0.0 : static_cast<double>(residual_result.events) / static_cast<double>(old_result.events))
                  << "\n";
    }

    return residual_accounted == raw_events ? 0 : 1;
}
