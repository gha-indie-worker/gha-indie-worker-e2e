import java.util.concurrent.*;
import java.util.concurrent.atomic.*;

/** Independently authored protocol oracle. Contains no private runtime source. */
public final class CandidateQuarantineOracle {
    private static final class Candidate {
        final AtomicBoolean physicalFinalized = new AtomicBoolean();
        final AtomicBoolean leaseClosed = new AtomicBoolean();
        final AtomicInteger pins;
        Candidate(AtomicInteger pins) { this.pins = pins; pins.incrementAndGet(); }
        void release() {
            if (physicalFinalized.get() && leaseClosed.compareAndSet(false, true))
                pins.decrementAndGet();
        }
    }
    private static final class Supervisor {
        final AtomicInteger pins = new AtomicInteger();
        final Candidate candidate = new Candidate(pins);
        boolean quarantined = true;
        boolean transferPending;
        boolean oldRoutesPending;
        boolean candidatePending = true;
        synchronized void reap() {
            if (candidatePending && candidate.physicalFinalized.get()) {
                candidate.release();
                candidatePending = false;
            }
            if (quarantined && !transferPending && !oldRoutesPending
                    && !candidatePending)
                quarantined = false;
        }
        synchronized boolean admits() { reap(); return !quarantined; }
    }
    private static void check(boolean b, String m) {
        if (!b) throw new AssertionError(m);
    }
    private static void negativeControl() {
        Supervisor s = new Supervisor();
        // Older logic checked only transfer and old routes.
        boolean oldQuarantine = s.quarantined
                && !s.transferPending && !s.oldRoutesPending;
        check(oldQuarantine && s.candidatePending && !s.candidate.physicalFinalized.get(),
                "negative control must demonstrate live candidate");
    }
    public static void main(String[] args) throws Exception {
        long assertions = 0;
        negativeControl(); assertions++;
        for (int i = 0; i < 30000; i++) {
            Supervisor s = new Supervisor();
            check(!s.admits(), "live candidate reopened ingress"); assertions++;
            check(s.pins.get() == 1, "candidate pin lost"); assertions++;
            s.candidate.physicalFinalized.set(true);
            s.reap();
            check(s.admits(), "retired candidate left ingress closed"); assertions++;
            check(s.pins.get() == 0, "lease debit not exactly once"); assertions++;
            s.reap();
            check(s.pins.get() == 0, "double debit"); assertions++;
        }
        ExecutorService pool = Executors.newFixedThreadPool(4);
        try {
            for (int round = 0; round < 5000; round++) {
                Supervisor s = new Supervisor();
                CountDownLatch start = new CountDownLatch(1);
                Future<?>[] work = new Future<?>[4];
                for (int k = 0; k < work.length; k++) {
                    work[k] = pool.submit(() -> {
                        try { start.await(); }
                        catch (InterruptedException e) {
                            Thread.currentThread().interrupt();
                            throw new AssertionError(e);
                        }
                        for (int n = 0; n < 4; n++) {
                            if (s.candidate.physicalFinalized.get()) s.reap();
                            else check(!s.admits(), "pre-finalization reentry");
                        }
                    });
                }
                start.countDown();
                for (Future<?> f : work) f.get();
                check(s.quarantined, "early ingress reopening"); assertions++;
                check(s.pins.get() == 1, "early generation pin release"); assertions++;
                s.candidate.physicalFinalized.set(true);
                for (Future<?> f : work) { // repeated reaper calls under contention
                    s.reap();
                    check(s.pins.get() == 0, "missing or double release"); assertions++;
                }
                check(s.admits(), "quarantine did not clear"); assertions++;
            }
        } finally { pool.shutdownNow(); }
        System.out.println("PASS source-free candidate quarantine assertions="
                + assertions + " concurrent_rounds=5000 workers=4"
                + " negative_control=exposes_early_release private_source_bytes=0");
    }
}
