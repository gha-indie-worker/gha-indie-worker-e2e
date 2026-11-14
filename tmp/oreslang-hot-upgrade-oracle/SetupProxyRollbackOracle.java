import java.util.concurrent.*;
import java.util.concurrent.atomic.*;

/** Independently authored setup-freeze retry model; no Oreslang source. */
public final class SetupProxyRollbackOracle {
    private static void check(boolean yes, String msg) {
        if (!yes) throw new AssertionError(msg);
    }
    private static final class Ticket {
        final long epoch = 1;
        boolean consumed;
    }
    private static final class Model {
        private final Object monitor = new Object();
        private final AtomicBoolean pinDebited = new AtomicBoolean();
        private Ticket activeTicket;
        private Ticket pendingTicket;
        private boolean frozen;
        private boolean invalidAccounting;
        private boolean quarantine;
        private int routeEpoch = 1;
        private int candidatePins = 1;
        private int attempts;

        void freezeAndCorrupt() {
            synchronized (monitor) {
                check(!frozen, "double freeze");
                activeTicket = new Ticket();
                frozen = true;
                invalidAccounting = true;
            }
        }
        private void rollback(Ticket ticket) {
            check(ticket == activeTicket && !ticket.consumed, "wrong freeze ticket");
            attempts++;
            if (invalidAccounting) throw new IllegalStateException("bad accounting");
            ticket.consumed = true;
            activeTicket = null;
            frozen = false;
        }
        void setupFailure() {
            synchronized (monitor) {
                check(frozen && activeTicket != null, "no setup freeze");
                Ticket owned = activeTicket;
                // The setup rollback and the common finalizer both attempt
                // to reconcile the same ticket; neither may forget it.
                for (int n = 0; n < 2; n++) {
                    try { rollback(owned); }
                    catch (IllegalStateException bad) {
                        if (pendingTicket != null && pendingTicket != owned)
                            throw new AssertionError("foreign pending ticket");
                        pendingTicket = owned;
                        quarantine = true;
                    }
                }
                if (pinDebited.compareAndSet(false, true)) candidatePins--;
            }
        }
        boolean reconcile() {
            synchronized (monitor) {
                if (pendingTicket == null) return !quarantine;
                rollback(pendingTicket);
                pendingTicket = null;
                if (candidatePins == 0) quarantine = false;
                return !quarantine;
            }
        }
        void repair() {
            synchronized (monitor) { invalidAccounting = false; }
        }
        void assertQuarantine() {
            synchronized (monitor) {
                check(frozen && quarantine && pendingTicket == activeTicket,
                        "unresolved setup failure lost freeze capability");
                check(routeEpoch == 1 && candidatePins == 0, "setup mutated route or pin");
            }
        }
        void assertReconciled() {
            synchronized (monitor) {
                check(!frozen && !quarantine && pendingTicket == null
                        && activeTicket == null && routeEpoch == 1
                        && candidatePins == 0, "reconciliation not atomic");
                check(attempts >= 3, "setup and recovery attempts unobserved");
            }
        }
    }
    public static void main(String[] args) throws Exception {
        long checks = 0;
        for (int n = 0; n < 40000; n++) {
            Model m = new Model();
            m.freezeAndCorrupt();
            m.setupFailure();
            m.assertQuarantine();
            checks++;
            try {
                m.reconcile();
                throw new AssertionError("unsafe thaw before accounting repair");
            } catch (IllegalStateException expected) {
                m.assertQuarantine();
                checks++;
            }
            m.repair();
            check(m.reconcile(), "healthy explicit rollback must succeed");
            m.assertReconciled();
            checks++;
        }
        ExecutorService pool = Executors.newFixedThreadPool(4);
        try {
            for (int round = 0; round < 5000; round++) {
                Model m = new Model();
                m.freezeAndCorrupt();
                m.setupFailure();
                m.repair();
                CountDownLatch launch = new CountDownLatch(1);
                Future<?>[] jobs = new Future<?>[4];
                for (int i = 0; i < jobs.length; i++) {
                    jobs[i] = pool.submit(() -> {
                        try { launch.await(); }
                        catch (InterruptedException e) {
                            Thread.currentThread().interrupt();
                            throw new AssertionError(e);
                        }
                        check(m.reconcile(), "concurrent reconciliation failed");
                    });
                }
                launch.countDown();
                for (Future<?> job : jobs) job.get();
                m.assertReconciled();
                checks += 2;
            }
        } finally { pool.shutdownNow(); }
        // Negative control: dropping the ticket without thaw loses retry
        // authority and strands a frozen mailbox.
        Model old = new Model();
        old.freezeAndCorrupt();
        old.setupFailure();
        synchronized (old.monitor) {
            old.pendingTicket = null;
            check(old.frozen && old.quarantine, "negative control not frozen");
        }
        old.repair();
        check(!old.reconcile(), "missing ticket falsely reconciled");
        checks++;
        System.out.println("PASS source-free setup proxy rollback checks="
                + checks + " four_worker_races=5000"
                + " lost_ticket_negative_control=detected private_source_bytes=0");
    }
}
