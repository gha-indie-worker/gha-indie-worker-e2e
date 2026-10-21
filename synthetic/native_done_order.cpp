#include <atomic>
#include <cstdint>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

struct Ticket {
    std::atomic<bool> physically_done{false};
    std::promise<void> done;
    std::shared_future<void> future = done.get_future().share();
};

struct Registry {
    std::mutex mutex;
    std::map<std::uint64_t, std::uint64_t> live;
    std::map<std::uint64_t, std::uint64_t> high;

    void adopt(std::uint64_t actor, std::uint64_t generation) {
        std::lock_guard<std::mutex> guard(mutex);
        if (live.contains(actor)) throw std::logic_error("still live");
        auto it = high.find(actor);
        if (it != high.end() && generation <= it->second)
            throw std::logic_error("stale generation");
        live[actor] = generation;
        high[actor] = generation;
    }

    void finalize(std::uint64_t actor, std::uint64_t generation, Ticket& ticket) {
        std::lock_guard<std::mutex> guard(mutex);
        auto it = live.find(actor);
        if (it == live.end() || it->second != generation)
            throw std::logic_error("stale finalizer");
        // Contract under test: physical retirement + owner release precede DONE.
        ticket.physically_done.store(true, std::memory_order_release);
        live.erase(it);
        ticket.done.set_value();
    }
};

int main() {
    constexpr int rounds = 2000;
    for (int i = 0; i < rounds; ++i) {
        Registry registry;
        Ticket old;
        registry.adopt(1, 41);

        std::atomic<bool> saw_physical{false};
        std::atomic<bool> admitted{false};
        std::thread waiter([&] {
            old.future.get();
            saw_physical.store(
                old.physically_done.load(std::memory_order_acquire),
                std::memory_order_release);
            try {
                registry.adopt(1, 42);
                admitted.store(true, std::memory_order_release);
            } catch (...) {
                admitted.store(false, std::memory_order_release);
            }
        });

        registry.finalize(1, 41, old);
        waiter.join();
        if (!saw_physical.load(std::memory_order_acquire))
            throw std::runtime_error("DONE outran physical retirement");
        if (!admitted.load(std::memory_order_acquire))
            throw std::runtime_error("newer same-id generation blocked after DONE");
    }

    std::cout << "SYNTHETIC NATIVE DONE CONTRACT: 2000/2000 passed\n";
    std::cout << "LLVM_SOURCE_SHA=9e28dcdd855b22fdfadb47978ebfcd2c918f703b\n";
    std::cout << "LLVM_SOURCE_TREE=e9bf402f802c0654ab602d1f12be4f10bfa58c50\n";
    std::cout << "NOTE=source-free model; no private Oreslang source copied\n";
}
