#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string_view>
#include <thread>

namespace oreslang {

/**
 * Host-only, explicitly enabled native compiler diagnostics.
 * No guest arguments, exception strings, file names or dynamic event names.
 *
 * Fixed storage and atomic release-publication keep recording allocation-free.
 * Export must remain a cold, host-owned operation (never a guest actor turn).
 */
class CoreLog final {
public:
    enum class Level : unsigned { Off = 0, Error = 50, Warn = 40,
                                  Info = 30, Debug = 20, Trace = 10 };
    enum class EventId : unsigned { CompileFailed = 1, InputRejected = 2 };
    static constexpr unsigned capacity = 8192;

    static bool parse_level(std::string_view text, Level& out) noexcept {
        // ASCII-only case folding: stable regardless of host locale.
        char ascii[5] = {};
        if (text.empty() || text.size() > 5) return false;
        for (std::size_t i = 0; i < text.size(); ++i) {
            unsigned char c = static_cast<unsigned char>(text[i]);
            if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c + 32);
            if (c < 'a' || c > 'z') return false;
            ascii[i] = static_cast<char>(c);
        }
        const std::string_view value(ascii, text.size());
        if (value == "off") out = Level::Off;
        else if (value == "error") out = Level::Error;
        else if (value == "warn") out = Level::Warn;
        else if (value == "info") out = Level::Info;
        else if (value == "debug") out = Level::Debug;
        else if (value == "trace") out = Level::Trace;
        else return false;
        return true;
    }

    explicit CoreLog(Level level = Level::Off) noexcept
        : threshold_(level),
          started_(level == Level::Off ? clock::time_point{} : clock::now()) { }

    bool enabled(Level severity) const noexcept {
        return threshold_ != Level::Off && severity != Level::Off &&
               static_cast<unsigned>(severity) >= static_cast<unsigned>(threshold_) &&
               !saturated_.load(std::memory_order_relaxed);
    }

    void record(Level severity, EventId id, std::int64_t value) noexcept {
        if (!enabled(severity)) return;
        const auto event = static_cast<unsigned>(id);
        if (event < 1 || event > 2) return;
        unsigned index = next_.load(std::memory_order_relaxed);
        for (;;) {
            if (index >= capacity) {
                saturated_.store(true, std::memory_order_relaxed);
                return;
            }
            if (next_.compare_exchange_weak(index, index + 1,
                    std::memory_order_relaxed, std::memory_order_relaxed)) break;
        }
        if (index + 1 == capacity)
            saturated_.store(true, std::memory_order_relaxed);
        auto& slot = entries_[index];
        slot.offset_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                clock::now() - started_).count());
        slot.value = value;
        slot.thread_id = static_cast<std::uint64_t>(
            std::hash<std::thread::id>{}(std::this_thread::get_id()));
        slot.code.store(event * 100 + static_cast<unsigned>(severity),
                        std::memory_order_release);
    }

    void dump(std::FILE* stream) const noexcept {
        if (threshold_ == Level::Off || !stream) return;
        std::fprintf(stream,
            "{\"schema\":\"ores-core-log.v1\",\"kind\":\"begin\",\"capacity\":%u}\n",
            capacity);
        const unsigned reserved = next_.load(std::memory_order_acquire);
        unsigned recorded = 0;
        for (unsigned i = 0; i < reserved; ++i) {
            const unsigned code = entries_[i].code.load(std::memory_order_acquire);
            if (!code) continue; // Never disclose a partially published row.
            const char* event = code / 100 == 1 ? "compiler.failed" : "compiler.input.rejected";
            const char* level = severity_name(code % 100);
            const auto& slot = entries_[i];
            std::fprintf(stream,
                "{\"schema\":\"ores-core-log.v1\",\"kind\":\"event\","
                "\"event\":\"%s\",\"severity\":\"%s\",\"start_ns\":%" PRIu64
                ",\"value\":%" PRId64 ",\"thread_id\":%" PRIu64 "}\n",
                event, level, slot.offset_ns, slot.value, slot.thread_id);
            ++recorded;
        }
        std::fprintf(stream,
            "{\"schema\":\"ores-core-log.v1\",\"kind\":\"end\","
            "\"recorded\":%u,\"reserved\":%u,\"unpublished\":%u,\"saturated\":%s}\n",
            recorded, reserved, reserved - recorded,
            saturated_.load(std::memory_order_relaxed) ? "true" : "false");
        std::fflush(stream);
    }

private:
    using clock = std::chrono::steady_clock;
    struct Entry {
        std::atomic<unsigned> code{0};
        std::uint64_t offset_ns = 0;
        std::int64_t value = 0;
        std::uint64_t thread_id = 0;
    };
    static const char* severity_name(unsigned rank) noexcept {
        switch (rank) {
            case 50: return "error";
            case 40: return "warn";
            case 30: return "info";
            case 20: return "debug";
            case 10: return "trace";
            default: return "unknown";
        }
    }
    const Level threshold_;
    const clock::time_point started_;
    std::array<Entry, capacity> entries_{};
    std::atomic<unsigned> next_{0};
    std::atomic<bool> saturated_{false};
};

} // namespace oreslang
