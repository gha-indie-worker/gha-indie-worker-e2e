#!/usr/bin/env bash
set -euo pipefail

cat >/tmp/CleanupBoundProbe.java <<'JAVA'
public final class CleanupBoundProbe {
    private static final int LIMIT = 64;
    private static Throwable add(Throwable primary, Throwable failure) {
        if (primary == null) return failure;
        if (primary != failure && primary.getSuppressed().length < LIMIT) {
            primary.addSuppressed(failure);
        }
        return primary;
    }
    public static void main(String[] args) {
        Throwable primary = null;
        int attempted = 0;
        for (int i = 0; i < 96; ++i) {
            attempted++;
            primary = add(primary, new IllegalStateException("cleanup-" + i));
        }
        if (attempted != 96) throw new AssertionError("cleanup short-circuited");
        if (primary == null || !"cleanup-0".equals(primary.getMessage()))
            throw new AssertionError("primary failure changed");
        if (primary.getSuppressed().length != LIMIT)
            throw new AssertionError("suppressed cleanup diagnostics are not bounded");
        System.out.println("bounded cleanup aggregation: PASS");
    }
}
JAVA
javac /tmp/CleanupBoundProbe.java
java -cp /tmp CleanupBoundProbe

echo "model_for_llvm_head=8f4e3c323ca06663f13521b49b0465ad5213667a"
echo "private_source_bytes=0"

cat >/tmp/prepublication_binding.cpp <<'CPP'
#include <cstdint>
#include <stdexcept>
#include <vector>

struct Scheduler {
    std::uint64_t domain{};
    bool live{true};
};
struct ActorRef { std::uint64_t domain{}, slot{}, generation{}; };
struct Ticket { Scheduler* scheduler{}; ActorRef actor{}; };

static bool poll_ready(Scheduler* scheduler, ActorRef actor) {
    return scheduler && scheduler->live
        && actor.domain == scheduler->domain
        && actor.generation != 0;
}

struct Group {
    std::vector<Ticket> published;
    void adopt(Scheduler* scheduler, ActorRef actor) {
        const bool any = actor.domain || actor.slot || actor.generation;
        if ((scheduler == nullptr) != !any ||
            (scheduler && (!actor.domain || !actor.generation ||
                           !poll_ready(scheduler, actor))))
            throw std::invalid_argument("incomplete or foreign scheduler binding");
        Ticket ticket;
        ticket.scheduler = scheduler;
        ticket.actor = actor;
        // Publication happens only after completeness AND scheduler provenance.
        published.push_back(ticket);
        const auto& seen = published.back();
        if (seen.scheduler != scheduler ||
            seen.actor.domain != actor.domain ||
            seen.actor.slot != actor.slot ||
            seen.actor.generation != actor.generation)
            throw std::runtime_error("published torn binding");
    }
};
int main() {
    Group group;
    Scheduler scheduler{9, true};
    Scheduler foreign{10, true};

    bool rejected = false;
    try { group.adopt(nullptr, ActorRef{9, 0, 3}); }
    catch (const std::invalid_argument&) { rejected = true; }
    if (!rejected || !group.published.empty()) return 1;

    rejected = false;
    try { group.adopt(&scheduler, ActorRef{0, 1, 3}); }
    catch (const std::invalid_argument&) { rejected = true; }
    if (!rejected || !group.published.empty()) return 2;

    // Complete public routing labels are not authority when the scheduler
    // pointer names a different minting domain. Reject before publication.
    rejected = false;
    try { group.adopt(&scheduler, ActorRef{foreign.domain, 1, 3}); }
    catch (const std::invalid_argument&) { rejected = true; }
    if (!rejected || !group.published.empty()) return 3;

    scheduler.live = false;
    rejected = false;
    try { group.adopt(&scheduler, ActorRef{scheduler.domain, 1, 3}); }
    catch (const std::invalid_argument&) { rejected = true; }
    if (!rejected || !group.published.empty()) return 4;
    scheduler.live = true;

    group.adopt(&scheduler, ActorRef{scheduler.domain, 1, 3});
    if (group.published.size() != 1) return 5;
    return 0;
}
CPP
g++ -std=c++20 -O2 -Wall -Wextra -Werror /tmp/prepublication_binding.cpp -o /tmp/prepublication_binding_gcc
/tmp/prepublication_binding_gcc
clang++ -std=c++20 -O2 -Wall -Wextra -Werror /tmp/prepublication_binding.cpp -o /tmp/prepublication_binding_clang
/tmp/prepublication_binding_clang
clang++ -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer /tmp/prepublication_binding.cpp -o /tmp/prepublication_binding_san
/tmp/prepublication_binding_san
echo "pre-publication scheduler binding: PASS compilers=3 private_source_bytes=0"


cat >/tmp/ref32_overflow.rs <<'RS'
fn old_checked_shift(slot: u32, shift: u32) -> Option<u32> {
    (slot - 1).checked_shl(shift)
}
fn checked_scale(slot: u32, shift: u32) -> Option<u32> {
    (slot - 1).checked_mul(1u32.checked_shl(shift)?)
}
fn main() {
    let raw = u32::MAX;
    let shift = 15;
    let old = old_checked_shift(raw, shift)
        .expect("checked_shl accepts the count even when value bits are discarded");
    assert_ne!(old, 0);
    assert_eq!(checked_scale(raw, shift), None);
    println!("compressed32 arithmetic overflow rejection: PASS");
}
RS
rustc -D warnings /tmp/ref32_overflow.rs -o /tmp/ref32_overflow
/tmp/ref32_overflow
