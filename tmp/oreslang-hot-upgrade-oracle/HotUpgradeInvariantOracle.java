import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * Independently authored source-free oracle for hot-upgrade lifecycle invariants.
 * It intentionally contains no copied Oreslang repository source.
 */
public final class HotUpgradeInvariantOracle {
    private HotUpgradeInvariantOracle() { }

    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    private static final class Lease implements AutoCloseable {
        private final AtomicInteger pins;
        private final AtomicBoolean closed = new AtomicBoolean();

        Lease(AtomicInteger pins) {
            this.pins = pins;
            pins.incrementAndGet();
        }

        @Override
        public void close() {
            if (closed.compareAndSet(false, true)) {
                int remaining = pins.decrementAndGet();
                check(remaining >= 0, "lease pin underflow");
            }
        }
    }

    private static final class Scheduler {
        private final AtomicInteger admissions = new AtomicInteger();
        private volatile boolean reject;

        void admit() {
            if (reject) throw new RejectedExecutionException("injected");
            admissions.incrementAndGet();
        }
    }

    private static final class Scope {
        private static final class Slot { }
        private final Set<Slot> owned = new HashSet<>();
        private boolean failReservation;

        void start(Scheduler scheduler, Lease lease) {
            Slot slot = new Slot();
            try {
                if (failReservation) {
                    throw new OutOfMemoryError("injected owner reservation");
                }
                owned.add(slot);
            } catch (RuntimeException | Error failure) {
                lease.close();
                throw failure;
            }

            try {
                scheduler.admit();
            } catch (RuntimeException | Error failure) {
                owned.remove(slot);
                lease.close();
                throw failure;
            }
        }
    }

    private static final class Router {
        private final Object lock = new Object();
        private final AtomicInteger pins = new AtomicInteger();
        private long epoch = 1L;

        Child child() {
            synchronized (lock) {
                return new Child(epoch);
            }
        }

        boolean publishNext() {
            synchronized (lock) {
                if (pins.get() != 0) return false;
                epoch++;
                return true;
            }
        }

        Lease start(Child child, Scheduler scheduler) {
            synchronized (lock) {
                if (child.epoch != epoch) {
                    throw new RejectedExecutionException("stale epoch");
                }
                Lease lease = new Lease(pins);
                try {
                    scheduler.admit();
                    return lease;
                } catch (RuntimeException | Error failure) {
                    lease.close();
                    throw failure;
                }
            }
        }

        long epoch() {
            synchronized (lock) {
                return epoch;
            }
        }

        int pins() {
            return pins.get();
        }
    }

    private record Child(long epoch) { }

    private static final class Drain {
        final long target;
        boolean done;

        Drain(long target) {
            this.target = target;
        }
    }

    private static final class Receipt {
        final long sequence;
        boolean done;
        boolean failed;

        Receipt(long sequence) {
            this.sequence = sequence;
        }
    }

    private static final class Cell {
        private static final int MAX_PUBLIC_DRAINS = 1_024;
        private static final int MAX_EXACT_RECEIPTS = 1_024;

        private final Object lock = new Object();
        private final List<Drain> publicDrains = new ArrayList<>();
        private final Map<Long, Receipt> exact = new HashMap<>();
        private long nextSequence;
        private long processed = -1L;
        private boolean stopped;

        Drain drain() {
            synchronized (lock) {
                if (publicDrains.size() >= MAX_PUBLIC_DRAINS) {
                    throw new RejectedExecutionException("public drain quota");
                }
                Drain drain = new Drain(nextSequence - 1L);
                if (processed >= drain.target) drain.done = true;
                else publicDrains.add(drain);
                return drain;
            }
        }

        Receipt admitTracked() {
            synchronized (lock) {
                if (exact.size() >= MAX_EXACT_RECEIPTS) {
                    throw new RejectedExecutionException("exact receipt quota");
                }
                long sequence = nextSequence++;
                Receipt receipt = new Receipt(sequence);
                Receipt prior = exact.put(sequence, receipt);
                check(prior == null, "duplicate exact receipt sequence");
                return receipt;
            }
        }

        long admitUntracked() {
            synchronized (lock) {
                return nextSequence++;
            }
        }

        void process(long sequence) {
            final Drain[] completed;
            final Receipt receipt;
            int count = 0;
            synchronized (lock) {
                check(sequence == processed + 1L, "nonsequential completion");

                // Reserve all completion storage before changing receipt/watermark state.
                completed = new Drain[publicDrains.size()];
                for (int i = publicDrains.size() - 1; i >= 0; --i) {
                    Drain drain = publicDrains.get(i);
                    if (drain.target <= sequence) {
                        completed[count++] = drain;
                        publicDrains.remove(i);
                    }
                }
                receipt = exact.remove(sequence);
                processed = sequence;
            }
            if (receipt != null) receipt.done = true;
            for (int i = 0; i < count; ++i) completed[i].done = true;
        }

