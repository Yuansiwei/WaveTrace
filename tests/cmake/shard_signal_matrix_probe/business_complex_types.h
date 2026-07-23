#pragma once

#include "reflect_macro.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace business_sim {
namespace model {
namespace detail {

template <typename PayloadT, std::size_t ElementCount, typename TraitsT>
class DynamicLane
    : public wave::DynamicTraceTargetFor<
          DynamicLane<PayloadT, ElementCount, TraitsT> > {
    WAVE_REFLECT_FRIEND

public:
    typedef typename TraitsT::counter_type counter_type;

    void initialize(counter_type sequence, std::uint32_t payload_base) {
        sequence_ = sequence;
        for (std::size_t i = 0; i < ElementCount; ++i) {
            payload_[i].word = payload_base + static_cast<std::uint32_t>(i);
            payload_[i].valid = (i & 1u) == 0u;
        }
    }

private:
    counter_type sequence_ = 0;
    std::array<PayloadT, ElementCount> payload_ = {};
};

template <typename PayloadT, std::size_t ElementCount>
class TemplatePeekSource
    : public wave::PeekTraceSourceFor<
          TemplatePeekSource<PayloadT, ElementCount>,
          std::array<PayloadT, ElementCount> > {
    WAVE_REFLECT_FRIEND

public:
    typedef std::array<PayloadT, ElementCount> value_type;

    void initialize(std::uint32_t base) {
        for (std::size_t i = 0; i < ElementCount; ++i) {
            value_[i].word = base + static_cast<std::uint32_t>(i);
            value_[i].valid = (i & 1u) != 0u;
        }
    }

    value_type* peek() { return &value_; }

private:
    value_type value_ = {};
};

} // namespace detail

class ProtectedCounters {
    WAVE_REFLECT_FRIEND

protected:
    std::uint64_t protected_cycles_ = 0;
};

class SidebandState {
    WAVE_REFLECT_FRIEND

protected:
    std::uint32_t protected_cookie_ = 0;
};

class ComplexBusinessBlock : public ProtectedCounters, protected SidebandState {
    WAVE_REFLECT_FRIEND

protected:
    std::uint64_t protected_local_cycles_ = 0;

private:
    struct PrivatePayload {
        std::uint32_t word = 0;
        bool valid = false;
    };

    struct PrivateTraits {
        typedef std::uint64_t counter_type;
    };

    static const std::size_t kPayloadCount = 3u;
    enum { kLaneCount = 2 };
    typedef detail::DynamicLane<
        PrivatePayload, kPayloadCount, PrivateTraits> Lane;

    Lane inline_lane_;
    std::array<Lane, kLaneCount> lane_array_;
    WAVE_PTR Lane* alias_ = NULL;
    std::size_t span_count_ = 0;
    WAVE_PTR_ARRAY(span_count_) Lane* span_ = NULL;
    WAVE_PTR std::unique_ptr<Lane> owned_;
    WAVE_PTR std::shared_ptr<Lane> shared_;
    WAVE_PTR std::weak_ptr<Lane> weak_;
    WAVE_PTR wave::DynamicTraceTarget* erased_ = NULL;

    union {
        struct {
            std::uint32_t enabled_ : 1;
            std::uint32_t priority_ : 3;
            std::uint32_t reserved_ : 28;
        };
        std::uint32_t raw_flags_;
    };

public:
    ComplexBusinessBlock() : raw_flags_(0) {}

    void initialize() {
        protected_cycles_ = 0xB001u;
        protected_cookie_ = 0xB002u;
        protected_local_cycles_ = 0xB003u;
        raw_flags_ = 0xBu;

        inline_lane_.initialize(0xB100u, 0xB110u);
        lane_array_[0].initialize(0xB200u, 0xB210u);
        lane_array_[1].initialize(0xB300u, 0xB310u);
        alias_ = &inline_lane_;
        span_count_ = lane_array_.size();
        span_ = lane_array_.data();

        owned_.reset(new Lane());
        owned_->initialize(0xB400u, 0xB410u);
        shared_.reset(new Lane());
        shared_->initialize(0xB500u, 0xB510u);
        weak_ = shared_;
        erased_ = shared_.get();
    }
};

class ComplexPeekOwner {
    WAVE_REFLECT_FRIEND

private:
    struct PrivatePeekPayload {
        std::uint32_t word = 0;
        bool valid = false;
    };

    static const std::size_t kPeekElementCount = 3u;
    typedef detail::TemplatePeekSource<
        PrivatePeekPayload, kPeekElementCount> PeekSource;

    PeekSource source_;
    WAVE_PTR wave::PeekTraceSource* erased_ = NULL;

public:
    void initialize() {
        source_.initialize(0xB610u);
        erased_ = &source_;
    }
};

struct ComplexBusinessRoot {
    ComplexBusinessBlock block;
    ComplexPeekOwner peek_owner;

    void initialize() {
        block.initialize();
        peek_owner.initialize();
    }
};

} // namespace model
} // namespace business_sim
