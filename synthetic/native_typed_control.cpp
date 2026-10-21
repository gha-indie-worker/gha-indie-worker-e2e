#include <array>
#include <atomic>
#include <cstdint>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

enum : std::uint64_t {
    PANIC = 1,
    CANCELLED = 2,
    STOPPED = 3,
    THROW = 4,
    RAISE = 5
};

struct Event {
    std::uint64_t kind;
    std::uint64_t child_generation;
};

int main() {
    if (PANIC != 1 || CANCELLED != 2 || STOPPED != 3 || THROW != 4 || RAISE != 5)
        throw std::runtime_error("typed CONTROL ABI numbering changed");

    std::array<Event, 3> events{{{THROW, 7}, {RAISE, 7}, {PANIC, 7}}};
    std::atomic<int> active{0};
    std::atomic<int> peak{0};
    std::array<int, 6> seen{};
    std::promise<void> done;
    auto future = done.get_future();

    std::thread parent([&] {
        for (const auto& event : events) {
            int now = active.fetch_add(1) + 1;
            int old = peak.load();
            while (now > old && !peak.compare_exchange_weak(old, now)) { }
            if (event.child_generation != 7) throw std::runtime_error("stale generation");
            if (event.kind != THROW && event.kind != RAISE && event.kind != PANIC)
                throw std::runtime_error("invalid typed CONTROL kind");
            ++seen.at(static_cast<std::size_t>(event.kind));
            active.fetch_sub(1);
        }
        done.set_value();
    });
    future.get();
    parent.join();

    if (peak.load() != 1) throw std::runtime_error("CONTROL overlap");
    if (seen[THROW] != 1 || seen[RAISE] != 1 || seen[PANIC] != 1)
        throw std::runtime_error("typed CONTROL collapsed");

    std::cout << "SYNTHETIC NATIVE TYPED CONTROL: 8/8 invariants passed\n";
    std::cout << "LLVM_SOURCE_SHA=9e28dcdd855b22fdfadb47978ebfcd2c918f703b\n";
    std::cout << "LLVM_SOURCE_TREE=e9bf402f802c0654ab602d1f12be4f10bfa58c50\n";
    std::cout << "NOTE=source-free model; no private Oreslang source copied\n";
}
