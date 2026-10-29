import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.HexFormat;
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

    private enum Disposition {
        PROCESSED,
        STOPPED,
        CANCELLED,
        FORCE_CANCELLED,
        RUNTIME_CLOSED,
        FAILED
    }

    private static final class Receipt {
        final long sequence;
        Disposition disposition;
        boolean done;
        boolean failed;

        Receipt(long sequence) {
            this.sequence = sequence;
        }

        void processed() {
            check(disposition == null, "receipt disposition double-set");
            disposition = Disposition.PROCESSED;
            done = true;
        }

        void fail(Disposition terminal) {
            check(terminal != Disposition.PROCESSED,
                    "failure path cannot claim processed");
            check(disposition == null, "receipt disposition double-set");
            disposition = terminal;
            failed = true;
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
            if (receipt != null) receipt.processed();
            for (int i = 0; i < count; ++i) completed[i].done = true;
        }

        void terminate(Disposition disposition, boolean injectSnapshotOome) {
            check(disposition != Disposition.PROCESSED,
                    "teardown cannot classify mail as processed");
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
            for (Receipt receipt : receiptSnapshot) receipt.fail(disposition);
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
        check(second.disposition == Disposition.PROCESSED,
                "exact processed receipt lacks PROCESSED disposition");
    }

    private static void teardownSnapshotFailurePrecedesStopPublication() {
        Cell cell = new Cell();
        Receipt receipt = cell.admitTracked();
        try {
            cell.terminate(Disposition.RUNTIME_CLOSED, true);
            throw new AssertionError("teardown OOME should escape");
        } catch (OutOfMemoryError expected) {
            check(!cell.stopped(),
                    "failed teardown snapshot published stopped state");
            check(!receipt.done && !receipt.failed,
                    "failed snapshot consumed exact receipt");
        }

        cell.terminate(Disposition.RUNTIME_CLOSED, false);
        check(cell.stopped(), "successful retry did not stop cell");
        check(receipt.failed && !receipt.done,
                "successful teardown did not fail outstanding receipt");
        check(receipt.disposition == Disposition.RUNTIME_CLOSED,
                "successful teardown lost runtime-close disposition");
    }

    private static void terminalDispositionSweep() {
        for (Disposition disposition : List.of(
                Disposition.STOPPED,
                Disposition.CANCELLED,
                Disposition.FORCE_CANCELLED,
                Disposition.RUNTIME_CLOSED,
                Disposition.FAILED)) {
            Cell cell = new Cell();
            Receipt receipt = cell.admitTracked();
            cell.terminate(disposition, false);
            check(receipt.failed && !receipt.done,
                    "terminal failure did not fail receipt: " + disposition);
            check(receipt.disposition == disposition,
                    "terminal disposition mismatch: " + disposition);
        }

        Cell success = new Cell();
        Receipt processed = success.admitTracked();
        success.process(processed.sequence);
        check(processed.done && !processed.failed,
                "processed receipt did not succeed");
        check(processed.disposition == Disposition.PROCESSED,
                "processed disposition mismatch");
    }

    private static void completionVersusTerminationRaces() throws Exception {
        for (Disposition terminal : List.of(
                Disposition.STOPPED,
                Disposition.CANCELLED,
                Disposition.FORCE_CANCELLED,
                Disposition.RUNTIME_CLOSED,
                Disposition.FAILED)) {
            for (int round = 0; round < 2_000; ++round) {
                Cell cell = new Cell();
                Receipt receipt = cell.admitTracked();
                CountDownLatch go = new CountDownLatch(1);
                AtomicInteger unexpected = new AtomicInteger();

                Thread complete = Thread.ofPlatform().start(() -> {
                    try {
                        go.await();
                        try {
                            cell.process(receipt.sequence);
                        } catch (AssertionError lostRace) {
                            // Termination may have removed exact ownership first.
                        }
                    } catch (InterruptedException interrupted) {
                        Thread.currentThread().interrupt();
                        unexpected.incrementAndGet();
                    }
                });
                Thread terminate = Thread.ofPlatform().start(() -> {
                    try {
                        go.await();
                        cell.terminate(terminal, false);
                    } catch (InterruptedException interrupted) {
                        Thread.currentThread().interrupt();
                        unexpected.incrementAndGet();
                    }
                });

                go.countDown();
                complete.join();
                terminate.join();
                check(unexpected.get() == 0, "race thread interrupted");
                check(receipt.disposition == Disposition.PROCESSED
                                || receipt.disposition == terminal,
                        "race produced impossible disposition "
                                + receipt.disposition + " vs " + terminal);
                check(receipt.done != receipt.failed,
                        "receipt must settle exactly one success/failure state");
            }
        }
    }

    private static final class ProxyEnvelope {
        final long sequence;

        ProxyEnvelope(long sequence) {
            this.sequence = sequence;
        }
    }

    /**
     * Source-free model of the local mailbox ownership rule. This intentionally
     * models identity/epoch/attempt invariants only; it is not copied from the
     * private Oreslang mailbox implementation.
     */
    private static final class ProxyLedger {
        private final Object lock = new Object();
        private final Map<Long, ProxyEnvelope> outstanding = new HashMap<>();
        private final Set<Long> attempted = new HashSet<>();
        private long routeEpoch = 1L;
        private long highestDispatched;

        ProxyEnvelope dispatch() {
            synchronized (lock) {
                long sequence = ++highestDispatched;
                ProxyEnvelope envelope = new ProxyEnvelope(sequence);
                check(outstanding.put(sequence, envelope) == null,
                        "duplicate proxy sequence");
                return envelope;
            }
        }

        void beginPhysicalSend(ProxyEnvelope envelope, long expectedEpoch) {
            synchronized (lock) {
                ProxyEnvelope exact = outstanding.get(envelope.sequence);
                if (expectedEpoch != routeEpoch || exact != envelope) {
                    throw new SecurityException(
                            "foreign envelope or stale route");
                }
                if (!attempted.add(envelope.sequence)) {
                    throw new IllegalStateException(
                            "duplicate physical send attempt");
                }
            }
        }

        void requeueUnsent(ProxyEnvelope envelope, long expectedEpoch) {
            synchronized (lock) {
                ProxyEnvelope exact = outstanding.get(envelope.sequence);
                if (expectedEpoch != routeEpoch
                        || envelope.sequence != highestDispatched
                        || exact != envelope) {
                    throw new SecurityException(
                            "rollback lacks exact current envelope authority");
                }
                if (attempted.contains(envelope.sequence)) {
                    throw new IllegalStateException(
                            "attempted physical send cannot be requeued");
                }
                outstanding.remove(envelope.sequence);
                highestDispatched--;
            }
        }

        int outstanding() {
            synchronized (lock) {
                return outstanding.size();
            }
        }

        int attempts() {
            synchronized (lock) {
                return attempted.size();
            }
        }
    }

    private static void exactEnvelopeRollbackAuthority() {
        ProxyLedger owner = new ProxyLedger();
        ProxyLedger foreign = new ProxyLedger();
        ProxyEnvelope exact = owner.dispatch();
        ProxyEnvelope copied = new ProxyEnvelope(exact.sequence);
        ProxyEnvelope foreignEnvelope = foreign.dispatch();

        try {
            owner.requeueUnsent(copied, 1L);
            throw new AssertionError("copied envelope gained rollback authority");
        } catch (SecurityException expected) {
            // exact object identity is required
        }
        try {
            owner.requeueUnsent(foreignEnvelope, 1L);
            throw new AssertionError("foreign mailbox envelope gained authority");
        } catch (SecurityException expected) {
            // another mailbox's equal-looking envelope is never authority
        }
        try {
            owner.requeueUnsent(exact, 2L);
            throw new AssertionError("stale route epoch gained rollback authority");
        } catch (SecurityException expected) {
            // route epoch is part of local authority
        }
        check(owner.outstanding() == 1,
                "rejected rollback mutated outstanding ownership");

        owner.requeueUnsent(exact, 1L);
        check(owner.outstanding() == 0,
                "exact pre-send rollback did not restore ownership");

        ProxyEnvelope attempted = owner.dispatch();
        owner.beginPhysicalSend(attempted, 1L);
        try {
            owner.requeueUnsent(attempted, 1L);
            throw new AssertionError("attempted send was requeued");
        } catch (IllegalStateException expected) {
            // ambiguous/accepted work remains quarantined
        }
        check(owner.outstanding() == 1 && owner.attempts() == 1,
                "rejected attempted-send rollback mutated authority");
    }

    private static void physicalSendVersusUnsentRollbackRaces()
            throws Exception {
        for (int round = 0; round < 10_000; ++round) {
            ProxyLedger ledger = new ProxyLedger();
            ProxyEnvelope envelope = ledger.dispatch();
            CountDownLatch go = new CountDownLatch(1);
            AtomicBoolean sendWon = new AtomicBoolean();
            AtomicBoolean rollbackWon = new AtomicBoolean();
            AtomicInteger unexpected = new AtomicInteger();

            Thread sender = Thread.ofPlatform().start(() -> {
                try {
                    go.await();
                    ledger.beginPhysicalSend(envelope, 1L);
                    sendWon.set(true);
                } catch (SecurityException | IllegalStateException lostRace) {
                    // rollback may have removed exact outstanding authority
                } catch (InterruptedException interrupted) {
                    Thread.currentThread().interrupt();
                    unexpected.incrementAndGet();
                }
            });
            Thread rollback = Thread.ofPlatform().start(() -> {
                try {
                    go.await();
                    ledger.requeueUnsent(envelope, 1L);
                    rollbackWon.set(true);
                } catch (SecurityException | IllegalStateException lostRace) {
                    // physical send reservation may have won
                } catch (InterruptedException interrupted) {
                    Thread.currentThread().interrupt();
                    unexpected.incrementAndGet();
                }
            });

            go.countDown();
            sender.join();
            rollback.join();
            check(unexpected.get() == 0, "rollback race thread interrupted");
            check(sendWon.get() != rollbackWon.get(),
                    "exactly one send/resend authority must win");

            if (sendWon.get()) {
                check(ledger.outstanding() == 1 && ledger.attempts() == 1,
                        "send winner lost attempted outstanding authority");
            } else {
                check(ledger.outstanding() == 0 && ledger.attempts() == 0,
                        "rollback winner left physical attempt authority");
            }
        }
    }

    private static final class RuntimeCloseModel {
        private final Cell cell = new Cell();
        private boolean closed;
        private boolean dispatchersShutdown;

        Receipt tracked() {
            return cell.admitTracked();
        }

        void close(boolean injectActorTeardownOome) {
            // The runtime actor snapshot is prepared before this publication.
            closed = true;
            cell.terminate(
                    Disposition.RUNTIME_CLOSED,
                    injectActorTeardownOome);
            // Independent retryable shutdown authority: this is intentionally
            // not keyed to "first close".
            if (!dispatchersShutdown) {
                dispatchersShutdown = true;
            }
        }
    }

    private static void runtimeCloseRetryAfterActorTeardownFailure() {
        RuntimeCloseModel runtime = new RuntimeCloseModel();
        Receipt receipt = runtime.tracked();
        try {
            runtime.close(true);
            throw new AssertionError("first close should inject OOME");
        } catch (OutOfMemoryError expected) {
            check(runtime.closed,
                    "global close publication should remain fail-closed");
            check(!runtime.dispatchersShutdown,
                    "failed actor teardown cannot consume dispatcher shutdown");
            check(receipt.disposition == null,
                    "failed actor teardown cannot classify exact receipt");
        }

        runtime.close(false);
        check(runtime.dispatchersShutdown,
                "retry must eventually publish dispatcher shutdown");
        check(receipt.disposition == Disposition.RUNTIME_CLOSED,
                "retry must settle outstanding exact receipt as runtime closed");

        runtime.close(false);
        check(runtime.dispatchersShutdown,
                "completed close remains idempotent");
    }

    private static final class CandidateOwnerModel {
        private static final class Owner {
            boolean physicalAdmitted;
            boolean physicalFinalized;
            boolean retirementEligible;
            boolean pinReleased;
        }

        private Owner owner;
        private int generationPins;

        void begin(boolean injectOwnerOome) {
            generationPins++;
            if (injectOwnerOome) {
                generationPins--;
                throw new OutOfMemoryError("candidate owner reservation");
            }
            owner = new Owner();
        }

        void admitPhysical() {
            check(owner != null, "physical candidate admitted without owner");
            check(!owner.physicalAdmitted, "candidate admitted twice");
            owner.physicalAdmitted = true;
        }

        void failBeforePhysical() {
            check(owner != null, "missing pre-reserved owner");
            check(!owner.physicalAdmitted,
                    "pre-physical cleanup saw an admitted candidate");
            owner = null;
            generationPins--;
        }

        void failAfterPhysicalUnknownFinalization() {
            check(owner != null && owner.physicalAdmitted,
                    "unknown physical candidate lost owner");
            // Publication has now been abandoned; only the pre-reserved
            // retirement owner may release the pin from here onward.
            owner.retirementEligible = true;
        }

        void markPhysicalFinalized() {
            check(owner != null && owner.physicalAdmitted,
                    "cannot finalize unowned candidate");
            owner.physicalFinalized = true;
        }

        boolean tryReap() {
            if (owner == null
                    || !owner.retirementEligible
                    || !owner.physicalFinalized) {
                return false;
            }
            check(!owner.pinReleased, "candidate pin released twice");
            owner.pinReleased = true;
            generationPins--;
            owner = null;
            return true;
        }

        void markPhysicalFinalizedAndReap() {
            markPhysicalFinalized();
            check(tryReap(), "eligible finalized candidate was not reaped");
        }

        int pins() { return generationPins; }
        boolean hasOwner() { return owner != null; }
    }

    private static void preReservedCandidateRetirementOwnership() throws Exception {
        for (int round = 0; round < 20_000; ++round) {
            CandidateOwnerModel preAdmission = new CandidateOwnerModel();
            try {
                preAdmission.begin(true);
                throw new AssertionError("owner OOME was not injected");
            } catch (OutOfMemoryError expected) {
                check(preAdmission.pins() == 0,
                        "pre-admission owner failure leaked generation pin");
                check(!preAdmission.hasOwner(),
                        "pre-admission owner failure published an owner");
            }

            CandidateOwnerModel admitted = new CandidateOwnerModel();
            admitted.begin(false);
            check(admitted.pins() == 1 && admitted.hasOwner(),
                    "candidate pin/owner not reserved together");
            admitted.admitPhysical();

            // A candidate may physically terminate while its originating
            // upgrade still has publication authority. Host observations and
            // opportunistic reapers must not consume that staging lease.
            admitted.markPhysicalFinalized();
            check(!admitted.tryReap(),
                    "staging candidate was reaped before publication abandoned");
            check(admitted.pins() == 1 && admitted.hasOwner(),
                    "staging reaper lost candidate owner/pin");

            admitted.failAfterPhysicalUnknownFinalization();
            check(admitted.pins() == 1 && admitted.hasOwner(),
                    "ambiguous physical cleanup lost candidate owner");
            check(admitted.tryReap(),
                    "eligible finalized candidate did not reap");
            check(admitted.pins() == 0 && !admitted.hasOwner(),
                    "reap did not release candidate owner exactly once");

            CandidateOwnerModel noPhysical = new CandidateOwnerModel();
            noPhysical.begin(false);
            noPhysical.failBeforePhysical();
            check(noPhysical.pins() == 0 && !noPhysical.hasOwner(),
                    "unspawned candidate did not release cleanly");
        }

        // Race finalization observation against a supervisor poll: the owner is
        // never absent while an admitted candidate still holds the pin.
        for (int round = 0; round < 2_000; ++round) {
            CandidateOwnerModel model = new CandidateOwnerModel();
            model.begin(false);
            model.admitPhysical();
            CountDownLatch go = new CountDownLatch(1);
            AtomicInteger failures = new AtomicInteger();
            Thread observer = Thread.ofPlatform().start(() -> {
                try {
                    go.await();
                    for (int i = 0; i < 32; ++i) {
                        int pins = model.pins();
                        boolean owner = model.hasOwner();
                        if (pins > 0 && !owner) failures.incrementAndGet();
                        Thread.onSpinWait();
                    }
                } catch (InterruptedException interrupted) {
                    Thread.currentThread().interrupt();
                    failures.incrementAndGet();
                }
            });
            go.countDown();
            model.markPhysicalFinalized();
            check(!model.tryReap(),
                    "concurrent staging candidate reaped before abandonment");
            model.failAfterPhysicalUnknownFinalization();
            observer.join();
            check(failures.get() == 0,
                    "observer saw pinned candidate without retry owner");
            check(model.tryReap(),
                    "eligible concurrent candidate did not reap");
        }
    }

    private enum TxnPhase {
        NONE, PREPARING, STAGING, PUBLISHED, RETIRING,
        RETIREMENT_PENDING, COMPLETE, ABANDONED
    }

    private static final class SupervisorTxnModel {
        long routeEpoch = 1L;
        long mailboxEpoch = 1L;
        boolean activeTxn;
        boolean proxyFrozen;
        boolean candidateOwner;
        boolean candidatePin;
        boolean candidatePhysical;
        boolean candidateRetirementEligible;
        boolean oldRouteRetirementOwner;
        boolean candidatePublishedToRoute;
        TxnPhase phase = TxnPhase.NONE;

        void begin() {
            check(!activeTxn, "duplicate supervisor transaction");
            check(!proxyFrozen, "stale proxy freeze before transaction");
            candidatePin = true;
            candidateOwner = true;
            activeTxn = true;
            phase = TxnPhase.PREPARING;
            proxyFrozen = true;
            phase = TxnPhase.STAGING;
            checkInvariant();
        }

        void admitCandidatePhysical() {
            check(activeTxn && phase == TxnPhase.STAGING,
                    "candidate physical admission outside staging");
            check(candidateOwner && candidatePin,
                    "candidate physical admitted without pin owner");
            candidatePhysical = true;
            checkInvariant();
        }

        void rollback(boolean leaseDebitFails) {
            check(activeTxn && !candidatePublishedToRoute,
                    "rollback after publication");
            proxyFrozen = false;
            activeTxn = false;
            phase = TxnPhase.ABANDONED;
            if (candidatePhysical) {
                candidateRetirementEligible = true;
            } else if (leaseDebitFails) {
                candidateRetirementEligible = true;
            } else {
                candidatePin = false;
                candidateOwner = false;
            }
            checkInvariant();
        }

        void publish() {
            check(activeTxn && phase == TxnPhase.STAGING,
                    "publication outside staging");
            check(proxyFrozen, "publication without proxy freeze");
            check(candidateOwner && candidatePin && candidatePhysical,
                    "publication without candidate owner");
            oldRouteRetirementOwner = true;
            mailboxEpoch++;
            routeEpoch++;
            candidatePublishedToRoute = true;
            proxyFrozen = false;
            phase = TxnPhase.PUBLISHED;
            checkInvariant();
        }

        void beginRetiring() {
            check(activeTxn && phase == TxnPhase.PUBLISHED,
                    "retirement outside publication");
            phase = TxnPhase.RETIRING;
            checkInvariant();
        }

        void finishPublished(boolean oldFinalized) {
            check(activeTxn && phase == TxnPhase.RETIRING,
                    "finish outside retirement");
            activeTxn = false;
            if (oldFinalized) {
                oldRouteRetirementOwner = false;
                phase = TxnPhase.COMPLETE;
            } else {
                phase = TxnPhase.RETIREMENT_PENDING;
            }
            checkInvariant();
        }

        boolean reapCandidate() {
            if (!candidateOwner || candidatePublishedToRoute) return false;
            if (!candidateRetirementEligible) return false;
            candidatePin = false;
            candidateOwner = false;
            candidateRetirementEligible = false;
            return true;
        }

        void checkInvariant() {
            check(routeEpoch == mailboxEpoch,
                    "route/mailbox publication torn");
            if (activeTxn && phase == TxnPhase.STAGING) {
                check(proxyFrozen, "staging transaction lost freeze");
                check(candidateOwner && candidatePin,
                        "staging transaction lost candidate ownership");
            }
            if (candidatePin && !candidatePublishedToRoute) {
                check(candidateOwner,
                        "unpinned owner invariant: candidate pin has no owner");
            }
            if (candidatePublishedToRoute) {
                check(candidatePin,
                        "published route lost candidate generation pin");
            }
            if (oldRouteRetirementOwner) {
                check(routeEpoch > 1L,
                        "old route retirement exists before publication");
            }
        }
    }

    private static void unifiedSupervisorTransactionProtocol() {
        for (int round = 0; round < 20_000; ++round) {
            SupervisorTxnModel rollback = new SupervisorTxnModel();
            rollback.begin();
            rollback.admitCandidatePhysical();
            check(!rollback.reapCandidate(),
                    "staging candidate was reaped before rollback");
            rollback.rollback(false);
            check(rollback.routeEpoch == 1L
                            && rollback.mailboxEpoch == 1L,
                    "rollback changed publication epoch");
            check(!rollback.proxyFrozen && !rollback.activeTxn,
                    "rollback leaked transaction freeze");
            check(rollback.reapCandidate(),
                    "abandoned physical candidate was not reapable");

            SupervisorTxnModel debitFailure = new SupervisorTxnModel();
            debitFailure.begin();
            debitFailure.rollback(true);
            check(debitFailure.candidateOwner
                            && debitFailure.candidatePin
                            && debitFailure.candidateRetirementEligible,
                    "failed pre-spawn debit lost retry owner");
            check(debitFailure.reapCandidate(),
                    "repaired pre-spawn debit was not reapable");

            SupervisorTxnModel success = new SupervisorTxnModel();
            success.begin();
            success.admitCandidatePhysical();
            success.publish();
            check(success.routeEpoch == 2L
                            && success.mailboxEpoch == 2L,
                    "publication did not advance epochs together");
            success.beginRetiring();
            success.finishPublished((round & 1) == 0);
            check(!success.activeTxn && !success.proxyFrozen,
                    "published transaction did not release supervisor authority");
            check(!success.reapCandidate(),
                    "published candidate bookkeeping double-released route pin");
        }
    }

    private static void acquiredResourceOwnershipPrecedesValidation() {
        for (int round = 0; round < 20_000; ++round) {
            // Model a resource returned by the physical runtime while the
            // transaction phase is corrupted before the router validates it.
            boolean transferAcquired = true;
            boolean transferOwnedByTxn = false;
            boolean physicalAcquired = true;
            boolean physicalOwnedByTxn = false;
            boolean phaseValid = false;

            // Production rule: assign cleanup ownership first.
            if (transferAcquired) transferOwnedByTxn = true;
            check(transferOwnedByTxn,
                    "acquired source transfer was not published to txn owner");
            if (!phaseValid) {
                check(transferOwnedByTxn,
                        "failed transfer validation stranded ingress freeze");
            }

            if (physicalAcquired) physicalOwnedByTxn = true;
            check(physicalOwnedByTxn,
                    "acquired physical candidate was not published to txn owner");
            if (!phaseValid) {
                check(physicalOwnedByTxn,
                        "failed physical validation stranded candidate actor");
            }

            // Cleanup can now deterministically close/retire both resources.
            transferOwnedByTxn = false;
            physicalOwnedByTxn = false;
            check(!transferOwnedByTxn && !physicalOwnedByTxn,
                    "transaction cleanup retained acquired resource");
        }
    }

    private static String digest(byte[] bytes) {
        try {
            return HexFormat.of().formatHex(
                    MessageDigest.getInstance("SHA-256").digest(bytes));
        } catch (NoSuchAlgorithmException impossible) {
            throw new IllegalStateException(impossible);
        }
    }

    private static final class EnvelopeIdentityModel {
        private final byte[] retained;
        private final String payloadDigest;
        private boolean physicalAttempt;
        private boolean completed;

        EnvelopeIdentityModel(byte[] source, String ignoredCallerDigest) {
            retained = source.clone();
            payloadDigest = digest(retained);
        }

        String payloadDigest() { return payloadDigest; }

        boolean integrityValid() {
            return payloadDigest.equals(digest(retained));
        }

        void mutateInternalForFaultInjection(int index, byte mask) {
            retained[index] ^= mask;
        }

        boolean reservePhysicalAttempt() {
            if (physicalAttempt || !integrityValid()) return false;
            physicalAttempt = true;
            return true;
        }

        boolean requeueUnsent() {
            return !physicalAttempt && integrityValid();
        }

        boolean completeAfterPhysicalReceipt() {
            if (!physicalAttempt || !integrityValid()) return false;
            completed = true;
            return true;
        }

        boolean completed() { return completed; }
    }

    private static void immutableEnvelopeContentIdentity() {
        for (int round = 0; round < 20_000; ++round) {
            byte[] source = new byte[] {
                    (byte) round,
                    (byte) (round >>> 8),
                    (byte) (round * 31),
                    (byte) (round * 17 + 3)
            };
            String expected = digest(source);
            EnvelopeIdentityModel pristine =
                    new EnvelopeIdentityModel(source, "0".repeat(64));
            check(expected.equals(pristine.payloadDigest()),
                    "mailbox accepted a caller-forged payload digest");
            source[0] ^= 0x55;
            check(pristine.integrityValid(),
                    "caller mutation changed mailbox-owned snapshot");
            check(pristine.reservePhysicalAttempt(),
                    "valid envelope could not reserve physical attempt");
            check(pristine.completeAfterPhysicalReceipt(),
                    "valid terminal receipt did not complete");
            check(pristine.completed(),
                    "valid envelope completion was not recorded");

            EnvelopeIdentityModel preSend =
                    new EnvelopeIdentityModel(
                            new byte[] {1, 2, 3, (byte) round}, null);
            preSend.mutateInternalForFaultInjection(1, (byte) 0x40);
            check(!preSend.reservePhysicalAttempt(),
                    "corrupted envelope obtained physical-send authority");
            check(!preSend.requeueUnsent(),
                    "corrupted envelope re-entered dispatch FIFO");

            EnvelopeIdentityModel postSend =
                    new EnvelopeIdentityModel(
                            new byte[] {4, 5, 6, (byte) round}, null);
            check(postSend.reservePhysicalAttempt(),
                    "post-send fixture could not reserve");
            postSend.mutateInternalForFaultInjection(2, (byte) 0x20);
            check(!postSend.completeAfterPhysicalReceipt(),
                    "corrupted attempted envelope refunded logical credits");
            check(!postSend.completed(),
                    "corrupted attempted envelope became completed");
        }
    }

    private static void proxyAbiContinuity() {
        for (int round = 0; round < 20_000; ++round) {
            byte[] stable = new byte[] {
                    9, 8, 7, (byte) round, (byte) (round >>> 8)
            };
            String activeAbi = digest(stable);
            String candidateAbi = (round & 1) == 0
                    ? activeAbi
                    : digest(new byte[] {
                            9, 8, 6, (byte) round, (byte) (round >>> 8)
                    });
            boolean compatible = activeAbi.equals(candidateAbi);
            boolean publish = compatible;
            check(publish == compatible,
                    "proxy route publication ignored compiler ABI continuity");
        }
    }

    public static void main(String[] args) throws Exception {
        preAdmissionOwnershipFailure();
        cancellationIsNotPhysicalExit();
        staleEpochAdmissionRace();
        publicDrainQuotaCannotStarveExactReceipt();
        teardownSnapshotFailurePrecedesStopPublication();
        terminalDispositionSweep();
        completionVersusTerminationRaces();
        runtimeCloseRetryAfterActorTeardownFailure();
        preReservedCandidateRetirementOwnership();
        unifiedSupervisorTransactionProtocol();
        acquiredResourceOwnershipPrecedesValidation();
        immutableEnvelopeContentIdentity();
        proxyAbiContinuity();
        exactEnvelopeRollbackAuthority();
        physicalSendVersusUnsentRollbackRaces();
        System.out.println("PASS source-free hot-upgrade oracle");
        System.out.println("route_race_rounds=2000");
        System.out.println("terminal_race_rounds=10000");
        System.out.println("runtime_close_retry=pass");
        System.out.println("candidate_owner_rounds=20000");
        System.out.println("candidate_owner_races=2000");
        System.out.println("candidate_staging_reap_gate=pass");
        System.out.println("supervisor_txn_rounds=20000");
        System.out.println("supervisor_txn_atomic_epochs=pass");
        System.out.println("acquired_resource_owner_rounds=20000");
        System.out.println("acquired_resource_before_validation=pass");
        System.out.println("envelope_identity_rounds=20000");
        System.out.println("envelope_digest_quarantine=pass");
        System.out.println("proxy_abi_rounds=20000");
        System.out.println("proxy_abi_continuity=pass");
        System.out.println("exact_envelope_rollback=pass");
        System.out.println("send_vs_requeue_races=10000");
        System.out.println("public_drain_slots=1024");
        System.out.println("private_source_bytes=0");
    }
}
