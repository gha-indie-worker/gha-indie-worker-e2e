public final class SharedMutexPublicationOverflowOracle {
    private static long checks;

    private static void check(boolean condition, String message) {
        checks++;
        if (!condition) throw new AssertionError(message);
    }

    private static final class Model {
        Object owner;
        int pending;
        boolean published;

        synchronized boolean reserve(Object runtime) {
            if (runtime == null) throw new NullPointerException("runtime");
            if (pending < 0 || (owner == null && pending != 0)) {
                throw new IllegalStateException("corrupted");
            }
            if (owner != null && owner != runtime) return false;
            if (pending == Integer.MAX_VALUE) {
                throw new IllegalStateException("capacity");
            }
            if (owner == null) owner = runtime;
            pending++;
            return true;
        }

        synchronized void commit(Object runtime) {
            if (owner != runtime || pending <= 0) throw new IllegalStateException("commit");
            pending--;
            published = true;
        }

        synchronized void abort(Object runtime) {
            if (owner != runtime || pending <= 0) throw new IllegalStateException("abort");
            pending--;
            if (pending == 0 && !published) owner = null;
        }
    }

    private static final class BrokenModel {
        Object owner;
        int pending;

        synchronized boolean reserve(Object runtime) {
            if (owner == null) owner = runtime;
            else if (owner != runtime) return false;
            pending++;
            return true;
        }
    }

    private static void normalRound() {
        Object a = new Object();
        Object b = new Object();
        Model m = new Model();
        check(m.reserve(a), "initial reserve failed");
        check(m.reserve(a), "second reserve failed");
        m.commit(a);
        m.abort(a);
        check(m.pending == 0, "balanced reservations did not drain");
        check(m.published, "commit did not publish runtime affinity");
        check(m.owner == a, "published mutex lost runtime owner");
        check(!m.reserve(b), "foreign runtime reserved published mutex");
    }

    private static void overflowRound() {
        Object a = new Object();
        Object b = new Object();
        Model m = new Model();
        m.owner = a;
        m.pending = Integer.MAX_VALUE;
        try {
            m.reserve(a);
            throw new AssertionError("expected overflow refusal");
        } catch (IllegalStateException expected) {
            check("capacity".equals(expected.getMessage()), "wrong overflow failure");
        }
        check(m.pending == Integer.MAX_VALUE, "overflow changed counter");
        check(m.owner == a, "overflow changed owner");
        check(!m.reserve(b), "foreign overflow reserve should be side-effect free");
        check(m.pending == Integer.MAX_VALUE, "foreign refusal changed counter");
        check(m.owner == a, "foreign refusal changed owner");
    }

    private static void corruptRound() {
        Object a = new Object();
        Model m = new Model();
        m.owner = null;
        m.pending = 1;
        try {
            m.reserve(a);
            throw new AssertionError("expected corrupt-state refusal");
        } catch (IllegalStateException expected) {
            check("corrupted".equals(expected.getMessage()), "wrong corrupt-state failure");
        }
        check(m.pending == 1, "corrupt refusal changed counter");
        check(m.owner == null, "corrupt refusal invented owner");
    }

    private static void negativeControl() {
        Object a = new Object();
        BrokenModel m = new BrokenModel();
        m.owner = a;
        m.pending = Integer.MAX_VALUE;
        check(m.reserve(a), "negative control reserve failed");
        check(m.pending == Integer.MIN_VALUE,
                "negative control failed to reproduce signed counter wrap");
    }

    public static void main(String[] args) {
        int rounds = 50_000;
        for (int i = 0; i < rounds; i++) {
            normalRound();
            overflowRound();
            corruptRound();
        }
        negativeControl();
        System.out.println(
                "PASS source-free SharedMutex publication bounds checks=" + checks
                + " rounds=" + rounds
                + " negative_control=signed_counter_wrap_detected"
                + " private_source_bytes=0");
    }
}
