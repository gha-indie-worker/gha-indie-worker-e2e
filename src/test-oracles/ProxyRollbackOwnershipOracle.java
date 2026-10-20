import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.Callable;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.atomic.AtomicLong;

/**
 * Source-free state-machine oracle for supervisor ownership of a failed
 * proxy-mailbox freeze rollback. This file intentionally contains no private
 * Oreslang source, signatures, constants, or copied implementation text.
 */
public final class ProxyRollbackOwnershipOracle {
    private static final AtomicLong CHECKS = new AtomicLong();

    private static final class Ticket {
        private boolean frozen = true;
        private int attempts;
        private int successfulRollbacks;

        synchronized void rollback(boolean injectFailure) {
            attempts++;
            if (injectFailure) {
                throw new IllegalStateException("injected structural rollback rejection");
            }
            if (!frozen) {
                throw new IllegalStateException("ticket already consumed");
            }
            frozen = false;
            successfulRollbacks++;
        }

        synchronized boolean frozen() {
            return frozen;
        }

        synchronized int attempts() {
            return attempts;
        }

        synchronized int successfulRollbacks() {
            return successfulRollbacks;
        }
    }

    private static final class Model {
        private Ticket txnTicket;
        private Ticket pendingTicket;
        private boolean transferPending;
        private boolean quarantined;
        private long routeEpoch = 1L;

        synchronized void begin(Ticket ticket, boolean withTransfer) {
            txnTicket = ticket;
            transferPending = withTransfer;
        }

        synchronized void retainFailedProxyRollback(Ticket ticket) {
            if (pendingTicket != null && pendingTicket != ticket) {
                throw new IllegalStateException("different pending ticket");
            }
            pendingTicket = ticket;
            txnTicket = null;
            quarantined = true;
        }

        synchronized void buggyRetainWithoutExclusiveTransfer(Ticket ticket) {
            pendingTicket = ticket;
            txnTicket = ticket;
            quarantined = true;
        }

        synchronized void outerFinalizerRetryIfTransactionStillOwns() {
            if (txnTicket != null) {
                txnTicket.rollback(false);
                txnTicket = null;
            }
        }

        synchronized boolean reconcileTransfer() {
            if (!transferPending) {
                return !quarantined;
            }
            transferPending = false;
            return !quarantined;
        }

        synchronized boolean reconcileProxy() {
            if (pendingTicket == null) {
                return !quarantined;
            }
            if (transferPending) {
                return false;
            }
            Ticket owned = pendingTicket;
            owned.rollback(false);
            pendingTicket = null;
            quarantined = false;
            return true;
        }

        synchronized Ticket txnTicket() {
            return txnTicket;
        }

        synchronized Ticket pendingTicket() {
            return pendingTicket;
        }

        synchronized boolean transferPending() {
            return transferPending;
        }

        synchronized boolean quarantined() {
            return quarantined;
        }

        synchronized long routeEpoch() {
            return routeEpoch;
        }
    }

    private static void check(boolean condition, String message) {
        CHECKS.incrementAndGet();
        if (!condition) {
            throw new AssertionError(message);
        }
    }

