import java.util.concurrent.*;
import java.util.concurrent.atomic.*;
import java.util.*;

/** Independently written virtual-thread lifetime oracle; no Oreslang source. */
public final class TaskExitOracle {
    static final AtomicInteger checks = new AtomicInteger();
    static void check(boolean valid, String label) {
        if (!valid) throw new AssertionError(label);
        checks.incrementAndGet();
    }
    static final class Runtime implements AutoCloseable {
        final ExecutorService carriers = Executors.newVirtualThreadPerTaskExecutor();
        final AtomicBoolean closed = new AtomicBoolean();
        final ConcurrentHashMap<Future<?>, Exit> pending = new ConcurrentHashMap<>();
        final long maxMillis;
        static final class Exit {
            final AtomicInteger state = new AtomicInteger();
            final CountDownLatch finished = new CountDownLatch(1);
            volatile Future<?> carrier;
            void cancel() {
                Future<?> f = carrier;
                if (f != null) f.cancel(true);
                if (state.compareAndSet(0, 2)) finished.countDown();
            }
        }
        Runtime(long millis) { maxMillis = millis; }
        Future<?> submit(Runnable action) {
            if (closed.get()) throw new RejectedExecutionException();
            Exit exit = new Exit();
            Future<?> carrier = carriers.submit(() -> {
                if (!exit.state.compareAndSet(0, 1)) return;
                try { action.run(); }
                finally { exit.state.set(2); exit.finished.countDown(); }
            });
            exit.carrier = carrier;
            pending.put(carrier, exit);
            if (closed.get()) exit.cancel();
            return carrier;
        }
        @Override public void close() {
            if (closed.compareAndSet(false, true)) {
                for (Exit exit : pending.values()) exit.cancel();
                carriers.shutdownNow();
            }
            long deadline = System.nanoTime() + TimeUnit.MILLISECONDS.toNanos(maxMillis);
            boolean interrupted = false;
            try {
                for (;;) {
                    long left = deadline - System.nanoTime();
                    if (left <= 0 && !carriers.isTerminated())
                        throw new IllegalStateException("executor not terminated");
                    try {
                        if (carriers.awaitTermination(Math.max(0, left),
                                TimeUnit.NANOSECONDS)) break;
                    } catch (InterruptedException ignore) { interrupted = true; }
                }
                // Executor termination is not a proof of exit after cancellation.
                for (Exit exit : pending.values()) {
                    for (;;) {
                        long left = deadline - System.nanoTime();
                        if (left <= 0 && exit.finished.getCount() > 0)
                            throw new IllegalStateException("physical Task exit incomplete");
                        try {
                            if (exit.finished.await(Math.max(0, left),
                                    TimeUnit.NANOSECONDS)) break;
                        } catch (InterruptedException ignore) { interrupted = true; }
                    }
                }
            } finally {
                if (interrupted) Thread.currentThread().interrupt();
            }
        }
    }
    static void awaitRelease(CountDownLatch release) {
        boolean interrupted = false;
        try {
            while (release.getCount() > 0) {
                try { release.await(); }
                catch (InterruptedException ignore) { interrupted = true; }
            }
        } finally { if (interrupted) Thread.currentThread().interrupt(); }
    }
    static void concurrentClosers() throws Exception {
        for (int i=0; i<40; i++) {
            Runtime rt = new Runtime(750);
            CountDownLatch started = new CountDownLatch(1);
            CountDownLatch cleaning = new CountDownLatch(1);
            CountDownLatch release = new CountDownLatch(1);
            Future<?> f = rt.submit(() -> {
                started.countDown();
                try { Thread.sleep(30_000); }
                catch (InterruptedException expected) { }
                finally { cleaning.countDown(); awaitRelease(release); }
            });
            check(started.await(2, TimeUnit.SECONDS), "started");
            AtomicReference<Throwable> e1 = new AtomicReference<>();
            AtomicReference<Throwable> e2 = new AtomicReference<>();
            AtomicBoolean restored = new AtomicBoolean();
            Thread a = Thread.ofVirtual().start(() -> {
                try { rt.close(); } catch (Throwable t) { e1.set(t); }
            });
            try {
                check(cleaning.await(2, TimeUnit.SECONDS), "finally entered");
                CountDownLatch secondFinished = new CountDownLatch(1);
                Thread b = Thread.ofVirtual().start(() -> {
                    Thread.currentThread().interrupt();
                    try { rt.close(); } catch (Throwable t) { e2.set(t); }
                    finally {
                        restored.set(Thread.currentThread().isInterrupted());
                        secondFinished.countDown();
                    }
                });
                try {
                    check(!secondFinished.await(12, TimeUnit.MILLISECONDS),
                            "second close must wait for physical exit");
                } finally { release.countDown(); b.join(2_000); }
                check(e2.get() == null, "second closer failure");
                check(restored.get(), "interrupt restored");
                check(f.isDone(), "Future settled");
            } finally { release.countDown(); a.join(2_000); }
            check(e1.get() == null, "first closer failure");
            check(rt.carriers.isTerminated(), "executor terminated");
        }
    }
    static void timeoutRetry() throws Exception {
        Runtime rt = new Runtime(150);
        CountDownLatch started = new CountDownLatch(1);
        CountDownLatch cleaning = new CountDownLatch(1);
        CountDownLatch release = new CountDownLatch(1);
        rt.submit(() -> {
            started.countDown();
            try { Thread.sleep(30_000); }
            catch (InterruptedException ignore) { }
            finally { cleaning.countDown(); awaitRelease(release); }
        });
        check(started.await(2, TimeUnit.SECONDS), "retry worker started");
        AtomicReference<Throwable> firstFailure = new AtomicReference<>();
        Thread a = Thread.ofVirtual().start(() -> {
            try { rt.close(); } catch (Throwable t) { firstFailure.set(t); }
        });
        check(cleaning.await(2, TimeUnit.SECONDS), "retry worker cleaning");
        a.join(2_000);
        check(firstFailure.get() instanceof IllegalStateException,
                "first close must fail closed on timeout");
        CountDownLatch done = new CountDownLatch(1);
        AtomicReference<Throwable> retryFailure = new AtomicReference<>();
        Thread b = Thread.ofVirtual().start(() -> {
            try { rt.close(); } catch (Throwable t) { retryFailure.set(t); }
            finally { done.countDown(); }
        });
        try {
            check(!done.await(25, TimeUnit.MILLISECONDS),
                    "retry must wait even if closed bit already set");
        } finally { release.countDown(); b.join(2_000); }
        check(retryFailure.get() == null, "retry joins cleanup");
        check(rt.carriers.isTerminated(), "retry physical quiescence");
    }
    static void fastTaskRaces() throws Exception {
        for (int i=0; i<250; i++) {
            Runtime rt = new Runtime(750);
            for (int j=0; j<3; j++) {
                try { rt.submit(() -> { }); }
                catch (RejectedExecutionException expected) { }
            }
            AtomicReference<Throwable> error = new AtomicReference<>();
            Thread a = Thread.ofVirtual().start(() -> {
                try { rt.close(); } catch (Throwable t) { error.set(t); }
            });
            Thread b = Thread.ofVirtual().start(() -> {
                try { rt.close(); } catch (Throwable t) { error.set(t); }
            });
            a.join(2_000); b.join(2_000);
            check(!a.isAlive() && !b.isAlive(), "closers joined");
            check(error.get() == null, "no error");
            check(rt.carriers.isTerminated(), "no carrier");
            check(rt.closed.get(), "no admission");
        }
    }
    public static void main(String[] args) throws Exception {
        concurrentClosers();
        timeoutRetry();
        fastTaskRaces();
        System.out.println("SOURCE_FREE_JAVA_TASK_EXIT_PASS checks=" + checks.get()
                + " concurrent=40 retries=1 fastRaces=250 export=false");
    }
}
