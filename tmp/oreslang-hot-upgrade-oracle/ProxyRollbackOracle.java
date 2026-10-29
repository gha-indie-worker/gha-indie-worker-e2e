import java.util.concurrent.*;
import java.util.concurrent.atomic.*;

/**
 * Source-free proxy-freeze ownership model. No private Oreslang code.
 */
public final class ProxyRollbackOracle {
    private static final class Ticket {
        boolean frozen = true;
        boolean valid = false;
        int successfulRollbacks;
        synchronized void rollback() {
            if (!valid) throw new IllegalStateException("injected structural drift");
            if (!frozen) throw new IllegalStateException("ticket already consumed");
            frozen = false;
            successfulRollbacks++;
        }
    }
    private static final class Supervisor {
        final Ticket ticket = new Ticket();
        boolean pending = true;
        boolean quarantined = true;
        boolean candidateLive;
        boolean sourceFreezePending;
        boolean retiredOldRoute;
        synchronized boolean reapAndReconcile() {
            if (pending) {
                if (candidateLive || sourceFreezePending || retiredOldRoute)
                    return false;
                ticket.rollback();
                pending = false;
            }
            if (quarantined && !pending && !candidateLive
                    && !sourceFreezePending && !retiredOldRoute)
                quarantined = false;
            return !quarantined;
        }
        synchronized boolean admits() {
            return !quarantined && !ticket.frozen;
        }
    }
    private static void check(boolean okay, String message) {
        if (!okay) throw new AssertionError(message);
    }
    private static void negativeControl() {
        Supervisor s = new Supervisor();
        boolean incorrectOldExit = s.quarantined
                && !s.candidateLive && !s.sourceFreezePending
                && !s.retiredOldRoute; // forgot pending proxy freeze ticket
        check(incorrectOldExit && s.ticket.frozen,
                "old reaper missing freeze owner must be detectably unsafe");
    }
    public static void main(String[] args) throws Exception {
        long checks = 0;
        negativeControl(); checks++;
        for (int i = 0; i < 30000; i++) {
            Supervisor s = new Supervisor();
            try {
                s.reapAndReconcile();
                throw new AssertionError("corrupt mailbox unexpectedly thawed");
            } catch (IllegalStateException expected) { checks++; }
            check(s.pending && !s.admits() && s.ticket.frozen,
                    "failed rollback must remain owned"); checks++;
            s.ticket.valid = true;
            s.candidateLive = (i % 3 == 0);
            s.sourceFreezePending = (i % 3 == 1);
            s.retiredOldRoute = (i % 3 == 2);
            check(!s.reapAndReconcile(), "physical owner unexpectedly thawed");
            checks++;
            check(s.pending && s.ticket.frozen, "pending freeze owner lost");
            checks++;
            s.candidateLive = false;
            s.sourceFreezePending = false;
            s.retiredOldRoute = false;
            check(s.reapAndReconcile(), "repaired rollback did not thaw");
            checks++;
            check(s.admits() && !s.pending, "recovery remains frozen");
            checks++;
            check(s.ticket.successfulRollbacks == 1,
                    "rollback ticket consumed more than once"); checks++;
        }
        ExecutorService workers = Executors.newFixedThreadPool(4);
        try {
            for (int round = 0; round < 5000; round++) {
                Supervisor s = new Supervisor();
                s.ticket.valid = true;
                s.candidateLive = true;
                CountDownLatch start = new CountDownLatch(1);
                Future<?>[] jobs = new Future<?>[4];
                for (int j = 0; j < 4; j++) {
                    jobs[j] = workers.submit(() -> {
                        try { start.await(); }
                        catch (InterruptedException interrupted) {
                            Thread.currentThread().interrupt();
                            throw new AssertionError(interrupted);
                        }
                        for (int k = 0; k < 4; k++)
                            check(!s.reapAndReconcile(),
                                    "candidate under physical lease was thawed");
                    });
                }
                start.countDown();
                for (Future<?> job : jobs) job.get();
                check(s.pending && s.ticket.frozen, "lost pending freeze"); checks++;
                s.candidateLive = false;
                Future<?>[] recovers = new Future<?>[4];
                for (int j = 0; j < 4; j++) {
                    recovers[j] = workers.submit(s::reapAndReconcile);
                }
                for (Future<?> future : recovers) future.get();
                check(s.ticket.successfulRollbacks == 1,
                        "concurrent host reconciliations consumed twice"); checks++;
                check(s.admits(), "final old route failed to reopen"); checks++;
            }
        } finally { workers.shutdownNow(); }
        System.out.println("PASS source-free proxy rollback assertions="
                + checks + " races=5000 workers=4"
                + " negative_control=missing_ticket_unsafe private_source_bytes=0");
    }
}