    private static void positiveProtocolRound(boolean withTransfer) {
        Ticket ticket = new Ticket();
        Model model = new Model();
        model.begin(ticket, withTransfer);

        boolean failed = false;
        try {
            ticket.rollback(true);
        } catch (IllegalStateException expected) {
            failed = true;
        }
        check(failed, "injected first rollback must fail");
        check(ticket.frozen(), "failed rollback must retain physical freeze");

        model.retainFailedProxyRollback(ticket);
        check(model.txnTicket() == null, "transaction must relinquish ticket ownership");
        check(model.pendingTicket() == ticket, "supervisor must retain exact failed ticket");
        check(model.quarantined(), "failed rollback must keep quarantine");
        check(model.routeEpoch() == 1L, "rollback recovery must not advance route epoch");

        model.outerFinalizerRetryIfTransactionStillOwns();
        check(ticket.frozen(), "outer finalizer must not consume transferred ticket");
        check(ticket.successfulRollbacks() == 0,
                "no successful rollback may occur behind supervisor ownership");

        if (withTransfer) {
            boolean sourceResult = model.reconcileTransfer();
            check(!sourceResult,
                    "source-transfer reconciliation may stay false while proxy quarantine remains");
            check(!model.transferPending(), "source-transfer owner must still clear independently");
            check(model.pendingTicket() == ticket,
                    "source-transfer reconciliation must not consume proxy ticket");
        }

        check(model.reconcileProxy(), "proxy rollback must succeed once prerequisites clear");
        check(model.pendingTicket() == null, "proxy owner must clear after confirmed thaw");
        check(!model.quarantined(), "quarantine must clear only after confirmed thaw");
        check(!ticket.frozen(), "ticket must be physically thawed");
        check(ticket.successfulRollbacks() == 1, "ticket must be consumed exactly once");
        check(ticket.attempts() == 2, "one rejected and one successful rollback expected");
        check(model.routeEpoch() == 1L, "recovery must preserve old route epoch");
        check(model.reconcileProxy(), "repeated reconciliation must be idempotent when clear");
        check(ticket.successfulRollbacks() == 1,
                "idempotent reconciliation must not re-consume ticket");
    }

    private static void negativeControl() {
        Ticket ticket = new Ticket();
        Model buggy = new Model();
        buggy.begin(ticket, false);

        try {
            ticket.rollback(true);
        } catch (IllegalStateException expected) {
            // Expected initial rollback rejection.
        }
        buggy.buggyRetainWithoutExclusiveTransfer(ticket);
        buggy.outerFinalizerRetryIfTransactionStillOwns();
        check(!ticket.frozen(), "negative control must consume ticket in outer finalizer");

        boolean staleDetected = false;
        try {
            buggy.reconcileProxy();
        } catch (IllegalStateException expected) {
            staleDetected = true;
        }
        check(staleDetected,
                "dual ownership must expose stale pending-ticket failure in negative control");
    }

    private static void concurrentRecoveryRounds(int rounds)
            throws InterruptedException, ExecutionException {
        ExecutorService pool = Executors.newFixedThreadPool(4);
        try {
            List<Callable<Boolean>> calls = new ArrayList<>(4);
            for (int round = 0; round < rounds; round++) {
                Ticket ticket = new Ticket();
                Model model = new Model();
                model.begin(ticket, true);
                try {
                    ticket.rollback(true);
                } catch (IllegalStateException expected) {
                    // Expected injected failure.
                }
                model.retainFailedProxyRollback(ticket);
                model.reconcileTransfer();

                calls.clear();
                for (int i = 0; i < 4; i++) {
                    calls.add(model::reconcileProxy);
                }
                List<Future<Boolean>> results = pool.invokeAll(calls);
                for (Future<Boolean> result : results) {
                    check(result.get(), "all serialized recovery callers must observe success");
                }
                check(ticket.successfulRollbacks() == 1,
                        "concurrent recovery must consume ticket exactly once");
                check(model.pendingTicket() == null,
                        "concurrent recovery must leave no pending owner");
                check(!model.quarantined(),
                        "concurrent recovery must clear quarantine");
                check(model.routeEpoch() == 1L,
                        "concurrent recovery must preserve route epoch");
            }
        } finally {
            pool.shutdownNow();
        }
    }

    public static void main(String[] args) throws Exception {
        negativeControl();
        for (int i = 0; i < 10_000; i++) {
            positiveProtocolRound((i & 1) == 0);
        }
        concurrentRecoveryRounds(5_000);
        System.out.println(
                "PASS source-free proxy rollback ownership checks=" + CHECKS.get()
                        + " race_rounds=5000 workers=4"
                        + " negative_control=dual_owner_stale_ticket_detected"
                        + " private_source_bytes=0");
    }
}
