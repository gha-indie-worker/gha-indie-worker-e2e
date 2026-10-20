import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicReference;

public final class TaskScopeRetirementPublicationOracle {
    private static long checks;

    private static void check(boolean condition, String message) {
        checks++;
        if (!condition) throw new AssertionError(message);
    }

    private static final class Owner {
        boolean exitObserved = true;
        boolean releaseComplete;
        boolean published;
        boolean faultOnce = true;
        int externalReleaseCalls;
        int publicationAttempts;

        synchronized void retry() {
            if (published) return;
            if (!exitObserved) {
                throw new IllegalStateException("retirement before physical exit");
            }
            if (!releaseComplete) {
                externalReleaseCalls++;
                releaseComplete = true;
            }
            publicationAttempts++;
            if (faultOnce) {
                faultOnce = false;
                throw new OutOfMemoryError("publication fault");
            }
            published = true;
        }
    }

    private static final class BrokenOwner {
        boolean exitObserved = true;
        boolean releaseComplete;
        boolean published;
        boolean faultOnce = true;
        int externalReleaseCalls;

        synchronized void retry() {
            if (releaseComplete) return;
            if (!exitObserved) {
                throw new IllegalStateException("retirement before physical exit");
            }
            externalReleaseCalls++;
            releaseComplete = true;
            if (faultOnce) {
                faultOnce = false;
                throw new OutOfMemoryError("publication fault");
            }
            published = true;
        }
    }

    private static void retryRound() {
        Owner owner = new Owner();
        try {
            owner.retry();
            throw new AssertionError("expected publication fault");
        } catch (OutOfMemoryError expected) {
            check(owner.releaseComplete, "external release completion was lost");
            check(!owner.published, "publication fault forged retirement witness");
            check(owner.externalReleaseCalls == 1, "external release count mismatch");
            check(owner.publicationAttempts == 1, "publication attempt mismatch");
        }

        owner.retry();
        owner.retry();

        check(owner.published, "publication retry did not complete");
        check(owner.externalReleaseCalls == 1, "successful release replayed");
        check(owner.publicationAttempts == 2, "publication did not retry exactly once");
    }

    private static void raceRound(int workers) throws Exception {
        Owner owner = new Owner();
        try {
            owner.retry();
        } catch (OutOfMemoryError expected) {
            // expected
        }

        CountDownLatch start = new CountDownLatch(1);
        AtomicReference<Throwable> failure = new AtomicReference<>();
        Thread[] threads = new Thread[workers];
        for (int i = 0; i < workers; i++) {
            threads[i] = Thread.ofPlatform().unstarted(() -> {
                try {
                    start.await();
                    owner.retry();
                } catch (Throwable problem) {
                    failure.compareAndSet(null, problem);
                }
            });
            threads[i].start();
        }
        start.countDown();
        for (Thread thread : threads) thread.join();

        check(failure.get() == null, "concurrent retry raised " + failure.get());
        check(owner.published, "concurrent retry did not publish retirement");
        check(owner.externalReleaseCalls == 1, "concurrent retry replayed external release");
        check(owner.publicationAttempts == 2, "concurrent retry duplicated publication attempts");
    }

    private static void negativeControl() {
        BrokenOwner owner = new BrokenOwner();
        try {
            owner.retry();
        } catch (OutOfMemoryError expected) {
            // expected
        }
        owner.retry();
        check(owner.releaseComplete, "negative control did not record release");
        check(!owner.published, "negative control failed to strand retirement fence");
        check(owner.externalReleaseCalls == 1, "negative control release mismatch");
    }

    public static void main(String[] args) throws Exception {
        int retryRounds = 20_000;
        int raceRounds = 5_000;
        int workers = 12;

        for (int i = 0; i < retryRounds; i++) retryRound();
        negativeControl();
        for (int i = 0; i < raceRounds; i++) raceRound(workers);

        System.out.println(
                "PASS source-free TaskScope retirement publication checks=" + checks
                + " retry_rounds=" + retryRounds
                + " race_rounds=" + raceRounds
                + " workers=" + workers
                + " negative_control=release_complete_pending_fence_detected"
                + " private_source_bytes=0");
    }
}