        void terminate(boolean injectSnapshotOome) {
            final List<Drain> drainSnapshot;
            final List<Receipt> receiptSnapshot;
            synchronized (lock) {
                if (injectSnapshotOome) {
                    throw new OutOfMemoryError("injected teardown snapshot");
                }
                // Every fallible snapshot is prepared before stop publication.
                drainSnapshot = List.copyOf(publicDrains);
                receiptSnapshot = List.copyOf(exact.values());

                stopped = true;
                publicDrains.clear();
                exact.clear();
            }
            for (Receipt receipt : receiptSnapshot) receipt.failed = true;
            for (Drain drain : drainSnapshot) drain.done = true;
        }

        boolean stopped() {
            synchronized (lock) {
                return stopped;
            }
        }
    }

    private static void preAdmissionOwnershipFailure() {
        AtomicInteger pins = new AtomicInteger();
        Scheduler scheduler = new Scheduler();
        Scope scope = new Scope();
        scope.failReservation = true;
        Lease lease = new Lease(pins);
        try {
            scope.start(scheduler, lease);
            throw new AssertionError("reservation OOME should escape");
        } catch (OutOfMemoryError expected) {
            check(scheduler.admissions.get() == 0,
                    "reservation failure admitted executable work");
            check(pins.get() == 0, "reservation failure stranded generation pin");
        }
    }

    private static void cancellationIsNotPhysicalExit() {
        AtomicInteger pins = new AtomicInteger();
        Lease lease = new Lease(pins);
        boolean resultCancelled = true;
        check(resultCancelled, "cancel model setup");
        check(pins.get() == 1,
                "result cancellation must not retire physical generation lease");
        lease.close();
        check(pins.get() == 0, "physical exit must retire exact lease");
    }

    private static void staleEpochAdmissionRace() throws Exception {
        for (int round = 0; round < 2_000; ++round) {
            Router router = new Router();
            Child child = router.child();
            Scheduler scheduler = new Scheduler();
            CountDownLatch go = new CountDownLatch(1);
            AtomicBoolean accepted = new AtomicBoolean();
            AtomicBoolean published = new AtomicBoolean();
            AtomicBoolean stale = new AtomicBoolean();
            Lease[] lease = new Lease[1];

            Thread starter = Thread.ofPlatform().start(() -> {
                try {
                    go.await();
                    lease[0] = router.start(child, scheduler);
                    accepted.set(true);
                } catch (RejectedExecutionException expected) {
                    stale.set(true);
                } catch (InterruptedException interrupted) {
                    Thread.currentThread().interrupt();
                    throw new AssertionError(interrupted);
                }
            });
            Thread publisher = Thread.ofPlatform().start(() -> {
                try {
                    go.await();
                    published.set(router.publishNext());
                } catch (InterruptedException interrupted) {
                    Thread.currentThread().interrupt();
                    throw new AssertionError(interrupted);
                }
            });

            go.countDown();
            starter.join();
            publisher.join();

            check(!(accepted.get() && stale.get()), "start both accepted and stale");
            if (accepted.get()) {
                check(router.pins() == 1, "accepted task lost generation pin");
                check(!published.get(),
                        "route publication crossed a live old-generation pin");
                lease[0].close();
                check(router.publishNext(), "publication did not resume after exit");
            } else {
                check(stale.get(), "start was neither accepted nor rejected stale");
                check(published.get(), "stale start requires publication to have won");
                check(router.pins() == 0, "stale start acquired generation pin");
                check(scheduler.admissions.get() == 0,
                        "stale start reached scheduler admission");
            }
            check(router.epoch() == 2L, "route epoch did not publish exactly once");
        }
    }

    private static void publicDrainQuotaCannotStarveExactReceipt() {
        Cell cell = new Cell();
        long first = cell.admitUntracked();
        check(first == 0L, "first sequence");
        List<Drain> drains = new ArrayList<>();
        for (int i = 0; i < Cell.MAX_PUBLIC_DRAINS; ++i) {
            drains.add(cell.drain());
        }

        Receipt second = cell.admitTracked();
        check(second.sequence == 1L, "tracked sequence");
        cell.process(0L);
        for (Drain drain : drains) check(drain.done, "prefix drain did not settle");
        check(!second.done, "prefix drains forged exact second-message completion");
        cell.process(1L);
        check(second.done && !second.failed, "exact receipt did not settle");
    }

    private static void teardownSnapshotFailurePrecedesStopPublication() {
        Cell cell = new Cell();
        Receipt receipt = cell.admitTracked();
        try {
            cell.terminate(true);
            throw new AssertionError("teardown OOME should escape");
        } catch (OutOfMemoryError expected) {
            check(!cell.stopped(),
                    "failed teardown snapshot published stopped state");
            check(!receipt.done && !receipt.failed,
                    "failed snapshot consumed exact receipt");
        }

        cell.terminate(false);
        check(cell.stopped(), "successful retry did not stop cell");
        check(receipt.failed && !receipt.done,
                "successful teardown did not fail outstanding receipt");
    }

    public static void main(String[] args) throws Exception {
        preAdmissionOwnershipFailure();
        cancellationIsNotPhysicalExit();
        staleEpochAdmissionRace();
        publicDrainQuotaCannotStarveExactReceipt();
        teardownSnapshotFailurePrecedesStopPublication();
        System.out.println("PASS source-free hot-upgrade oracle");
        System.out.println("race_rounds=2000");
        System.out.println("public_drain_slots=1024");
        System.out.println("private_source_bytes=0");
    }
}
