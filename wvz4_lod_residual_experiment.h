#ifndef WVZ4_LOD_RESIDUAL_EXPERIMENT_H_
#define WVZ4_LOD_RESIDUAL_EXPERIMENT_H_

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace wvz4_lod_exp {

using i64 = std::int64_t;
using u64 = std::uint64_t;

struct Event {
    i64 cycle = 0;
    u64 value = 0;
};

inline bool event_cycle_less(const Event& a, i64 cycle) {
    return a.cycle < cycle;
}

inline u64 mix_event(const Event& e) {
    u64 x = static_cast<u64>(e.cycle) ^ (e.value + 0x9e3779b97f4a7c15ull);
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebull;
    x ^= x >> 31;
    return x;
}

inline u64 bucket_start(i64 cycle, u64 bucket) {
    if (cycle <= 0 || bucket == 0) return 0;
    return static_cast<u64>(cycle) / bucket;
}

struct QueryMetrics {
    u64 events = 0;
    u64 checksum = 0;
};

struct LevelEvents {
    u64 bucket = 0;
    std::vector<Event> events;
};

class OldIndependentLodBuilder {
public:
    explicit OldIndependentLodBuilder(const std::vector<u64>& buckets) {
        levels_.resize(buckets.size());
        for (std::size_t i = 0; i < buckets.size(); ++i) levels_[i].bucket = buckets[i];
    }

    void feed(const Event& e) {
        ++raw_count_;
        for (std::size_t i = 0; i < levels_.size(); ++i) feed_level(i, e);
    }

    void finish() {
        for (std::size_t i = 0; i < states_.size(); ++i) {
            if (states_[i].valid) {
                levels_[i].events.push_back(states_[i].event);
                states_[i].valid = false;
            }
        }
    }

    const std::vector<LevelEvents>& levels() const { return levels_; }
    u64 raw_count() const { return raw_count_; }

    std::vector<std::size_t> selected_levels() const {
        std::vector<std::size_t> selected;
        u64 previous_count = raw_count_;
        for (std::size_t i = 0; i < levels_.size(); ++i) {
            const u64 count = static_cast<u64>(levels_[i].events.size());
            if (count != 0 && count <= previous_count / 5u) {
                selected.push_back(i);
                previous_count = count;
            }
        }
        return selected;
    }

private:
    struct Pending {
        bool valid = false;
        u64 bucket_id = 0;
        Event event;
    };

    void feed_level(std::size_t level, const Event& e) {
        if (level >= levels_.size()) return;
        if (states_.size() < levels_.size()) states_.resize(levels_.size());
        Pending& p = states_[level];
        const u64 b = bucket_start(e.cycle, levels_[level].bucket);
        if (!p.valid) {
            p.valid = true;
            p.bucket_id = b;
            p.event = e;
            return;
        }
        if (p.bucket_id == b) {
            p.event = e;
            return;
        }
        levels_[level].events.push_back(p.event);
        p.bucket_id = b;
        p.event = e;
    }

    u64 raw_count_ = 0;
    std::vector<LevelEvents> levels_;
    std::vector<Pending> states_;
};

class ResidualLodBuilder {
public:
    explicit ResidualLodBuilder(const std::vector<u64>& buckets) {
        levels_.resize(buckets.size());
        for (std::size_t i = 0; i < buckets.size(); ++i) levels_[i].bucket = buckets[i];
        states_.resize(buckets.size());
    }

    void feed(const Event& e) {
        ++raw_count_;
        if (levels_.empty()) {
            ++raw_sink_count_;
            return;
        }
        feed_level(levels_.size() - 1u, e);
    }

    void finish() {
        for (std::size_t i = 0; i < states_.size(); ++i) {
            if (states_[i].valid) {
                levels_[i].events.push_back(states_[i].event);
                states_[i].valid = false;
            }
        }
    }

    const std::vector<LevelEvents>& levels() const { return levels_; }
    u64 raw_count() const { return raw_count_; }
    u64 raw_sink_count() const { return raw_sink_count_; }

private:
    struct Pending {
        bool valid = false;
        u64 bucket_id = 0;
        Event event;
    };

    void feed_level(std::size_t level, const Event& e) {
        Pending& p = states_[level];
        const u64 b = bucket_start(e.cycle, levels_[level].bucket);
        if (!p.valid) {
            p.valid = true;
            p.bucket_id = b;
            p.event = e;
            return;
        }
        if (p.bucket_id == b) {
            Event displaced = p.event;
            p.event = e;
            if (level == 0) {
                ++raw_sink_count_;
            } else {
                feed_level(level - 1u, displaced);
            }
            return;
        }
        levels_[level].events.push_back(p.event);
        p.bucket_id = b;
        p.event = e;
    }

    u64 raw_count_ = 0;
    u64 raw_sink_count_ = 0;
    std::vector<LevelEvents> levels_;
    std::vector<Pending> states_;
};

inline u64 total_level_events(const std::vector<LevelEvents>& levels) {
    u64 total = 0;
    for (std::size_t i = 0; i < levels.size(); ++i) {
        total += static_cast<u64>(levels[i].events.size());
    }
    return total;
}

inline int choose_largest_bucket_level(const std::vector<LevelEvents>& levels, double cycles_per_pixel) {
    int best = -1;
    for (std::size_t i = 0; i < levels.size(); ++i) {
        if (static_cast<double>(levels[i].bucket) <= cycles_per_pixel) best = static_cast<int>(i);
    }
    return best;
}

inline int choose_largest_bucket_level_from_selected(const std::vector<LevelEvents>& levels,
                                                     const std::vector<std::size_t>& selected,
                                                     double cycles_per_pixel) {
    int best = -1;
    for (std::size_t i = 0; i < selected.size(); ++i) {
        const std::size_t level = selected[i];
        if (level < levels.size() && static_cast<double>(levels[level].bucket) <= cycles_per_pixel) {
            best = static_cast<int>(level);
        }
    }
    return best;
}

inline QueryMetrics scan_single_level(const std::vector<Event>& events, i64 start_cycle, i64 end_cycle) {
    QueryMetrics m;
    if (events.empty() || end_cycle <= start_cycle) return m;
    std::vector<Event>::const_iterator begin =
        std::lower_bound(events.begin(), events.end(), start_cycle, event_cycle_less);
    std::vector<Event>::const_iterator it = begin;
    if (begin != events.begin()) --it; // predecessor for left-edge value.
    const std::vector<Event>::const_iterator end =
        std::lower_bound(events.begin(), events.end(), end_cycle, event_cycle_less);
    for (; it != end; ++it) {
        ++m.events;
        m.checksum ^= mix_event(*it) + 0x9e3779b97f4a7c15ull + (m.checksum << 6) + (m.checksum >> 2);
    }
    return m;
}

inline QueryMetrics merge_residual_levels(const std::vector<LevelEvents>& levels,
                                          int target_level,
                                          i64 start_cycle,
                                          i64 end_cycle) {
    QueryMetrics m;
    if (target_level < 0 || end_cycle <= start_cycle) return m;
    struct Cursor {
        const std::vector<Event>* events = nullptr;
        std::size_t index = 0;
        std::size_t end = 0;
    };
    std::vector<Cursor> cursors;
    for (std::size_t level = static_cast<std::size_t>(target_level); level < levels.size(); ++level) {
        const std::vector<Event>& evs = levels[level].events;
        if (evs.empty()) continue;
        std::vector<Event>::const_iterator begin =
            std::lower_bound(evs.begin(), evs.end(), start_cycle, event_cycle_less);
        std::size_t index = static_cast<std::size_t>(begin - evs.begin());
        if (index > 0) --index;
        const std::vector<Event>::const_iterator end =
            std::lower_bound(evs.begin(), evs.end(), end_cycle, event_cycle_less);
        std::size_t end_index = static_cast<std::size_t>(end - evs.begin());
        if (index < end_index) {
            Cursor c;
            c.events = &evs;
            c.index = index;
            c.end = end_index;
            cursors.push_back(c);
        }
    }

    while (true) {
        int best = -1;
        Event best_event;
        for (std::size_t i = 0; i < cursors.size(); ++i) {
            const Cursor& c = cursors[i];
            if (c.index >= c.end) continue;
            const Event& candidate = (*c.events)[c.index];
            if (best < 0 ||
                candidate.cycle < best_event.cycle ||
                (candidate.cycle == best_event.cycle && candidate.value < best_event.value)) {
                best = static_cast<int>(i);
                best_event = candidate;
            }
        }
        if (best < 0) break;
        ++cursors[static_cast<std::size_t>(best)].index;
        ++m.events;
        m.checksum ^= mix_event(best_event) + 0x9e3779b97f4a7c15ull + (m.checksum << 6) + (m.checksum >> 2);
    }
    return m;
}

inline std::vector<u64> make_decimal_buckets(u64 min_bucket, std::size_t level_count) {
    std::vector<u64> out;
    u64 b = min_bucket;
    for (std::size_t i = 0; i < level_count; ++i) {
        out.push_back(b);
        if (b > (std::numeric_limits<u64>::max)() / 10u) break;
        b *= 10u;
    }
    return out;
}

inline std::vector<u64> make_current_like_buckets(u64 min_bucket, std::size_t level_count) {
    std::vector<u64> out;
    u64 b = min_bucket;
    for (std::size_t i = 0; i < level_count; ++i) {
        out.push_back(b);
        if (b > (std::numeric_limits<u64>::max)() / 5u) break;
        b *= 5u;
    }
    return out;
}

} // namespace wvz4_lod_exp

#endif // WVZ4_LOD_RESIDUAL_EXPERIMENT_H_
