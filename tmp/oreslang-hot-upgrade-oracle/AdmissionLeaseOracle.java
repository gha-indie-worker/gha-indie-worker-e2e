import java.util.concurrent.*;
import java.util.concurrent.atomic.*;

/** Independent public protocol model; contains no private Oreslang source. */
public final class AdmissionLeaseOracle {
    static final class Ref {
        final int id;
        final AtomicBoolean finalized = new AtomicBoolean();
        final AtomicBoolean pinReleased = new AtomicBoolean();
        Ref(int id) { this.id = id; }
    }
    static final class State {
        final ConcurrentHashMap<Integer, Ref> registered = new ConcurrentHashMap<>();
        final AtomicInteger actorSlots = new AtomicInteger(1);
        final AtomicInteger codePins = new AtomicInteger(1);
        Ref exactSupervisorOwner;
        Ref failedAdmission(int id, boolean failBeforeRemoval) {
            Ref ref = new Ref(id);
            exactSupervisorOwner = ref;
            registered.put(id, ref);
            // Simulate failure after insertion and before ActorRef return.
            if (!failBeforeRemoval) registered.remove(id, ref);
            if (registered.get(id) != ref) {
                actorSlots.decrementAndGet();
                ref.finalized.set(true);
            }
            return ref;
        }
        void retire(Ref ref) {
            if (registered.remove(ref.id, ref)) {
                actorSlots.decrementAndGet();
                ref.finalized.set(true);
            }
            if (ref.finalized.get()
                    && ref.pinReleased.compareAndSet(false, true))
                codePins.decrementAndGet();
        }
    }
    static void verify(boolean ok, String why) {
        if (!ok) throw new AssertionError(why);
    }
    static void negativeControl() throws Exception {
        AtomicInteger unguarded = new AtomicInteger(1);
        CountDownLatch allObserved = new CountDownLatch(4);
        CountDownLatch proceed = new CountDownLatch(1);
        ExecutorService pool = Executors.newFixedThreadPool(4);
        try {
            Future<?>[] futures = new Future<?>[4];
            for (int i = 0; i < futures.length; i++) {
                futures[i] = pool.submit(() -> {
                    boolean sawPin = unguarded.get() > 0;
                    allObserved.countDown();
                    try { proceed.await(); }
                    catch (InterruptedException error) {
                        Thread.currentThread().interrupt();
                        throw new AssertionError(error);
                    }
                    if (sawPin) unguarded.decrementAndGet();
                });
            }
            verify(allObserved.await(10, TimeUnit.SECONDS), "negative control hung");
            proceed.countDown();
            for (Future<?> future : futures) future.get();
            verify(unguarded.get() == -3, "old non-atomic retirement should underflow");
        } finally { pool.shutdownNow(); }
    }
    public static void main(String[] args) throws Exception {
        long assertions = 0;
        for (int n = 0; n < 20000; n++) {
            for (boolean failedRemoval : new boolean[] {false, true}) {
                State s = new State();
                Ref ref = s.failedAdmission(n, failedRemoval);
                verify(ref == s.exactSupervisorOwner, "ownership handoff"); assertions++;
                if (failedRemoval) {
                    verify(s.actorSlots.get() == 1, "slot still registered"); assertions++;
                    verify(!ref.finalized.get(), "no early finalization"); assertions++;
                } else {
                    verify(s.actorSlots.get() == 0, "absent entry slot"); assertions++;
                    verify(ref.finalized.get(), "absent entry finalization"); assertions++;
                }
                s.retire(ref);
                s.retire(ref);
                verify(s.actorSlots.get() == 0, "exact slot retired once"); assertions++;
                verify(s.codePins.get() == 0, "exact code pin retired once"); assertions++;
                verify(ref.pinReleased.get(), "atomic ownership claim"); assertions++;
            }
        }
        ExecutorService pool = Executors.newFixedThreadPool(4);
        try {
            for (int n = 0; n < 5000; n++) {
                State state = new State();
                Ref ref = state.failedAdmission(n, true);
                CountDownLatch gate = new CountDownLatch(1);
                Future<?>[] work = new Future<?>[4];
                for (int i = 0; i < work.length; i++) {
                    work[i] = pool.submit(() -> {
                        try { gate.await(); }
                        catch (InterruptedException err) {
                            Thread.currentThread().interrupt();
                            throw new AssertionError(err);
                        }
                        state.retire(ref);
                    });
                }
                gate.countDown();
                for (Future<?> future : work) future.get();
                verify(state.registered.isEmpty(), "ghost registration"); assertions++;
                verify(state.actorSlots.get() == 0, "slot exactly once"); assertions++;
                verify(state.codePins.get() == 0, "pin exactly once"); assertions++;
                verify(ref.finalized.get() && ref.pinReleased.get(), "terminal witness"); assertions++;
            }
        } finally { pool.shutdownNow(); }
        negativeControl(); assertions++;
        System.out.println("PASS source-free admission lease oracle assertions="
                + assertions + " race_rounds=5000 workers=4 "
                + "unsafe_check_then_debit_negative_control=detected "
                + "private_source_bytes=0");
    }
}
