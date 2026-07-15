#include <array>
#include <memory>

namespace wave {
template <typename T>
struct WavePtr {
    T value;
};
}

struct PayloadA {
    int value;
};

struct PayloadB {
    int value;
};

namespace alpha {
template <typename T>
struct Box {
    wave::WavePtr<T*> template_ptr;
    wave::WavePtr<T*> enabled_ptr;
    int ordinary;
};
}

namespace beta {
struct Box {
    wave::WavePtr<int*> other_ptr;
};
}

using AliasPtr = wave::WavePtr<PayloadB*>;

struct Root {
    alpha::Box<PayloadA> first;
    alpha::Box<PayloadB> second;
    beta::Box third;
    std::array<PayloadA, 2> array_payloads;
    std::shared_ptr<PayloadA> shared_payload;
    std::unique_ptr<PayloadB> unique_payload;
    wave::WavePtr<PayloadA*> direct_ptr;
    AliasPtr alias_ptr;
};
