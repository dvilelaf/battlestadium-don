// Synthetic C++ unit that the setup flow compiles once to measure the speed of this PC (task T-630). It is
// our own code with no game content: template instantiations and loops, sized for about 3 s at -O1 on a fast
// PC. Nothing calls it, so inside the test library it is only compiled, never run. The compile itself is done
// by a later card; the measured seconds go to don_setup::speed_factor.
#include <array>
#include <cstdint>
#include <utility>

namespace don_calibration {

template <int Depth, long long Salt>
struct Mixer {
    static uint64_t apply(uint64_t x) {
        const uint64_t a = Mixer<Depth - 1, Salt * 3 + 1>::apply(x * 0x9E3779B97F4A7C15ull + Salt);
        const uint64_t b = Mixer<Depth - 1, Salt * 5 + 2>::apply((x >> 7) ^ (x << 3) ^ uint64_t(Salt));
        return (a ^ (b >> 5)) + (a << 11) + uint64_t(Depth);
    }
};
template <long long Salt>
struct Mixer<0, Salt> {
    static uint64_t apply(uint64_t x) {
        for (int i = 0; i < 16; ++i) x = (x ^ (x >> 13)) * 0xFF51AFD7ED558CCDull + uint64_t(Salt + i);
        return x;
    }
};

template <std::size_t... I>
uint64_t fan_out(uint64_t x, std::index_sequence<I...>) {
    uint64_t acc = x;
    ((acc ^= Mixer<11, (long long)(I)>::apply(acc + I)), ...);
    return acc;
}

// Entry point. External linkage so the compiler cannot drop the instantiations.
uint64_t run(uint64_t seed) {
    std::array<uint64_t, 8> state{};
    for (std::size_t i = 0; i < state.size(); ++i) state[i] = seed + i;
    for (int round = 0; round < 4; ++round) {
        state[0] = fan_out(state[0], std::make_index_sequence<64>{});
        state[1] = fan_out(state[1], std::make_index_sequence<64>{});
    }
    uint64_t sum = 0;
    for (uint64_t v : state) sum ^= v;
    return sum;
}

}  // namespace don_calibration
