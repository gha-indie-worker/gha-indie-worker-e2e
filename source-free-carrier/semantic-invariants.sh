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

cat >/tmp/prepublication_binding.cpp <<'CPP'
#include <cstdint>
#include <stdexcept>
#include <vector>

struct Scheduler {};
struct ActorRef { std::uint64_t domain{}, slot{}, generation{}; };
struct Ticket { Scheduler* scheduler{}; ActorRef actor{}; };
struct Group {
    std::vector<Ticket> published;
    void adopt(Scheduler* scheduler, ActorRef actor) {
        const bool any = actor.domain || actor.slot || actor.generation;
        if ((scheduler == nullptr) != !any ||
            (scheduler && (!actor.domain || !actor.generation)))
            throw std::invalid_argument("incomplete binding");
        Ticket ticket;
        ticket.scheduler = scheduler;
        ticket.actor = actor;
        // Publication happens only after the ticket is completely initialized.
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
    Scheduler scheduler;
    bool rejected = false;
    try { group.adopt(nullptr, ActorRef{9, 0, 3}); }
    catch (const std::invalid_argument&) { rejected = true; }
    if (!rejected || !group.published.empty()) return 1;

    rejected = false;
    try { group.adopt(&scheduler, ActorRef{0, 1, 3}); }
    catch (const std::invalid_argument&) { rejected = true; }
    if (!rejected || !group.published.empty()) return 2;

    group.adopt(&scheduler, ActorRef{9, 1, 3});
    if (group.published.size() != 1) return 3;
    return 0;
}
CPP
clang++ -std=c++20 -Wall -Wextra -Werror /tmp/prepublication_binding.cpp -o /tmp/prepublication_binding
/tmp/prepublication_binding
echo "pre-publication scheduler binding: PASS"
