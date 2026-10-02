package dev.oreslang.runtime;

import java.lang.reflect.Array;
import java.math.BigDecimal;
import java.math.BigInteger;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.Collections;
import java.util.IdentityHashMap;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.Set;
import java.util.UUID;
import java.util.WeakHashMap;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.CancellationException;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.ThreadFactory;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;
import java.util.concurrent.locks.ReentrantLock;
import java.util.function.Function;
import java.util.function.Supplier;
import java.util.function.UnaryOperator;

/**
 * Host-side actor substrate used by the first interpreter.
 *
 * Oreslang follows the core Akka-style execution invariant: actors are
 * multiplexed over dispatcher threads, but one actor processes its mailbox
 * serially. Carrier-thread identity is never actor identity.
 *
 * PRIVATE and SHARED actors are deliberately bulkheaded onto different
 * dispatchers. Private actors also receive a confined logical memory slice;
 * shared actors may coordinate through explicitly synchronized shared cells.
 */
public final class ActorRuntime implements AutoCloseable {
    private static final int MAX_MESSAGE_GRAPH_DEPTH = 256;
    private static final int MAX_MESSAGE_GRAPH_NODES = 100_000;
    private static final long CLOSE_WAIT_NANOS = TimeUnit.MILLISECONDS.toNanos(250);
    private static final ThreadLocal<Boolean> ACTOR_CARRIER = ThreadLocal.withInitial(() -> Boolean.FALSE);
    private static final ThreadLocal<ActorExecutionContext> CURRENT_ACTOR_EXECUTION = new ThreadLocal<>();

    private record ActorExecutionContext(
            ActorRuntime runtime,
            ActorId actorId,
            ActorKind kind,
            IsolatePolicy policy,
            Object executionDomain) { }

    @FunctionalInterface
    public interface TurnExecutor {
        void execute(Runnable turn);

        static TurnExecutor direct() {
            return Runnable::run;
        }
    }

    public static boolean isActorCarrierThread() {
        return Boolean.TRUE.equals(ACTOR_CARRIER.get());
    }

    public static boolean inActorExecution() {
        return CURRENT_ACTOR_EXECUTION.get() != null;
    }

    public static IsolatePolicy currentActorPolicy() {
        ActorExecutionContext current = CURRENT_ACTOR_EXECUTION.get();
        return current == null ? null : current.policy();
    }

    public static ActorRuntime currentActorRuntime() {
        ActorExecutionContext current = CURRENT_ACTOR_EXECUTION.get();
        return current == null ? null : current.runtime();
    }

    public static ActorId currentActorId() {
        ActorExecutionContext current = CURRENT_ACTOR_EXECUTION.get();
        return current == null ? null : current.actorId();
    }

    public static ActorKind currentActorKind() {
        ActorExecutionContext current = CURRENT_ACTOR_EXECUTION.get();
        return current == null ? null : current.kind();
    }

    public static Object currentExecutionDomain() {
        ActorExecutionContext current = CURRENT_ACTOR_EXECUTION.get();
        return current == null ? Thread.currentThread() : current.executionDomain();
    }

    public enum ActorKind { PRIVATE, SHARED }

    public record DispatcherConfig(
            int privateParallelism,
            int sharedParallelism,
            int throughput,
            int maxActors) {
        public DispatcherConfig {
            if (privateParallelism <= 0) throw new IllegalArgumentException("privateParallelism must be > 0");
            if (sharedParallelism <= 0) throw new IllegalArgumentException("sharedParallelism must be > 0");
            if (throughput <= 0) throw new IllegalArgumentException("throughput must be > 0");
            if (maxActors <= 0) throw new IllegalArgumentException("maxActors must be > 0");
        }

        public DispatcherConfig(int privateParallelism, int sharedParallelism, int throughput) {
            this(privateParallelism, sharedParallelism, throughput, 16_384);
        }

        public static DispatcherConfig defaults() {
            int cpus = Math.max(2, Runtime.getRuntime().availableProcessors());
            return new DispatcherConfig(cpus, cpus, 64, 16_384);
        }
    }

    public record ActorId(UUID value) {
        public ActorId { Objects.requireNonNull(value); }
        public static ActorId create() { return new ActorId(UUID.randomUUID()); }
    }

    public record ActorGroupId(UUID value) {
        public ActorGroupId { Objects.requireNonNull(value); }
        public static ActorGroupId create() { return new ActorGroupId(UUID.randomUUID()); }
    }

    public record FanoutReceipt(
            int attempted,
            int delivered,
            int terminated,
            int rejected,
            Map<ActorId, String> failures) {
        public FanoutReceipt {
            if (attempted < 0 || delivered < 0 || terminated < 0 || rejected < 0) {
                throw new IllegalArgumentException("fanout counts cannot be negative");
            }
            if (attempted != delivered + terminated + rejected) {
                throw new IllegalArgumentException("fanout counts must add up to attempted");
            }
            failures = Map.copyOf(failures);
        }

        public boolean allDelivered() { return attempted == delivered; }
    }

    public static final class ActorTerminatedException extends IllegalStateException {
        private final ActorId actorId;
        private final ActorKind actorKind;

        private ActorTerminatedException(ActorId actorId, ActorKind actorKind, Throwable cause) {
            super("actor " + actorId + " (" + actorKind + ") is terminated", cause);
            this.actorId = actorId;
            this.actorKind = actorKind;
        }

        public ActorId actorId() { return actorId; }
        public ActorKind actorKind() { return actorKind; }
    }

    /**
     * Deeply immutable runtime-owned shared value. The backing graph is frozen
     * once, quota-accounted once, and retained until runtime teardown.
     */
    public final class Shared<T> {
        private final AtomicBoolean sharedClosed = new AtomicBoolean();
        private final AtomicLong reservedBytes;
        private final RuntimeGc.Registration gcRegistration;
        private T value;

        private Shared(T value, long reservedBytes) {
            this.value = value;
            AtomicLong accounting = new AtomicLong(reservedBytes);
            this.reservedBytes = accounting;
            this.gcRegistration = garbageCollector.trackProcess(this, () -> {
                long bytes = accounting.getAndSet(0L);
                if (bytes != 0L) ActorRuntime.this.releaseSharedRuntimeBytes(bytes);
                return bytes;
            });
        }

        public T value() {
            rejectPrivateActorSharedMemoryAccess("Shared.value");
            if (sharedClosed.get()) throw new IllegalStateException("Shared value belongs to a closed actor runtime");
            return value;
        }

        private boolean ownedBy(ActorRuntime runtime) {
            return ActorRuntime.this == runtime;
        }

        private void closeFromRuntime() {
            if (!sharedClosed.compareAndSet(false, true)) return;
            value = null;
            gcRegistration.cleanNow();
            sharedValues.remove(this);
        }
    }

    private final Map<ActorId, ActorCell<?>> actors = new ConcurrentHashMap<>();
    private final AtomicInteger actorCount = new AtomicInteger();
    private final AtomicBoolean closed = new AtomicBoolean();
    private final AtomicLong privateMemoryBytes = new AtomicLong();
    private final AtomicLong sharedMemoryBytes = new AtomicLong();
    private final Object memoryBudgetLock = new Object();
    private final Object runtimeLifecycleLock = new Object();
    private final Set<SyncCell<?>> syncCells =
            Collections.synchronizedSet(Collections.newSetFromMap(new WeakHashMap<>()));
    private final Set<Shared<?>> sharedValues =
            Collections.synchronizedSet(Collections.newSetFromMap(new WeakHashMap<>()));
    private final RuntimeGc garbageCollector = new RuntimeGc();
    private final IsolatePolicy policyCeiling;
    private final DispatcherConfig dispatcherConfig;
    private final TurnExecutor turnExecutor;
    private final ExecutorService privateDispatcher;
    private final ExecutorService sharedDispatcher;
    private final ThreadLocal<ActorCell<?>> currentActor = new ThreadLocal<>();
    private final ThreadLocal<SyncCell<?>> currentSyncCell = new ThreadLocal<>();

    public ActorRuntime() {
        this(IsolatePolicy.developer(), DispatcherConfig.defaults(), TurnExecutor.direct());
    }

    public ActorRuntime(IsolatePolicy policyCeiling) {
        this(policyCeiling, DispatcherConfig.defaults(), TurnExecutor.direct());
    }

    public ActorRuntime(IsolatePolicy policyCeiling, int maxActors) {
        this(
                policyCeiling,
                new DispatcherConfig(
                        DispatcherConfig.defaults().privateParallelism(),
                        DispatcherConfig.defaults().sharedParallelism(),
                        DispatcherConfig.defaults().throughput(),
                        maxActors),
                TurnExecutor.direct());
    }

    public ActorRuntime(IsolatePolicy policyCeiling, DispatcherConfig dispatcherConfig) {
        this(policyCeiling, dispatcherConfig, TurnExecutor.direct());
    }

    public ActorRuntime(
            IsolatePolicy policyCeiling,
            DispatcherConfig dispatcherConfig,
            TurnExecutor turnExecutor) {
        this.policyCeiling = Objects.requireNonNull(policyCeiling);
        this.dispatcherConfig = Objects.requireNonNull(dispatcherConfig);
        this.turnExecutor = Objects.requireNonNull(turnExecutor);
        this.privateDispatcher = newDispatcher(
                dispatcherConfig.privateParallelism(),
                dispatcherConfig.maxActors(),
                "ores-private-actor-dispatcher-");
        this.sharedDispatcher = newDispatcher(
                dispatcherConfig.sharedParallelism(),
                dispatcherConfig.maxActors(),
                "ores-shared-actor-dispatcher-");
    }

    public IsolatePolicy policyCeiling() { return policyCeiling; }
    public DispatcherConfig dispatcherConfig() { return dispatcherConfig; }
    public int maxActors() { return dispatcherConfig.maxActors(); }
    public int actorCount() { return actorCount.get(); }
    public long privateMemoryBytes() { return privateMemoryBytes.get(); }
    public long sharedMemoryBytes() { return sharedMemoryBytes.get(); }
    public long actorMemoryBytes() { return privateMemoryBytes.get() + sharedMemoryBytes.get(); }
    public int trackedGcResources() { return garbageCollector.trackedResources(); }

    public RuntimeGc.GcStats collectCurrentActorGarbage() {
        requireCallerRuntimeAffinity("collect actor garbage");
        ActorExecutionContext current = CURRENT_ACTOR_EXECUTION.get();
        if (current == null || current.runtime() != this) {
            throw new IllegalStateException("actor.gc() is valid only during the current actor mailbox turn");
        }
        return garbageCollector.collectActor(current.actorId());
    }

    public RuntimeGc.GcStats collectProcessGarbage(boolean requestHostGc) {
        requireCallerRuntimeAffinity("collect process garbage");
        if (closed.get()) throw new IllegalStateException("actor runtime is closed");
        return garbageCollector.collectProcess(requestHostGc);
    }

    /**
     * Logical actor-confined memory slice for one private actor.
     *
     * This is independent of carrier threads. Mailbox payloads and persistent
     * actor-state allocations share one budget. The current JVM backend uses
     * accounting plus alias isolation; a native/polyglot-isolate backend can map
     * this same contract to a physically separate heap/arena.
     */
    public final class ActorMemorySlice implements AutoCloseable {
        private final ActorId owner;
        private final long limitBytes;
        private final AtomicLong usedBytes = new AtomicLong();
        private final AtomicBoolean sliceClosed = new AtomicBoolean();
        private final Set<PrivateMemoryBlock> blocks =
                Collections.synchronizedSet(Collections.newSetFromMap(new WeakHashMap<>()));

        private ActorMemorySlice(ActorId owner, long limitBytes) {
            this.owner = Objects.requireNonNull(owner);
            this.limitBytes = limitBytes;
        }

        public ActorId owner() { return owner; }
        public long limitBytes() { return limitBytes; }
        public long usedBytes() { return usedBytes.get(); }
        public long remainingBytes() { return Math.max(0L, limitBytes - usedBytes.get()); }
        public boolean closed() { return sliceClosed.get(); }

        /**
         * Reserve persistent private-actor heap. Compiler/interpreter lowering
         * should retain the reservation for as long as the state allocation is
         * live and close it when that allocation dies.
         */
        public MemoryReservation reserveHeap(long bytes) {
            requireCurrentOwner();
            return reserve(bytes, "private actor heap");
        }

        /**
         * Allocate actor-confined direct memory. The raw ByteBuffer is never
         * exposed; all reads/writes verify the owning ActorId. This gives
         * compiler-lowered private actor state a genuinely unshared backing
         * region while actors remain multiplexed over carrier threads.
         */
        public PrivateMemoryBlock allocatePrivateBytes(int bytes) {
            requireCurrentOwner();
            if (bytes < 0) throw new IllegalArgumentException("private memory block size cannot be negative");
            MemoryReservation reservation = reserve(bytes, "private actor direct heap");
            try {
                PrivateMemoryBlock block = new PrivateMemoryBlock(this, reservation, bytes);
                blocks.add(block);
                return block;
            } catch (RuntimeException | Error failure) {
                reservation.close();
                throw failure;
            }
        }

        private MemoryReservation reserveMailbox(Object isolatedMessage) {
            return reserve(estimateFrozenBytes(isolatedMessage), "private actor mailbox");
        }

        private synchronized MemoryReservation reserve(long bytes, String purpose) {
            if (bytes < 0) throw new IllegalArgumentException("memory reservation cannot be negative");
            if (sliceClosed.get()) throw new IllegalStateException("private actor memory slice is closed");
            if (bytes == 0) return new MemoryReservation(this, 0);

            long current = usedBytes.get();
            long next;
            try {
                next = Math.addExact(current, bytes);
            } catch (ArithmeticException overflow) {
                throw new IllegalStateException(purpose + " accounting overflow");
            }
            if (next > limitBytes) {
                throw new IllegalStateException(purpose + " limit exceeded for " + owner
                        + ": requested=" + bytes + " used=" + current + " limit=" + limitBytes);
            }

            reservePrivateRuntimeBytes(bytes, owner, purpose);
            usedBytes.set(next);
            return new MemoryReservation(this, bytes);
        }

        private void requireCurrentOwner() {
            ActorCell<?> cell = currentActor.get();
            if (cell == null || cell.kind != ActorKind.PRIVATE || !cell.ref.id().equals(owner)) {
                throw new IllegalStateException(
                        "private actor memory slice may only be reserved by its owning actor");
            }
        }

        private synchronized long release(long bytes) {
            if (bytes == 0 || sliceClosed.get()) return 0L;
            long remaining = usedBytes.addAndGet(-bytes);
            if (remaining < 0) {
                usedBytes.addAndGet(bytes);
                throw new IllegalStateException("private actor memory accounting underflow for " + owner);
            }
            try {
                releasePrivateRuntimeBytes(bytes, owner);
                return bytes;
            } catch (RuntimeException failure) {
                usedBytes.addAndGet(bytes);
                throw failure;
            }
        }

        @Override
        public synchronized void close() {
            if (!sliceClosed.compareAndSet(false, true)) return;
            List<PrivateMemoryBlock> snapshot;
            synchronized (blocks) {
                snapshot = List.copyOf(blocks);
            }
            for (PrivateMemoryBlock block : snapshot) block.invalidateFromSlice();
            blocks.clear();
            long bytes = usedBytes.getAndSet(0);
            if (bytes != 0) releasePrivateRuntimeBytes(bytes, owner);
        }

        private void unregister(PrivateMemoryBlock block) {
            blocks.remove(block);
        }
    }

    /**
     * Owner-checked direct memory owned by exactly one private actor.
     *
     * No mutable buffer reference escapes this wrapper. Closing the block or
     * terminating the actor overwrites the entire region before invalidation.
     */
    public final class PrivateMemoryBlock implements AutoCloseable {
        private final ActorMemorySlice slice;
        private final MemoryReservation reservation;
        private final int capacity;
        private final AtomicReference<ByteBuffer> memory;
        private final AtomicBoolean blockClosed = new AtomicBoolean();
        private final RuntimeGc.Registration gcRegistration;

        private PrivateMemoryBlock(
                ActorMemorySlice slice,
                MemoryReservation reservation,
                int bytes) {
            this.slice = Objects.requireNonNull(slice);
            this.reservation = Objects.requireNonNull(reservation);
            this.capacity = bytes;
            AtomicReference<ByteBuffer> memoryState = new AtomicReference<>(
                    ByteBuffer.allocateDirect(bytes).order(ByteOrder.LITTLE_ENDIAN));
            this.memory = memoryState;
            this.gcRegistration = garbageCollector.trackActor(this, slice.owner(), () -> {
                ByteBuffer abandoned = memoryState.getAndSet(null);
                zeroBuffer(abandoned);
                return reservation.releaseNow();
            });
        }

        public int capacity() { return capacity; }
        public ActorId owner() { return slice.owner(); }
        public boolean closed() { return blockClosed.get(); }

        public byte readByte(int index) {
            return openMemory().get(index);
        }

        public void writeByte(int index, byte value) {
            openMemory().put(index, value);
        }

        public int readInt(int index) {
            return openMemory().getInt(index);
        }

        public void writeInt(int index, int value) {
            openMemory().putInt(index, value);
        }

        public long readLong(int index) {
            return openMemory().getLong(index);
        }

        public void writeLong(int index, long value) {
            openMemory().putLong(index, value);
        }

        public double readDouble(int index) {
            return openMemory().getDouble(index);
        }

        public void writeDouble(int index, double value) {
            openMemory().putDouble(index, value);
        }

        public byte[] copyOut() {
            ByteBuffer live = openMemory();
            byte[] out = new byte[capacity];
            ByteBuffer duplicate = live.duplicate();
            duplicate.clear();
            duplicate.get(out);
            return out;
        }

        public void copyIn(byte[] bytes) {
            Objects.requireNonNull(bytes);
            ByteBuffer live = openMemory();
            if (bytes.length != capacity) {
                throw new IllegalArgumentException(
                        "private memory copy size mismatch: expected " + capacity
                                + " bytes but got " + bytes.length);
            }
            ByteBuffer duplicate = live.duplicate();
            duplicate.clear();
            duplicate.put(bytes);
        }

        private ByteBuffer openMemory() {
            if (blockClosed.get() || slice.closed()) {
                throw new IllegalStateException("private actor memory block is closed");
            }
            slice.requireCurrentOwner();
            ByteBuffer live = memory.get();
            if (live == null) throw new IllegalStateException("private actor memory block is closed");
            return live;
        }

        private void invalidateFromSlice() {
            if (!blockClosed.compareAndSet(false, true)) return;
            gcRegistration.cleanNow();
        }

        @Override
        public void close() {
            openMemory(); // owner + liveness check before invalidation
            if (!blockClosed.compareAndSet(false, true)) return;
            gcRegistration.cleanNow();
            slice.unregister(this);
        }
    }

    public final class MemoryReservation implements AutoCloseable {
        private final ActorMemorySlice slice;
        private final long bytes;
        private final AtomicBoolean released;
        private final RuntimeGc.Registration gcRegistration;

        private MemoryReservation(ActorMemorySlice slice, long bytes) {
            this.slice = Objects.requireNonNull(slice);
            this.bytes = bytes;
            AtomicBoolean releaseState = new AtomicBoolean();
            this.released = releaseState;
            this.gcRegistration = garbageCollector.trackActor(this, slice.owner(), () -> {
                if (!releaseState.compareAndSet(false, true)) return 0L;
                return slice.release(bytes);
            });
        }

        public ActorId owner() { return slice.owner(); }
        public long bytes() { return bytes; }

        private long releaseNow() {
            return gcRegistration.cleanNow();
        }

        @Override
        public void close() {
            releaseNow();
        }
    }

    private record MessageEnvelope(Object value, Runnable release) implements AutoCloseable {
        @Override
        public void close() {
            if (release != null) release.run();
        }
    }

    /**
     * Explicit synchronized shared-memory cell.
     *
     * Actor fields do not use this: a mailbox turn already provides exclusive
     * mutation of actor-owned state. SyncCell is for state intentionally shared
     * by multiple SHARED actors.
     */
    public final class SyncCell<T> implements AutoCloseable {
        private final ReentrantLock lock = new ReentrantLock(true);
        private final AtomicBoolean cellClosed = new AtomicBoolean();
        private final AtomicLong reservedBytes;
        private final RuntimeGc.Registration gcRegistration;
        private T value;

        @SuppressWarnings("unchecked")
        private SyncCell(T initialValue) {
            Object frozen = freeze(initialValue);
            rejectSharedMutableHandles(frozen, new IdentityHashMap<>(), 0);
            long bytes = estimateFrozenBytes(frozen);
            reserveSharedRuntimeBytes(bytes, "shared SyncCell");
            AtomicLong accounting = new AtomicLong(bytes);
            this.value = (T) frozen;
            this.reservedBytes = accounting;
            this.gcRegistration = garbageCollector.trackProcess(this, () -> {
                long released = accounting.getAndSet(0L);
                if (released != 0L) ActorRuntime.this.releaseSharedRuntimeBytes(released);
                return released;
            });
        }

        public boolean closed() { return cellClosed.get(); }

        private boolean ownedBy(ActorRuntime runtime) {
            return ActorRuntime.this == runtime;
        }

        public T snapshot() {
            rejectPrivateActorSharedMemoryAccess("SyncCell.snapshot");
            boolean entered = enterSyncCell(this);
            lock.lock();
            try {
                requireOpen();
                return value;
            } finally {
                lock.unlock();
                exitSyncCell(entered);
            }
        }

        public <R> R read(Function<? super T, ? extends R> reader) {
            Objects.requireNonNull(reader);
            requireSharedActorTurn();
            boolean entered = enterSyncCell(this);
            lock.lock();
            try {
                requireOpen();
                Object frozen = freeze(reader.apply(value));
                rejectSharedMutableHandles(frozen, new IdentityHashMap<>(), 0);
                @SuppressWarnings("unchecked")
                R result = (R) frozen;
                return result;
            } finally {
                lock.unlock();
                exitSyncCell(entered);
            }
        }

        @SuppressWarnings("unchecked")
        public T update(UnaryOperator<T> updater) {
            Objects.requireNonNull(updater);
            requireSharedActorTurn();
            boolean entered = enterSyncCell(this);
            lock.lock();
            try {
                requireOpen();
                Object frozen = freeze(updater.apply(value));
                rejectSharedMutableHandles(frozen, new IdentityHashMap<>(), 0);
                requireOpen();
                if (closed.get()) throw new IllegalStateException("actor runtime is closed");
                long nextBytes = estimateFrozenBytes(frozen);
                long previousBytes = reservedBytes.get();
                long delta = nextBytes - previousBytes;
                if (delta > 0) reserveSharedRuntimeBytes(delta, "shared SyncCell update");
                value = (T) frozen;
                if (delta < 0) releaseSharedRuntimeBytes(-delta);
                reservedBytes.set(nextBytes);
                return value;
            } finally {
                lock.unlock();
                exitSyncCell(entered);
            }
        }

        private void requireOpen() {
            if (cellClosed.get()) throw new IllegalStateException("SyncCell is closed");
        }

        @Override
        public void close() {
            rejectPrivateActorSharedMemoryAccess("SyncCell.close");
            boolean entered = enterSyncCell(this);
            try {
                closeFromRuntime();
            } finally {
                exitSyncCell(entered);
            }
        }

        private void closeFromRuntime() {
            lock.lock();
            try {
                if (!cellClosed.compareAndSet(false, true)) return;
                value = null;
                gcRegistration.cleanNow();
                syncCells.remove(this);
            } finally {
                lock.unlock();
            }
        }

        private void invalidateFromRuntime() {
            if (cellClosed.compareAndSet(false, true)) {
                gcRegistration.cleanNow();
            }
            syncCells.remove(this);
        }
    }

    @FunctionalInterface
    public interface Behavior<M> {
        void onMessage(M message, ActorContext<M> context) throws Exception;
    }

    /**
     * Compiler-facing actor constructor. The actor context is available before
     * state initialization, so private actor fields can reserve/allocate in the
     * actor's confined memory slice rather than being captured from the caller.
     */
    @FunctionalInterface
    public interface BehaviorFactory<M> {
        Behavior<M> create(ActorContext<M> context) throws Exception;
    }

    public interface ActorContext<M> {
        ActorRef<M> self();
        ActorRuntime runtime();
        IsolatePolicy policy();
        ActorKind kind();
        Optional<ActorMemorySlice> privateMemory();

        /** Immediate non-blocking pull from this actor's own mailbox. */
        Optional<M> tryReceive();

        /**
         * Claims exactly one future mailbox item without parking the actor
         * dispatcher carrier. Source-level blocking receive lowers to this
         * continuation primitive.
         */
        CompletionStage<M> receiveAsync();
    }

    /**
     * Explicit typed fan-out handle. Membership is runtime-local, deduplicated
     * by ActorId, and broadcast uses a point-in-time membership snapshot.
     */
    public final class ActorGroup<M> implements AutoCloseable {
        private static final int MAX_CATEGORY_LENGTH = 128;

        private final ActorGroupId id = ActorGroupId.create();
        private final String category;
        private final ConcurrentHashMap<ActorId, ActorRef<M>> members = new ConcurrentHashMap<>();
        private final AtomicBoolean groupClosed = new AtomicBoolean();
        private final Object groupLifecycleLock = new Object();

        private ActorGroup(String category) {
            String normalized = Objects.requireNonNull(category, "group category").trim();
            if (normalized.isEmpty()) throw new IllegalArgumentException("actor group category cannot be blank");
            if (normalized.length() > MAX_CATEGORY_LENGTH) {
                throw new IllegalArgumentException("actor group category exceeds " + MAX_CATEGORY_LENGTH + " characters");
            }
            this.category = normalized;
        }

        public ActorGroupId id() { return id; }
        public String category() { return category; }
        public int size() {
            requireCallerRuntimeAffinity("inspect actor groups");
            synchronized (groupLifecycleLock) {
                requireOpenLocked();
                return members.size();
            }
        }
        public boolean closed() { return groupClosed.get(); }
        private boolean ownedBy(ActorRuntime runtime) { return ActorRuntime.this == runtime; }

        public ActorGroup<M> add(ActorRef<M> ref) {
            requireCallerRuntimeAffinity("modify actor groups");
            Objects.requireNonNull(ref);
            synchronized (groupLifecycleLock) {
                requireOpenLocked();
                if (!ref.ownedBy(ActorRuntime.this)) {
                    throw new IllegalArgumentException(
                            "ActorRef belongs to a different ActorRuntime; cross-runtime groups require an explicit bridge");
                }
                if (!ref.isAlive()) throw terminated(ref);
                members.put(ref.id(), ref);
            }
            return this;
        }

        public ActorGroup<M> remove(ActorRef<M> ref) {
            requireCallerRuntimeAffinity("modify actor groups");
            Objects.requireNonNull(ref);
            synchronized (groupLifecycleLock) {
                requireOpenLocked();
                if (!ref.ownedBy(ActorRuntime.this)) {
                    throw new IllegalArgumentException(
                            "ActorRef belongs to a different ActorRuntime; cross-runtime groups require an explicit bridge");
                }
                members.remove(ref.id(), ref);
            }
            return this;
        }

        public List<ActorRef<M>> members() {
            requireCallerRuntimeAffinity("inspect actor groups");
            synchronized (groupLifecycleLock) {
                requireOpenLocked();
                return snapshotMembersLocked();
            }
        }

        private List<ActorRef<M>> snapshotMembersLocked() {
            ArrayList<ActorRef<M>> snapshot = new ArrayList<>(members.values());
            snapshot.sort(java.util.Comparator.comparing(ref -> ref.id().value().toString()));
            return List.copyOf(snapshot);
        }

        public FanoutReceipt broadcast(M message) {
            requireCallerRuntimeAffinity("broadcast actor groups");
            final List<ActorRef<M>> snapshot;
            synchronized (groupLifecycleLock) {
                requireOpenLocked();
                snapshot = snapshotMembersLocked();
            }

            validateMessageGraph(message);
            requireOwnedActorRefs(message, new IdentityHashMap<>(), 0);
            int delivered = 0;
            int terminated = 0;
            int rejected = 0;
            LinkedHashMap<ActorId, String> failures = new LinkedHashMap<>();

            for (ActorRef<M> ref : snapshot) {
                try {
                    ActorRuntime.this.send(ref, message);
                    delivered++;
                } catch (ActorTerminatedException dead) {
                    terminated++;
                    members.remove(ref.id(), ref);
                    failures.put(ref.id(), dead.getMessage());
                } catch (IllegalStateException | IllegalArgumentException | SecurityException failure) {
                    rejected++;
                    failures.put(ref.id(), failure.getMessage() == null
                            ? failure.getClass().getSimpleName()
                            : failure.getMessage());
                }
            }
            return new FanoutReceipt(snapshot.size(), delivered, terminated, rejected, failures);
        }

        private void requireOpen() {
            requireCallerRuntimeAffinity("use actor groups");
            synchronized (groupLifecycleLock) {
                requireOpenLocked();
            }
        }

        private void requireOpenLocked() {
            if (groupClosed.get()) throw new IllegalStateException("actor group is closed");
            if (ActorRuntime.this.closed.get()) throw new IllegalStateException("actor runtime is closed");
        }

        @Override
        public void close() {
            requireCallerRuntimeAffinity("close actor groups");
            synchronized (groupLifecycleLock) {
                if (!groupClosed.compareAndSet(false, true)) return;
                members.clear();
            }
        }
    }

    public final class ActorRef<M> {
        private final ActorId id;
        private final ActorKind kind;
        private final AtomicReference<Throwable> terminationCause = new AtomicReference<>();

        private ActorRef(ActorId id, ActorKind kind) {
            this.id = id;
            this.kind = kind;
        }

        public ActorId id() { return id; }
        public ActorKind kind() { return kind; }
        private boolean ownedBy(ActorRuntime runtime) { return ActorRuntime.this == runtime; }
        public boolean isAlive() { return ActorRuntime.this.isAlive(this); }
        public Optional<Throwable> failure() { return Optional.ofNullable(terminationCause.get()); }

        public boolean awaitTermination(long timeout, TimeUnit unit) throws InterruptedException {
            Objects.requireNonNull(unit);
            if (timeout < 0) throw new IllegalArgumentException("timeout must be non-negative");
            ActorCell<?> cell = actors.get(id);
            if (cell == null) return true;
            cell.awaitFinalized(unit.toNanos(timeout));
            return cell.finalized();
        }

        public void send(M message) {
            ActorRuntime.this.send(this, message);
        }

        public void stop() {
            ActorRuntime.this.stop(this);
        }

        @Override
        public String toString() {
            return "ActorRef[" + kind + ":" + id.value() + "]";
        }
    }

    private void requireCallerRuntimeAffinity(String operation) {
        ActorRuntime caller = currentActorRuntime();
        if (caller != null && caller != this) {
            throw new SecurityException(
                    "actor cannot " + operation + " through another ActorRuntime");
        }
    }

    public <M> ActorGroup<M> group(String category) {
        requireCallerRuntimeAffinity("create actor groups");
        if (closed.get()) throw new IllegalStateException("actor runtime is closed");
        return new ActorGroup<>(category);
    }

    private static void requireSupervisorContext(String operation) {
        if (inActorExecution()) {
            throw new SecurityException(
                    "actor code cannot " + operation + "; this operation belongs to the host/supervisor");
        }
    }

    private IsolatePolicy defaultSpawnPolicy() {
        IsolatePolicy caller = currentActorPolicy();
        return caller == null ? policyCeiling : caller;
    }

    private void requireWithinCallerPolicy(IsolatePolicy child) {
        IsolatePolicy caller = currentActorPolicy();
        if (caller == null) return;

        if (!caller.capabilities().containsAll(child.capabilities())) {
            java.util.Set<IsolatePolicy.Capability> excess = child.capabilities().isEmpty()
                    ? java.util.EnumSet.noneOf(IsolatePolicy.Capability.class)
                    : java.util.EnumSet.copyOf(child.capabilities());
            excess.removeAll(caller.capabilities());
            throw new SecurityException("child actor policy exceeds caller actor capabilities: " + excess);
        }
        if (child.maxHeapBytes() > caller.maxHeapBytes()) {
            throw new SecurityException("child actor maxHeapBytes exceeds caller actor policy");
        }
        if (child.maxMailboxMessages() > caller.maxMailboxMessages()) {
            throw new SecurityException("child actor mailbox limit exceeds caller actor policy");
        }
        if (child.maxWallTime().compareTo(caller.maxWallTime()) > 0) {
            throw new SecurityException("child actor wall-time limit exceeds caller actor policy");
        }
        if (caller.adversarial() && !child.adversarial()) {
            throw new SecurityException("child actor cannot weaken an adversarial caller policy");
        }
    }

    /** Backward-compatible default: an unqualified runtime actor is private. */
    public <M> ActorRef<M> spawn(Supplier<? extends Behavior<M>> behaviorFactory) {
        return spawnPrivate(defaultSpawnPolicy(), behaviorFactory);
    }

    public <M> ActorRef<M> spawn(
            IsolatePolicy policy,
            Supplier<? extends Behavior<M>> behaviorFactory) {
        return spawnPrivate(policy, behaviorFactory);
    }

    public <M> ActorRef<M> spawnPrivate(Supplier<? extends Behavior<M>> behaviorFactory) {
        return spawnPrivate(policyCeiling, behaviorFactory);
    }

    /**
     * Trusted-host compatibility path. Compiler-generated actors should prefer
     * the context-aware BehaviorFactory overload.
     */
    public <M> ActorRef<M> spawnPrivate(
            IsolatePolicy policy,
            Supplier<? extends Behavior<M>> behaviorFactory) {
        requireSupervisorContext("use trusted Supplier private actor construction");
        Objects.requireNonNull(behaviorFactory);
        requireTrustedSupplierPolicy(policy);
        return spawnInternal(ActorKind.PRIVATE, policy, context -> behaviorFactory.get(), true);
    }

    /**
     * Isolation-safe private actor construction. The factory itself must be
     * stateless/capture-free; mutable actor state must be created after the
     * actor context is installed and stored in actor-owned memory.
     */
    public <M> ActorRef<M> spawnPrivate(BehaviorFactory<M> behaviorFactory) {
        return spawn(ActorKind.PRIVATE, defaultSpawnPolicy(), behaviorFactory);
    }

    public <M> ActorRef<M> spawnPrivate(
            IsolatePolicy policy,
            BehaviorFactory<M> behaviorFactory) {
        return spawn(ActorKind.PRIVATE, policy, behaviorFactory);
    }

    /**
     * Explicit host-only escape hatch for tests/embedding code that needs a
     * context-aware factory with captured Java objects. Never used by Oreslang
     * compiler lowering and forbidden for adversarial policies.
     */
    public <M> ActorRef<M> spawnPrivateTrusted(BehaviorFactory<M> behaviorFactory) {
        return spawnPrivateTrusted(defaultSpawnPolicy(), behaviorFactory);
    }

    public <M> ActorRef<M> spawnPrivateTrusted(
            IsolatePolicy policy,
            BehaviorFactory<M> behaviorFactory) {
        requireSupervisorContext("use trusted captured private actor construction");
        Objects.requireNonNull(behaviorFactory);
        requireTrustedSupplierPolicy(policy);
        return spawnInternal(ActorKind.PRIVATE, policy, behaviorFactory, true);
    }

    public <M> ActorRef<M> spawnShared(Supplier<? extends Behavior<M>> behaviorFactory) {
        return spawnShared(defaultSpawnPolicy(), behaviorFactory);
    }

    public <M> ActorRef<M> spawnShared(
            IsolatePolicy policy,
            Supplier<? extends Behavior<M>> behaviorFactory) {
        requireSupervisorContext("use trusted Supplier shared actor construction");
        Objects.requireNonNull(behaviorFactory);
        requireTrustedSupplierPolicy(policy);
        return spawnInternal(ActorKind.SHARED, policy, context -> behaviorFactory.get(), true);
    }

    public <M> ActorRef<M> spawnShared(BehaviorFactory<M> behaviorFactory) {
        return spawn(ActorKind.SHARED, defaultSpawnPolicy(), behaviorFactory);
    }

    public <M> ActorRef<M> spawnShared(
            IsolatePolicy policy,
            BehaviorFactory<M> behaviorFactory) {
        return spawn(ActorKind.SHARED, policy, behaviorFactory);
    }

    /**
     * Explicit host-only escape hatch for context-aware shared actor factories
     * that intentionally capture host objects. Compiler lowering must never use
     * this path. Adversarial policies reject it.
     */
    public <M> ActorRef<M> spawnSharedTrusted(BehaviorFactory<M> behaviorFactory) {
        return spawnSharedTrusted(defaultSpawnPolicy(), behaviorFactory);
    }

    public <M> ActorRef<M> spawnSharedTrusted(
            IsolatePolicy policy,
            BehaviorFactory<M> behaviorFactory) {
        requireSupervisorContext("use trusted captured shared actor construction");
        Objects.requireNonNull(behaviorFactory);
        requireTrustedSupplierPolicy(policy);
        return spawnInternal(ActorKind.SHARED, policy, behaviorFactory, true);
    }

    /** Compatibility path for trusted host callers. */
    public <M> ActorRef<M> spawn(
            ActorKind kind,
            IsolatePolicy policy,
            Supplier<? extends Behavior<M>> behaviorFactory) {
        requireSupervisorContext("use trusted Supplier actor construction");
        Objects.requireNonNull(behaviorFactory);
        requireTrustedSupplierPolicy(policy);
        return spawnInternal(kind, policy, context -> behaviorFactory.get(), true);
    }

    private static void requireStatelessActorFactory(Object factory) {
        for (Class<?> type = factory.getClass();
             type != null && type != Object.class;
             type = type.getSuperclass()) {
            for (java.lang.reflect.Field field : type.getDeclaredFields()) {
                int modifiers = field.getModifiers();
                boolean isStatic = java.lang.reflect.Modifier.isStatic(modifiers);
                boolean isFinal = java.lang.reflect.Modifier.isFinal(modifiers);

                if (!isStatic) {
                    throw new SecurityException(
                            "actor BehaviorFactory must be stateless; captured host state must enter through explicit actor messages/capabilities");
                }
                if (!isFinal) {
                    throw new SecurityException(
                            "actor BehaviorFactory declares mutable static JVM state '"
                                    + field.getName() + "'; actor construction cannot share static state");
                }
                if (!field.trySetAccessible()) {
                    throw new SecurityException(
                            "actor BehaviorFactory contains inaccessible static state: " + field.getName());
                }
                final Object value;
                try {
                    value = field.get(null);
                } catch (IllegalAccessException impossible) {
                    throw new SecurityException(
                            "cannot inspect actor BehaviorFactory static state: " + field.getName(),
                            impossible);
                }
                if (!isPrivateStaticConstant(value)) {
                    throw new SecurityException(
                            "actor BehaviorFactory declares shared static object '"
                                    + field.getName()
                                    + "'; only immutable scalar constants are allowed");
                }
            }
        }
    }

    private void validatePrivateBehaviorState(ActorId owner, Behavior<?> behavior) {
        for (Class<?> type = behavior.getClass();
             type != null && type != Object.class;
             type = type.getSuperclass()) {
            for (java.lang.reflect.Field field : type.getDeclaredFields()) {
                int modifiers = field.getModifiers();
                boolean isStatic = java.lang.reflect.Modifier.isStatic(modifiers);
                boolean isFinal = java.lang.reflect.Modifier.isFinal(modifiers);

                if (isStatic) {
                    if (!isFinal) {
                        throw new SecurityException(
                                "private actor behavior class declares mutable static JVM state '"
                                        + field.getName() + "'; private actors cannot share static state");
                    }
                    if (!field.trySetAccessible()) {
                        throw new SecurityException(
                                "private actor behavior contains inaccessible static state: " + field.getName());
                    }
                    final Object staticValue;
                    try {
                        staticValue = field.get(null);
                    } catch (IllegalAccessException impossible) {
                        throw new SecurityException(
                                "cannot inspect private actor static state: " + field.getName(),
                                impossible);
                    }
                    if (!isPrivateStaticConstant(staticValue)) {
                        throw new SecurityException(
                                "private actor behavior class declares shared static object '"
                                        + field.getName()
                                        + "'; only immutable scalar constants are allowed");
                    }
                    continue;
                }

                if (!isFinal) {
                    throw new SecurityException(
                            "private actor behavior field '" + field.getName()
                                    + "' is mutable JVM state; persistent mutable state must use context.privateMemory()");
                }
                if (!field.trySetAccessible()) {
                    throw new SecurityException(
                            "private actor behavior contains inaccessible captured state: " + field.getName());
                }
                final Object value;
                try {
                    value = field.get(behavior);
                } catch (IllegalAccessException impossible) {
                    throw new SecurityException(
                            "cannot inspect private actor behavior capture: " + field.getName(),
                            impossible);
                }
                validatePrivateBehaviorCapture(owner, field.getName(), value);
            }
        }
    }

    private static boolean isPrivateStaticConstant(Object value) {
        return value == null
                || isScalar(value)
                || value instanceof Class<?>;
    }

    private void validatePrivateBehaviorCapture(ActorId owner, String fieldName, Object value) {
        if (value == null || isScalar(value) || value instanceof Class<?>) return;

        if (value instanceof PrivateMemoryBlock block) {
            if (!block.owner().equals(owner)) {
                throw new SecurityException(
                        "private actor behavior captured another actor's memory block in " + fieldName);
            }
            return;
        }
        if (value instanceof ActorMemorySlice slice) {
            if (!slice.owner().equals(owner)) {
                throw new SecurityException(
                        "private actor behavior captured another actor's memory slice in " + fieldName);
            }
            return;
        }
        if (value instanceof MemoryReservation reservation) {
            if (!reservation.owner().equals(owner)) {
                throw new SecurityException(
                        "private actor behavior captured another actor's memory reservation in " + fieldName);
            }
            return;
        }
        if (value instanceof ActorRef<?> ref) {
            if (!ref.ownedBy(this)) {
                throw new SecurityException(
                        "private actor behavior captured an ActorRef from another runtime in " + fieldName);
            }
            return;
        }
        if (value instanceof ActorGroup<?> group) {
            if (!group.ownedBy(this)) {
                throw new SecurityException(
                        "private actor behavior captured an ActorGroup from another runtime in " + fieldName);
            }
            return;
        }
        if (value instanceof ActorContext<?> actorContext) {
            if (!actorContext.self().id().equals(owner) || actorContext.runtime() != this) {
                throw new SecurityException(
                        "private actor behavior captured a foreign actor context in " + fieldName);
            }
            return;
        }

        throw new SecurityException(
                "private actor behavior captured mutable/non-private JVM state in "
                        + fieldName + " (" + value.getClass().getName()
                        + "); allocate persistent state through context.privateMemory()");
    }

    private void requireTrustedSupplierPolicy(IsolatePolicy policy) {
        Objects.requireNonNull(policy);
        if (policy.adversarial()) {
            throw new SecurityException(
                    "adversarial actors require the context-aware BehaviorFactory path; "
                            + "Supplier factories can capture host/shared mutable references");
        }
    }

    /**
     * Creates the actor identity and private memory slice immediately. Behavior
     * initialization later runs on that actor's dispatcher with the actor
     * context already installed.
     */
    public <M> ActorRef<M> spawn(
            ActorKind kind,
            IsolatePolicy policy,
            BehaviorFactory<M> behaviorFactory) {
        return spawnInternal(kind, policy, behaviorFactory, false);
    }

    private <M> ActorRef<M> spawnInternal(
            ActorKind kind,
            IsolatePolicy policy,
            BehaviorFactory<M> behaviorFactory,
            boolean trustedFactory) {
        requireCallerRuntimeAffinity("spawn actors");
        Objects.requireNonNull(kind);
        Objects.requireNonNull(policy);
        Objects.requireNonNull(behaviorFactory);
        requireWithinCeiling(policy);
        IsolatePolicy effectivePolicy = kind == ActorKind.PRIVATE
                ? policy.withoutCapabilities(
                        IsolatePolicy.Capability.SHARED_MEMORY,
                        IsolatePolicy.Capability.ACTOR_SHARE_READONLY)
                : policy;
        requireWithinCallerPolicy(effectivePolicy);
        if (kind == ActorKind.SHARED) {
            effectivePolicy.require(IsolatePolicy.Capability.SHARED_MEMORY, "shared actor spawn");
        }
        if (!trustedFactory) {
            requireStatelessActorFactory(behaviorFactory);
        }

        synchronized (runtimeLifecycleLock) {
            if (closed.get()) throw new IllegalStateException("actor runtime is closed");
            reserveActorSlot();

            ActorId id = ActorId.create();
            ActorRef<M> ref = new ActorRef<>(id, kind);
            try {
                ActorCell<M> cell = new ActorCell<>(
                    ref, kind, effectivePolicy, behaviorFactory, trustedFactory);
                actors.put(id, cell);
                return ref;
            } catch (RuntimeException | Error failure) {
                actorCount.decrementAndGet();
                throw failure;
            }
        }
    }

    private void reserveActorSlot() {
        while (true) {
            int current = actorCount.get();
            if (current >= dispatcherConfig.maxActors()) {
                throw new IllegalStateException(
                        "actor runtime limit exceeded: maximum " + dispatcherConfig.maxActors());
            }
            if (actorCount.compareAndSet(current, current + 1)) return;
        }
    }

    private void unregisterActor(ActorCell<?> cell) {
        if (actors.remove(cell.ref.id(), cell)) {
            int remaining = actorCount.decrementAndGet();
            if (remaining < 0) {
                actorCount.incrementAndGet();
                throw new IllegalStateException("actor count accounting underflow");
            }
        }
    }

    public <T> SyncCell<T> syncCell(T initialValue) {
        requireCallerRuntimeAffinity("create shared SyncCell values");
        if (closed.get()) throw new IllegalStateException("actor runtime is closed");
        IsolatePolicy callerPolicy = currentActorPolicy();
        if (callerPolicy != null) {
            callerPolicy.require(IsolatePolicy.Capability.SHARED_MEMORY, "SyncCell");
        } else {
            policyCeiling.require(IsolatePolicy.Capability.SHARED_MEMORY, "SyncCell");
        }
        rejectPrivateActorSharedMemoryAccess("SyncCell creation");
        SyncCell<T> cell = new SyncCell<>(initialValue);
        syncCells.add(cell);
        if (closed.get()) {
            cell.close();
            throw new IllegalStateException("actor runtime is closed");
        }
        return cell;
    }

    private boolean enterSyncCell(SyncCell<?> cell) {
        SyncCell<?> held = currentSyncCell.get();
        if (held == null) {
            currentSyncCell.set(cell);
            return true;
        }
        if (held != cell) {
            throw new IllegalStateException(
                    "nested synchronization across different SyncCell values is forbidden; "
                            + "snapshot values first or use one shared cell");
        }
        return false;
    }

    private void exitSyncCell(boolean entered) {
        if (entered) currentSyncCell.remove();
    }

    private void rejectPrivateActorSharedMemoryAccess(String operation) {
        ActorCell<?> current = currentActor.get();
        if (current != null && current.kind == ActorKind.PRIVATE) {
            throw new IllegalStateException("private actors cannot access synchronized shared memory via " + operation);
        }
    }

    private void requireSharedActorTurn() {
        ActorCell<?> cell = currentActor.get();
        if (cell == null || cell.kind != ActorKind.SHARED) {
            throw new IllegalStateException("shared state mutation requires a shared actor mailbox turn");
        }
    }

    private void reservePrivateRuntimeBytes(long bytes, ActorId owner, String purpose) {
        synchronized (memoryBudgetLock) {
            long privateBytes = privateMemoryBytes.get();
            long sharedBytes = sharedMemoryBytes.get();
            long total;
            try {
                total = Math.addExact(Math.addExact(privateBytes, sharedBytes), bytes);
            } catch (ArithmeticException overflow) {
                throw new IllegalStateException(purpose + " aggregate accounting overflow");
            }
            if (total > policyCeiling.maxHeapBytes()) {
                throw new IllegalStateException(purpose + " aggregate runtime limit exceeded for " + owner
                        + ": requested=" + bytes + " privateUsed=" + privateBytes
                        + " sharedUsed=" + sharedBytes + " runtimeLimit=" + policyCeiling.maxHeapBytes());
            }
            privateMemoryBytes.addAndGet(bytes);
        }
    }

    private void releasePrivateRuntimeBytes(long bytes, ActorId owner) {
        if (bytes == 0) return;
        synchronized (memoryBudgetLock) {
            long current = privateMemoryBytes.get();
            if (bytes > current) {
                throw new IllegalStateException(
                        "private actor aggregate memory accounting underflow for " + owner
                                + ": release=" + bytes + " privateUsed=" + current);
            }
            privateMemoryBytes.set(current - bytes);
        }
    }

    private void reserveSharedRuntimeBytes(long bytes, String purpose) {
        if (closed.get()) throw new IllegalStateException("actor runtime is closed");
        if (bytes < 0) throw new IllegalArgumentException("shared memory reservation cannot be negative");
        if (bytes == 0) return;
        synchronized (memoryBudgetLock) {
            long privateBytes = privateMemoryBytes.get();
            long sharedBytes = sharedMemoryBytes.get();
            long total;
            try {
                total = Math.addExact(Math.addExact(privateBytes, sharedBytes), bytes);
            } catch (ArithmeticException overflow) {
                throw new IllegalStateException(purpose + " aggregate accounting overflow");
            }
            if (total > policyCeiling.maxHeapBytes()) {
                throw new IllegalStateException(purpose + " aggregate runtime limit exceeded"
                        + ": requested=" + bytes + " privateUsed=" + privateBytes
                        + " sharedUsed=" + sharedBytes + " runtimeLimit=" + policyCeiling.maxHeapBytes());
            }
            sharedMemoryBytes.addAndGet(bytes);
        }
    }

    private void releaseSharedRuntimeBytes(long bytes) {
        if (bytes == 0 || closed.get()) return;
        long remaining = sharedMemoryBytes.addAndGet(-bytes);
        if (remaining < 0) {
            sharedMemoryBytes.set(0);
            throw new IllegalStateException("shared actor memory accounting underflow");
        }
    }

    private void requireWithinCeiling(IsolatePolicy child) {
        if (!policyCeiling.capabilities().containsAll(child.capabilities())) {
            java.util.Set<IsolatePolicy.Capability> excess = java.util.EnumSet.copyOf(child.capabilities());
            excess.removeAll(policyCeiling.capabilities());
            throw new SecurityException("child actor policy exceeds parent capabilities: " + excess);
        }
        if (child.maxHeapBytes() > policyCeiling.maxHeapBytes()) {
            throw new SecurityException("child actor maxHeapBytes exceeds parent policy");
        }
        if (child.maxMailboxMessages() > policyCeiling.maxMailboxMessages()) {
            throw new SecurityException("child actor mailbox limit exceeds parent policy");
        }
        if (child.maxWallTime().compareTo(policyCeiling.maxWallTime()) > 0) {
            throw new SecurityException("child actor wall-time limit exceeds parent policy");
        }
        if (policyCeiling.adversarial() && !child.adversarial()) {
            throw new SecurityException("child actor cannot weaken an adversarial parent policy");
        }
    }

    public boolean isAlive(ActorRef<?> ref) {
        Objects.requireNonNull(ref);
        if (!ref.ownedBy(this)) return false;
        ActorCell<?> cell = actors.get(ref.id());
        return cell != null && !cell.stopped.get();
    }

    public void stop(ActorRef<?> ref) {
        requireCallerRuntimeAffinity("stop actors");
        Objects.requireNonNull(ref);
        if (!ref.ownedBy(this)) {
            throw new IllegalArgumentException("ActorRef belongs to a different ActorRuntime");
        }
        ActorCell<?> cell = actors.get(ref.id());
        if (cell == null) return;

        cell.stop();

        // A host/supervisor stop is a synchronization point: once it returns,
        // private actor memory and actor-count quota have been reclaimed. A
        // self-stop from inside the actor turn cannot wait for itself; endTurn()
        // finalizes it immediately after the current turn unwinds.
        if (currentActor.get() == cell) return;

        try {
            cell.awaitFinalized(CLOSE_WAIT_NANOS);
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
            throw new IllegalStateException(
                    "interrupted while waiting for actor " + ref.id() + " to finalize",
                    interrupted);
        }
        if (!cell.finalized()) {
            throw new IllegalStateException(
                    "actor " + ref.id() + " did not finalize within the stop deadline");
        }
    }

    private ActorTerminatedException terminated(ActorRef<?> ref) {
        return new ActorTerminatedException(ref.id(), ref.kind(), ref.terminationCause.get());
    }

    @SuppressWarnings("unchecked")
    public <M> void send(ActorRef<M> ref, M message) {
        requireCallerRuntimeAffinity("send messages");
        if (closed.get()) throw new IllegalStateException("actor runtime is closed");
        Objects.requireNonNull(ref);
        if (!ref.ownedBy(this)) {
            throw new IllegalArgumentException("ActorRef belongs to a different ActorRuntime");
        }
        ActorCell<M> cell = (ActorCell<M>) actors.get(ref.id());
        if (cell == null || cell.stopped.get()) throw terminated(ref);
        if (!cell.reserveMailboxSlot()) {
            throw new IllegalStateException("actor mailbox limit exceeded for " + ref.id());
        }
        boolean mailboxSlotTransferred = false;
        try {
        validateMessageGraph(message);
        requireMutexTransport(cell, message, new IdentityHashMap<>(), 0);
        requireOwnedActorRefs(message, new IdentityHashMap<>(), 0);
        long runtimeRemaining = Math.max(0L, policyCeiling.maxHeapBytes() - actorMemoryBytes());
        if (cell.kind == ActorKind.SHARED) {
            requireOwnedSharedHandles(message, new IdentityHashMap<>(), 0);
            long actorRemaining = Math.max(0L, cell.policy.maxHeapBytes() - cell.sharedMailboxBytes.get());
            long allowed = Math.min(actorRemaining, runtimeRemaining);
            try {
                estimateSharedTransportBytes(message, new IdentityHashMap<>(), 0, allowed);
            } catch (IllegalStateException tooLarge) {
                throw new IllegalStateException(
                        "shared actor mailbox memory limit exceeded for " + ref.id() + ": " + tooLarge.getMessage(),
                        tooLarge);
            }
        } else {
            long actorRemaining = cell.memorySlice.remainingBytes();
            try {
                estimatePrivateTransportBytes(message, new IdentityHashMap<>(), 0, actorRemaining);
            } catch (IllegalStateException tooLarge) {
                throw new IllegalStateException(
                        "private actor mailbox limit exceeded for " + ref.id() + ": " + tooLarge.getMessage(),
                        tooLarge);
            }
            try {
                estimatePrivateTransportBytes(message, new IdentityHashMap<>(), 0, runtimeRemaining);
            } catch (IllegalStateException aggregateExceeded) {
                throw new IllegalStateException(
                        "private actor aggregate runtime limit exceeded for " + ref.id()
                                + ": " + aggregateExceeded.getMessage(),
                        aggregateExceeded);
            }
        }

        if (closed.get()) throw new IllegalStateException("actor runtime is closed");

        Object prepared = cell.kind == ActorKind.PRIVATE ? isolateCopy(message) : freezeForTransport(message);
        Runnable release;
        if (cell.kind == ActorKind.PRIVATE) {
            MemoryReservation reservation;
            try {
                reservation = cell.memorySlice.reserveMailbox(prepared);
            } catch (IllegalStateException exceeded) {
                throw new IllegalStateException(
                        "private actor mailbox limit exceeded for " + ref.id() + ": " + exceeded.getMessage(),
                        exceeded);
            }
            release = reservation::close;
        } else {
            long bytes = estimateSharedMailboxBytes(prepared, new IdentityHashMap<>(), 0);
            cell.reserveSharedMailbox(bytes);
            release = () -> cell.releaseSharedMailbox(bytes);
        }

        MessageEnvelope envelope = new MessageEnvelope(prepared, release);
        List<OresMutex.Shared<?>> sharedMutexReservations = List.of();
        boolean admitted = false;
        try {
            synchronized (runtimeLifecycleLock) {
                if (closed.get()) throw new IllegalStateException("actor runtime is closed");
                if (cell.kind == ActorKind.SHARED) {
                    sharedMutexReservations = reserveSharedMutexBindings(prepared);
                }
                synchronized (cell.lifecycleLock) {
                    if (cell.stopped.get()) {
                        throw terminated(ref);
                    }
                    if (!cell.mailbox.offer(envelope)) {
                        throw new IllegalStateException("actor mailbox limit exceeded for " + ref.id());
                    }
                    mailboxSlotTransferred = true;
                    admitted = true;
                    commitSharedMutexBindings(sharedMutexReservations);
                }
            }
        } finally {
            if (!admitted) {
                abortSharedMutexBindings(sharedMutexReservations);
                envelope.close();
            }
        }
        cell.schedule();
        } finally {
            if (!mailboxSlotTransferred) cell.releaseMailboxSlot();
        }
    }

    /**
     * Cooperative scheduler hook used by compiler-injected loop safepoints.
     * Carrier threads remain an implementation detail.
     */
    public void schedulerSafepoint() {
        if (closed.get()) throw new CancellationException("actor runtime is closing");
        ActorCell<?> cell = currentActor.get();
        if (cell != null && cell.stopped.get()) {
            throw new CancellationException("actor execution stopped");
        }
        if (Thread.currentThread().isInterrupted()) {
            throw new CancellationException("actor execution interrupted");
        }
        garbageCollector.safepoint();
        Thread.yield();
    }

    @SuppressWarnings("unchecked")
    public <T> Shared<T> shareReadonly(T value) {
        requireCallerRuntimeAffinity("share readonly values");
        if (closed.get()) throw new IllegalStateException("actor runtime is closed");
        IsolatePolicy callerPolicy = currentActorPolicy();
        if (callerPolicy != null) {
            callerPolicy.require(IsolatePolicy.Capability.ACTOR_SHARE_READONLY, "shareReadonly");
        } else {
            policyCeiling.require(IsolatePolicy.Capability.ACTOR_SHARE_READONLY, "shareReadonly");
        }
        rejectPrivateActorSharedMemoryAccess("shareReadonly");
        requireOwnedSharedHandles(value, new IdentityHashMap<>(), 0);
        Object frozen = freeze(value);
        rejectSharedMutableHandles(frozen, new IdentityHashMap<>(), 0);
        long bytes = estimateFrozenBytes(frozen);
        reserveSharedRuntimeBytes(bytes, "shared readonly value");
        Shared<T> shared = new Shared<>((T) frozen, bytes);
        sharedValues.add(shared);
        if (closed.get()) {
            shared.closeFromRuntime();
            throw new IllegalStateException("actor runtime is closed");
        }
        return shared;
    }

    private void requireMutexTransport(
            ActorCell<?> target,
            Object value,
            IdentityHashMap<Object, Boolean> visiting,
            int depth) {
        requireGraphDepth(depth);
        if (value == null || isScalar(value)
                || value instanceof ActorRuntime.ActorRef<?>
                || value instanceof ActorRuntime.ActorGroup<?>) return;
        if (value instanceof OresMutex.Local<?>) {
            throw new IllegalArgumentException("Mutex<T> is actor-local state and cannot cross actor mailboxes");
        }
        if (value instanceof OresMutex.Guard<?>) {
            throw new IllegalArgumentException("MutexGuard<T> is lexical and cannot cross actor mailboxes");
        }
        if (value instanceof OresMutex.Shared<?> sharedMutex) {
            if (target.kind != ActorKind.SHARED) {
                throw new SecurityException("private actors cannot receive SharedMutex<T>");
            }
            ActorKind senderKind = currentActorKind();
            if (senderKind == ActorKind.PRIVATE) {
                throw new SecurityException("private actors cannot send SharedMutex<T>");
            }
            IsolatePolicy senderPolicy = currentActorPolicy();
            if (senderPolicy != null) {
                senderPolicy.require(IsolatePolicy.Capability.SHARED_MEMORY, "SharedMutex actor send");
            } else {
                policyCeiling.require(IsolatePolicy.Capability.SHARED_MEMORY, "SharedMutex host send");
            }
            target.policy.require(IsolatePolicy.Capability.SHARED_MEMORY, "SharedMutex actor receive");
            requireOwnedActorRefs(sharedMutex.transportValue(), new IdentityHashMap<>(), depth + 1);
            return;
        }
        if (value instanceof Shared<?> shared) {
            requireMutexTransport(target, shared.value(), visiting, depth + 1);
            return;
        }
        if (value instanceof SyncCell<?>) return;
        if (visiting.put(value, Boolean.TRUE) != null) {
            throw new IllegalArgumentException("cyclic values cannot cross actor boundaries");
        }
        try {
            if (value instanceof List<?> list) {
                for (Object item : list) requireMutexTransport(target, item, visiting, depth + 1);
            } else if (value instanceof Set<?> set) {
                for (Object item : set) requireMutexTransport(target, item, visiting, depth + 1);
            } else if (value instanceof Map<?, ?> map) {
                for (Map.Entry<?, ?> entry : map.entrySet()) {
                    requireMutexTransport(target, entry.getKey(), visiting, depth + 1);
                    requireMutexTransport(target, entry.getValue(), visiting, depth + 1);
                }
            } else if (value.getClass().isArray()) {
                int length = Array.getLength(value);
                for (int i = 0; i < length; i++) {
                    requireMutexTransport(target, Array.get(value, i), visiting, depth + 1);
                }
            }
        } finally {
            visiting.remove(value);
        }
    }

    private void requireOwnedActorRefs(
            Object value,
            IdentityHashMap<Object, Boolean> visiting,
            int depth) {
        requireGraphDepth(depth);
        if (value == null || isScalar(value)) return;
        if (value instanceof ActorRuntime.ActorRef<?> ref) {
            if (!ref.ownedBy(this)) {
                throw new IllegalArgumentException(
                        "ActorRef belongs to a different ActorRuntime; cross-runtime actor channels require an explicit bridge");
            }
            return;
        }
        if (value instanceof ActorRuntime.ActorGroup<?> group) {
            if (!group.ownedBy(this)) {
                throw new IllegalArgumentException(
                        "ActorGroup belongs to a different ActorRuntime; cross-runtime groups require an explicit bridge");
            }
            if (group.closed()) throw new IllegalArgumentException("ActorGroup is closed");
            return;
        }
        if (value instanceof Shared<?> shared) {
            requireOwnedActorRefs(shared.value(), visiting, depth + 1);
            return;
        }
        if (value instanceof SyncCell<?>) return;
        if (value instanceof OresMutex.Shared<?> sharedMutex) {
            requireOwnedActorRefs(sharedMutex.transportValue(), visiting, depth + 1);
            return;
        }
        if (value instanceof OresMutex.Local<?> || value instanceof OresMutex.Guard<?>) {
            throw new IllegalArgumentException("actor-local mutex state cannot cross actor boundaries");
        }
        if (visiting.put(value, Boolean.TRUE) != null) {
            throw new IllegalArgumentException("cyclic values cannot cross actor boundaries");
        }
        try {
            if (value instanceof List<?> list) {
                for (Object item : list) requireOwnedActorRefs(item, visiting, depth + 1);
            } else if (value instanceof Set<?> set) {
                for (Object item : set) requireOwnedActorRefs(item, visiting, depth + 1);
            } else if (value instanceof Map<?, ?> map) {
                for (Map.Entry<?, ?> entry : map.entrySet()) {
                    requireOwnedActorRefs(entry.getKey(), visiting, depth + 1);
                    requireOwnedActorRefs(entry.getValue(), visiting, depth + 1);
                }
            } else if (value.getClass().isArray()) {
                int length = Array.getLength(value);
                for (int i = 0; i < length; i++) {
                    requireOwnedActorRefs(Array.get(value, i), visiting, depth + 1);
                }
            }
        } finally {
            visiting.remove(value);
        }
    }

    private void requireOwnedSharedHandles(
            Object value,
            IdentityHashMap<Object, Boolean> visiting,
            int depth) {
        requireGraphDepth(depth);
        if (value == null || isScalar(value)) return;
        if (value instanceof ActorRuntime.ActorRef<?> ref) {
            if (!ref.ownedBy(this)) {
                throw new IllegalArgumentException(
                        "ActorRef belongs to a different ActorRuntime; cross-runtime actor channels require an explicit bridge");
            }
            return;
        }
        if (value instanceof ActorRuntime.ActorGroup<?> group) {
            if (!group.ownedBy(this)) {
                throw new IllegalArgumentException(
                        "ActorGroup belongs to a different ActorRuntime; cross-runtime groups require an explicit bridge");
            }
            if (group.closed()) throw new IllegalArgumentException("ActorGroup is closed");
            return;
        }
        if (value instanceof Shared<?> shared) {
            if (!shared.ownedBy(this)) {
                throw new IllegalArgumentException(
                        "Shared value belongs to a different ActorRuntime; copy/freeze it into the destination runtime");
            }
            shared.value();
            return;
        }
        if (value instanceof SyncCell<?> cell) {
            if (!cell.ownedBy(this)) {
                throw new IllegalArgumentException(
                        "SyncCell belongs to a different ActorRuntime and cannot cross shared-memory domains");
            }
            if (cell.closed()) throw new IllegalArgumentException("SyncCell is closed");
            return;
        }
        if (value instanceof OresMutex.Shared<?>) {
            // Runtime affinity is reserved atomically immediately before mailbox admission.
            return;
        }
        if (value instanceof OresMutex.Local<?> || value instanceof OresMutex.Guard<?>) {
            throw new IllegalArgumentException("actor-local mutex state cannot cross shared-memory domains");
        }
        if (visiting.put(value, Boolean.TRUE) != null) {
            throw new IllegalArgumentException("cyclic values cannot cross actor boundaries");
        }
        try {
            if (value instanceof List<?> list) {
                for (Object item : list) requireOwnedSharedHandles(item, visiting, depth + 1);
            } else if (value instanceof Set<?> set) {
                for (Object item : set) requireOwnedSharedHandles(item, visiting, depth + 1);
            } else if (value instanceof Map<?, ?> map) {
                for (Map.Entry<?, ?> entry : map.entrySet()) {
                    requireOwnedSharedHandles(entry.getKey(), visiting, depth + 1);
                    requireOwnedSharedHandles(entry.getValue(), visiting, depth + 1);
                }
            } else if (value.getClass().isArray()) {
                int length = Array.getLength(value);
                for (int i = 0; i < length; i++) {
                    requireOwnedSharedHandles(Array.get(value, i), visiting, depth + 1);
                }
            }
        } finally {
            visiting.remove(value);
        }
    }

    private List<OresMutex.Shared<?>> reserveSharedMutexBindings(Object value) {
        Set<OresMutex.Shared<?>> unique = java.util.Collections.newSetFromMap(new IdentityHashMap<>());
        collectSharedMutexes(value, unique, new IdentityHashMap<>(), 0);

        List<OresMutex.Shared<?>> reserved = new ArrayList<>(unique.size());
        try {
            for (OresMutex.Shared<?> mutex : unique) {
                if (!mutex.reserveRuntimePublication(this)) {
                    throw new IllegalArgumentException(
                            "SharedMutex may cross actor mailboxes only within its owning ActorRuntime");
                }
                reserved.add(mutex);
            }
            return List.copyOf(reserved);
        } catch (RuntimeException | Error failure) {
            abortSharedMutexBindings(reserved);
            throw failure;
        }
    }

    private static void collectSharedMutexes(
            Object value,
            Set<OresMutex.Shared<?>> out,
            IdentityHashMap<Object, Boolean> visiting,
            int depth) {
        requireGraphDepth(depth);
        if (value == null || isScalar(value)
                || value instanceof ActorRuntime.ActorRef<?>
                || value instanceof ActorRuntime.ActorGroup<?>
                || value instanceof SyncCell<?>) return;
        if (value instanceof OresMutex.Shared<?> sharedMutex) {
            out.add(sharedMutex);
            return;
        }
        if (value instanceof Shared<?> shared) {
            collectSharedMutexes(shared.value(), out, visiting, depth + 1);
            return;
        }
        if (value instanceof OresMutex.Local<?> || value instanceof OresMutex.Guard<?>) return;
        if (visiting.put(value, Boolean.TRUE) != null) {
            throw new IllegalArgumentException("cyclic values cannot cross actor boundaries");
        }
        try {
            if (value instanceof List<?> list) {
                for (Object item : list) collectSharedMutexes(item, out, visiting, depth + 1);
            } else if (value instanceof Set<?> set) {
                for (Object item : set) collectSharedMutexes(item, out, visiting, depth + 1);
            } else if (value instanceof Map<?, ?> map) {
                for (Map.Entry<?, ?> entry : map.entrySet()) {
                    collectSharedMutexes(entry.getKey(), out, visiting, depth + 1);
                    collectSharedMutexes(entry.getValue(), out, visiting, depth + 1);
                }
            } else if (value.getClass().isArray()) {
                int length = Array.getLength(value);
                for (int i = 0; i < length; i++) {
                    collectSharedMutexes(Array.get(value, i), out, visiting, depth + 1);
                }
            }
        } finally {
            visiting.remove(value);
        }
    }

    private void commitSharedMutexBindings(List<OresMutex.Shared<?>> reservations) {
        for (OresMutex.Shared<?> mutex : reservations) {
            mutex.commitRuntimePublication(this);
        }
    }

    private void abortSharedMutexBindings(List<OresMutex.Shared<?>> reservations) {
        for (int i = reservations.size() - 1; i >= 0; i--) {
            reservations.get(i).abortRuntimePublication(this);
        }
    }

    private static void rejectSharedMutableHandles(
            Object value,
            IdentityHashMap<Object, Boolean> visiting,
            int depth) {
        requireGraphDepth(depth);
        if (value == null || isScalar(value) || value instanceof ActorRuntime.ActorRef<?>) return;
        if (value instanceof ActorRuntime.ActorGroup<?>) {
            throw new IllegalArgumentException("ActorGroup is a live runtime capability and cannot be wrapped as Shared");
        }
        if (value instanceof ActorRuntime.SyncCell<?>) {
            throw new IllegalArgumentException("SyncCell is mutable shared state and cannot be wrapped as Shared");
        }
        if (value instanceof OresMutex.Shared<?>) {
            throw new IllegalArgumentException("SharedMutex is mutable shared state and cannot be wrapped as Shared");
        }
        if (value instanceof OresMutex.Local<?> || value instanceof OresMutex.Guard<?>) {
            throw new IllegalArgumentException("actor-local mutex state cannot be wrapped as Shared");
        }
        if (value instanceof Shared<?> shared) {
            shared.value();
            return;
        }
        if (visiting.put(value, Boolean.TRUE) != null) {
            throw new IllegalArgumentException("cyclic values cannot be shared read-only");
        }
        try {
            if (value instanceof List<?> list) {
                for (Object item : list) rejectSharedMutableHandles(item, visiting, depth + 1);
            } else if (value instanceof Set<?> set) {
                for (Object item : set) rejectSharedMutableHandles(item, visiting, depth + 1);
            } else if (value instanceof Map<?, ?> map) {
                for (Map.Entry<?, ?> entry : map.entrySet()) {
                    rejectSharedMutableHandles(entry.getKey(), visiting, depth + 1);
                    rejectSharedMutableHandles(entry.getValue(), visiting, depth + 1);
                }
            } else if (value.getClass().isArray()) {
                int length = Array.getLength(value);
                for (int i = 0; i < length; i++) {
                    rejectSharedMutableHandles(Array.get(value, i), visiting, depth + 1);
                }
            } else {
                throw new IllegalArgumentException("value of type " + value.getClass().getName()
                        + " is not a runtime-owned immutable actor value");
            }
        } finally {
            visiting.remove(value);
        }
    }

    private static void validateMessageGraph(Object value) {
        validateMessageGraph(value, new IdentityHashMap<>(), 0, new long[]{0L});
    }

    private static void validateMessageGraph(
            Object value,
            IdentityHashMap<Object, Boolean> visiting,
            int depth,
            long[] nodes) {
        requireGraphDepth(depth);
        if (++nodes[0] > MAX_MESSAGE_GRAPH_NODES) {
            throw new IllegalArgumentException(
                    "actor message graph exceeds maximum node count " + MAX_MESSAGE_GRAPH_NODES);
        }
        if (value == null || isScalar(value)
                || value instanceof ActorRuntime.ActorRef<?>
                || value instanceof ActorRuntime.ActorGroup<?>
                || value instanceof Shared<?>
                || value instanceof SyncCell<?>
                || value instanceof OresMutex.Shared<?>) {
            return;
        }
        if (value instanceof OresMutex.Local<?> || value instanceof OresMutex.Guard<?>) {
            return; // transport-specific validation produces the semantic error.
        }
        if (visiting.put(value, Boolean.TRUE) != null) {
            throw new IllegalArgumentException("cyclic values cannot cross actor boundaries");
        }
        try {
            if (value instanceof List<?> list) {
                requireGraphNodeCapacity(nodes[0], list.size());
                for (Object item : list) validateMessageGraph(item, visiting, depth + 1, nodes);
            } else if (value instanceof Set<?> set) {
                requireGraphNodeCapacity(nodes[0], set.size());
                for (Object item : set) validateMessageGraph(item, visiting, depth + 1, nodes);
            } else if (value instanceof Map<?, ?> map) {
                requireGraphNodeCapacity(nodes[0], Math.multiplyExact((long) map.size(), 2L));
                for (Map.Entry<?, ?> entry : map.entrySet()) {
                    validateMessageGraph(entry.getKey(), visiting, depth + 1, nodes);
                    validateMessageGraph(entry.getValue(), visiting, depth + 1, nodes);
                }
            } else if (value.getClass().isArray()) {
                int length = Array.getLength(value);
                requireGraphNodeCapacity(nodes[0], length);
                for (int i = 0; i < length; i++) {
                    validateMessageGraph(Array.get(value, i), visiting, depth + 1, nodes);
                }
            }
        } finally {
            visiting.remove(value);
        }
    }

    private static void requireGraphNodeCapacity(long alreadyVisited, long additionalNodes) {
        if (additionalNodes < 0
                || additionalNodes > (long) MAX_MESSAGE_GRAPH_NODES - alreadyVisited) {
            throw new IllegalArgumentException(
                    "actor message graph exceeds maximum node count " + MAX_MESSAGE_GRAPH_NODES);
        }
    }

    private static void requireGraphDepth(int depth) {
        if (depth > MAX_MESSAGE_GRAPH_DEPTH) {
            throw new IllegalArgumentException(
                    "actor message graph exceeds maximum nesting depth " + MAX_MESSAGE_GRAPH_DEPTH);
        }
    }

    /**
     * Converts supported values into a deeply immutable/sendable graph.
     * Unknown host objects are rejected instead of being passed by reference.
     */
    public static Object freeze(Object value) {
        validateMessageGraph(value);
        rejectDataFreezeCapabilities(value, new IdentityHashMap<>(), 0);
        return freeze(value, new IdentityHashMap<>(), 0);
    }

    private static Object freezeForTransport(Object value) {
        validateMessageGraph(value);
        return freeze(value, new IdentityHashMap<>(), 0);
    }

    private static void rejectDataFreezeCapabilities(
            Object value,
            IdentityHashMap<Object, Boolean> visiting,
            int depth) {
        requireGraphDepth(depth);
        if (value == null || isScalar(value)) return;
        if (value instanceof ActorRuntime.ActorRef<?>
                || value instanceof ActorRuntime.ActorGroup<?>
                || value instanceof Shared<?>
                || value instanceof SyncCell<?>
                || value instanceof OresMutex.Lock<?>
                || value instanceof OresMutex.Guard<?>) {
            throw new IllegalArgumentException(
                    "freeze() accepts data values only; live actor/shared capabilities require explicit actor transport");
        }
        if (visiting.put(value, Boolean.TRUE) != null) {
            throw new IllegalArgumentException("cyclic values cannot be frozen");
        }
        try {
            if (value instanceof List<?> list) {
                for (Object item : list) rejectDataFreezeCapabilities(item, visiting, depth + 1);
            } else if (value instanceof Set<?> set) {
                for (Object item : set) rejectDataFreezeCapabilities(item, visiting, depth + 1);
            } else if (value instanceof Map<?, ?> map) {
                for (Map.Entry<?, ?> entry : map.entrySet()) {
                    rejectDataFreezeCapabilities(entry.getKey(), visiting, depth + 1);
                    rejectDataFreezeCapabilities(entry.getValue(), visiting, depth + 1);
                }
            } else if (value.getClass().isArray()) {
                int length = Array.getLength(value);
                for (int i = 0; i < length; i++) {
                    rejectDataFreezeCapabilities(Array.get(value, i), visiting, depth + 1);
                }
            }
        } finally {
            visiting.remove(value);
        }
    }

    private static Object freeze(
            Object value,
            IdentityHashMap<Object, Boolean> visiting,
            int depth) {
        requireGraphDepth(depth);
        if (isScalar(value)) return value;
        if (value instanceof Shared<?> shared) {
            shared.value();
            return shared;
        }
        if (value instanceof ActorRuntime.ActorRef<?> ref) return ref;
        if (value instanceof ActorRuntime.ActorGroup<?> group) return group;
        if (value instanceof ActorRuntime.SyncCell<?> cell) return cell;
        if (value instanceof OresMutex.Shared<?> sharedMutex) return sharedMutex;
        if (value instanceof OresMutex.Local<?> || value instanceof OresMutex.Guard<?>) {
            throw new IllegalArgumentException("actor-local mutex state cannot cross actor boundaries");
        }

        if (visiting.put(value, Boolean.TRUE) != null) {
            throw new IllegalArgumentException("cyclic values cannot cross actor boundaries");
        }
        try {
            if (value instanceof List<?> list) {
                List<Object> frozen = new ArrayList<>(list.size());
                for (Object item : list) frozen.add(freeze(item, visiting, depth + 1));
                return List.copyOf(frozen);
            }
            if (value instanceof Set<?> set) {
                LinkedHashSet<Object> frozen = new LinkedHashSet<>();
                for (Object item : set) frozen.add(freeze(item, visiting, depth + 1));
                return Collections.unmodifiableSet(frozen);
            }
            if (value instanceof Map<?, ?> map) {
                Map<Object, Object> frozen = new LinkedHashMap<>();
                for (Map.Entry<?, ?> entry : map.entrySet()) {
                    frozen.put(freeze(entry.getKey(), visiting, depth + 1), freeze(entry.getValue(), visiting, depth + 1));
                }
                return Collections.unmodifiableMap(frozen);
            }
            if (value.getClass().isArray()) {
                int length = Array.getLength(value);
                List<Object> frozen = new ArrayList<>(length);
                for (int i = 0; i < length; i++) {
                    frozen.add(freeze(Array.get(value, i), visiting, depth + 1));
                }
                return List.copyOf(frozen);
            }
            throw new IllegalArgumentException("value of type " + value.getClass().getName()
                    + " is not Sendable; mutable host objects cannot cross actor boundaries");
        } finally {
            visiting.remove(value);
        }
    }

    /**
     * Private transport never retains a shared mutable reference. Immutable
     * shared wrappers are unwrapped and copied into the private message graph.
     */
    private static Object isolateCopy(Object value) {
        return isolateCopy(value, new IdentityHashMap<>(), 0);
    }

    private static Object isolateCopy(
            Object value,
            IdentityHashMap<Object, Boolean> visiting,
            int depth) {
        requireGraphDepth(depth);
        if (isScalar(value)) return value;
        if (value instanceof ActorRuntime.SyncCell<?>) {
            throw new IllegalArgumentException("private actors cannot receive shared SyncCell values");
        }
        if (value instanceof OresMutex.Shared<?>) {
            throw new IllegalArgumentException("private actors cannot receive SharedMutex<T>");
        }
        if (value instanceof OresMutex.Local<?> || value instanceof OresMutex.Guard<?>) {
            throw new IllegalArgumentException("actor-local mutex state cannot cross actor boundaries");
        }
        if (value instanceof Shared<?> shared) return isolateCopy(shared.value(), visiting, depth + 1);
        if (value instanceof ActorRuntime.ActorRef<?> ref) return ref;
        if (value instanceof ActorRuntime.ActorGroup<?> group) return group;

        if (visiting.put(value, Boolean.TRUE) != null) {
            throw new IllegalArgumentException("cyclic values cannot cross private actor boundaries");
        }
        try {
            if (value instanceof List<?> list) {
                List<Object> copy = new ArrayList<>(list.size());
                for (Object item : list) copy.add(isolateCopy(item, visiting, depth + 1));
                return Collections.unmodifiableList(copy);
            }
            if (value instanceof Set<?> set) {
                LinkedHashSet<Object> copy = new LinkedHashSet<>();
                for (Object item : set) copy.add(isolateCopy(item, visiting, depth + 1));
                return Collections.unmodifiableSet(copy);
            }
            if (value instanceof Map<?, ?> map) {
                Map<Object, Object> copy = new LinkedHashMap<>();
                for (Map.Entry<?, ?> entry : map.entrySet()) {
                    copy.put(isolateCopy(entry.getKey(), visiting, depth + 1), isolateCopy(entry.getValue(), visiting, depth + 1));
                }
                return Collections.unmodifiableMap(copy);
            }
            if (value.getClass().isArray()) {
                int length = Array.getLength(value);
                List<Object> copy = new ArrayList<>(length);
                for (int i = 0; i < length; i++) {
                    copy.add(isolateCopy(Array.get(value, i), visiting, depth + 1));
                }
                return Collections.unmodifiableList(copy);
            }
            throw new IllegalArgumentException("value of type " + value.getClass().getName()
                    + " is not Sendable; mutable host objects cannot cross actor boundaries");
        } finally {
            visiting.remove(value);
        }
    }

    private static long estimateSharedTransportBytes(
            Object value,
            IdentityHashMap<Object, Boolean> visiting,
            int depth,
            long limit) {
        requireGraphDepth(depth);
        if (limit < 0) throw new IllegalStateException("message exceeds remaining actor memory");

        long scalar = scalarLogicalBytes(value);
        if (scalar >= 0) return requireWithinLimit(scalar, limit);
        if (value instanceof Shared<?>) return requireWithinLimit(48L, limit);
        if (value instanceof ActorRuntime.SyncCell<?>) return requireWithinLimit(64L, limit);
        if (value instanceof OresMutex.Shared<?>) return requireWithinLimit(64L, limit);
        if (value instanceof OresMutex.Local<?> || value instanceof OresMutex.Guard<?>) {
            throw new IllegalArgumentException("actor-local mutex state cannot cross actor boundaries");
        }
        if (value instanceof ActorRuntime.ActorRef<?>) return requireWithinLimit(48L, limit);
        if (value instanceof ActorRuntime.ActorGroup<?>) return requireWithinLimit(64L, limit);

        if (visiting.put(value, Boolean.TRUE) != null) {
            throw new IllegalArgumentException("cyclic values cannot cross actor boundaries");
        }
        try {
            if (value instanceof List<?> list) {
                long total = requireWithinLimit(containerBase(24L, 8L, list.size()), limit);
                for (Object item : list) {
                    total = addWithinLimit(total,
                            estimateSharedTransportBytes(item, visiting, depth + 1, limit - total),
                            limit);
                }
                return total;
            }
            if (value instanceof Set<?> set) {
                long total = requireWithinLimit(containerBase(24L, 16L, set.size()), limit);
                for (Object item : set) {
                    total = addWithinLimit(total,
                            estimateSharedTransportBytes(item, visiting, depth + 1, limit - total),
                            limit);
                }
                return total;
            }
            if (value instanceof Map<?, ?> map) {
                long total = requireWithinLimit(containerBase(24L, 32L, map.size()), limit);
                for (Map.Entry<?, ?> entry : map.entrySet()) {
                    total = addWithinLimit(total,
                            estimateSharedTransportBytes(entry.getKey(), visiting, depth + 1, limit - total),
                            limit);
                    total = addWithinLimit(total,
                            estimateSharedTransportBytes(entry.getValue(), visiting, depth + 1, limit - total),
                            limit);
                }
                return total;
            }
            if (value.getClass().isArray()) {
                int length = Array.getLength(value);
                long total = requireWithinLimit(containerBase(24L, 8L, length), limit);
                for (int i = 0; i < length; i++) {
                    total = addWithinLimit(total,
                            estimateSharedTransportBytes(
                                    Array.get(value, i), visiting, depth + 1, limit - total),
                            limit);
                }
                return total;
            }
            throw new IllegalArgumentException("value of type " + value.getClass().getName()
                    + " is not Sendable; mutable host objects cannot cross actor boundaries");
        } finally {
            visiting.remove(value);
        }
    }

    private static long estimateSharedMailboxBytes(
            Object value,
            IdentityHashMap<Object, Boolean> seen,
            int depth) {
        requireGraphDepth(depth);
        long scalar = scalarLogicalBytes(value);
        if (scalar >= 0) return scalar;
        if (value instanceof Shared<?>) return 48L;
        if (value instanceof ActorRuntime.SyncCell<?>) return 64L;
        if (value instanceof OresMutex.Shared<?>) return 64L;
        if (value instanceof OresMutex.Local<?> || value instanceof OresMutex.Guard<?>) {
            throw new IllegalArgumentException("actor-local mutex state cannot cross actor boundaries");
        }
        if (value instanceof ActorRuntime.ActorRef<?>) return 48L;
        if (value instanceof ActorRuntime.ActorGroup<?>) return 64L;
        if (seen.put(value, Boolean.TRUE) != null) return 0L;

        long bytes = 24L;
        if (value instanceof List<?> list) {
            bytes = Math.addExact(bytes, 8L * list.size());
            for (Object item : list) {
                bytes = Math.addExact(bytes, estimateSharedMailboxBytes(item, seen, depth + 1));
            }
            return bytes;
        }
        if (value instanceof Set<?> set) {
            bytes = Math.addExact(bytes, 16L * set.size());
            for (Object item : set) {
                bytes = Math.addExact(bytes, estimateSharedMailboxBytes(item, seen, depth + 1));
            }
            return bytes;
        }
        if (value instanceof Map<?, ?> map) {
            bytes = Math.addExact(bytes, 32L * map.size());
            for (Map.Entry<?, ?> entry : map.entrySet()) {
                bytes = Math.addExact(bytes, estimateSharedMailboxBytes(entry.getKey(), seen, depth + 1));
                bytes = Math.addExact(bytes, estimateSharedMailboxBytes(entry.getValue(), seen, depth + 1));
            }
            return bytes;
        }
        return 64L;
    }

    /**
     * Validates and estimates a private-actor message before allocating its
     * isolation copy. The walk short-circuits as soon as the destination or
     * parent-runtime budget cannot admit the logical graph.
     */
    private static long estimatePrivateTransportBytes(
            Object value,
            IdentityHashMap<Object, Boolean> visiting,
            int depth,
            long limit) {
        requireGraphDepth(depth);
        if (limit < 0) throw new IllegalStateException("message exceeds remaining actor memory");

        long scalar = scalarLogicalBytes(value);
        if (scalar >= 0) return requireWithinLimit(scalar, limit);

        if (value instanceof ActorRuntime.SyncCell<?>) {
            throw new IllegalArgumentException("private actors cannot receive shared SyncCell values");
        }
        if (value instanceof Shared<?> shared) {
            return estimatePrivateTransportBytes(shared.value(), visiting, depth + 1, limit);
        }
        if (value instanceof ActorRuntime.ActorRef<?>) return requireWithinLimit(48L, limit);
        if (value instanceof ActorRuntime.ActorGroup<?>) return requireWithinLimit(64L, limit);
        if (value instanceof ActorRuntime.ActorGroup<?>) return requireWithinLimit(64L, limit);

        if (visiting.put(value, Boolean.TRUE) != null) {
            throw new IllegalArgumentException("cyclic values cannot cross private actor boundaries");
        }
        try {
            if (value instanceof List<?> list) {
                long total = requireWithinLimit(containerBase(24L, 8L, list.size()), limit);
                for (Object item : list) {
                    total = addWithinLimit(total,
                            estimatePrivateTransportBytes(item, visiting, depth + 1, limit - total),
                            limit);
                }
                return total;
            }
            if (value instanceof Set<?> set) {
                long total = requireWithinLimit(containerBase(24L, 16L, set.size()), limit);
                for (Object item : set) {
                    total = addWithinLimit(total,
                            estimatePrivateTransportBytes(item, visiting, depth + 1, limit - total),
                            limit);
                }
                return total;
            }
            if (value instanceof Map<?, ?> map) {
                long total = requireWithinLimit(containerBase(24L, 32L, map.size()), limit);
                for (Map.Entry<?, ?> entry : map.entrySet()) {
                    total = addWithinLimit(total,
                            estimatePrivateTransportBytes(entry.getKey(), visiting, depth + 1, limit - total),
                            limit);
                    total = addWithinLimit(total,
                            estimatePrivateTransportBytes(entry.getValue(), visiting, depth + 1, limit - total),
                            limit);
                }
                return total;
            }
            if (value.getClass().isArray()) {
                int length = Array.getLength(value);
                long total = requireWithinLimit(containerBase(24L, 8L, length), limit);
                for (int i = 0; i < length; i++) {
                    total = addWithinLimit(total,
                            estimatePrivateTransportBytes(
                                    Array.get(value, i), visiting, depth + 1, limit - total),
                            limit);
                }
                return total;
            }
            throw new IllegalArgumentException("value of type " + value.getClass().getName()
                    + " is not Sendable; mutable host objects cannot cross actor boundaries");
        } finally {
            visiting.remove(value);
        }
    }

    private static long scalarLogicalBytes(Object value) {
        if (value == null) return 8L;
        if (value instanceof Boolean || value instanceof Byte || value instanceof Short
                || value instanceof Character || value instanceof Integer || value instanceof Float) return 16L;
        if (value instanceof Long || value instanceof Double) return 24L;
        if (value instanceof BigInteger integer) return 32L + integer.toByteArray().length;
        if (value instanceof BigDecimal decimal) return 48L + decimal.unscaledValue().toByteArray().length;
        if (value instanceof String string) return 40L + (long) string.length() * 2L;
        if (value instanceof UUID || value instanceof ActorId || value instanceof ActorGroupId) return 40L;
        if (value instanceof Enum<?>) return 24L;
        return -1L;
    }

    private static long containerBase(long header, long perEntry, int count) {
        try {
            return Math.addExact(header, Math.multiplyExact(perEntry, (long) count));
        } catch (ArithmeticException overflow) {
            throw new IllegalStateException("actor message size accounting overflow");
        }
    }

    private static long requireWithinLimit(long bytes, long limit) {
        if (bytes > limit) {
            throw new IllegalStateException(
                    "message requires at least " + bytes + " bytes but only " + limit + " remain");
        }
        return bytes;
    }

    private static long addWithinLimit(long left, long right, long limit) {
        long total;
        try {
            total = Math.addExact(left, right);
        } catch (ArithmeticException overflow) {
            throw new IllegalStateException("actor message size accounting overflow");
        }
        return requireWithinLimit(total, limit);
    }

    /**
     * Conservative language-level footprint estimate. This is a quota metric,
     * not a promise about HotSpot/Graal object layout.
     */
    private static long estimateFrozenBytes(Object value) {
        return estimateFrozenBytes(value, new IdentityHashMap<>(), 0);
    }

    private static long estimateFrozenBytes(
            Object value,
            IdentityHashMap<Object, Boolean> seen,
            int depth) {
        requireGraphDepth(depth);
        if (value == null) return 8L;
        if (value instanceof Boolean || value instanceof Byte || value instanceof Short
                || value instanceof Character || value instanceof Integer || value instanceof Float) return 16L;
        if (value instanceof Long || value instanceof Double) return 24L;
        if (value instanceof BigInteger integer) return 32L + integer.toByteArray().length;
        if (value instanceof BigDecimal decimal) return 48L + decimal.unscaledValue().toByteArray().length;
        if (value instanceof String string) return 40L + (long) string.length() * 2L;
        if (value instanceof UUID || value instanceof ActorId || value instanceof ActorGroupId) return 40L;
        if (value instanceof Enum<?>) return 24L;
        if (value instanceof ActorRuntime.ActorRef<?>) return 48L;
        if (value instanceof ActorRuntime.ActorGroup<?>) return 64L;
        if (value instanceof ActorRuntime.SyncCell<?>) return 64L;
        if (value instanceof OresMutex.Shared<?>) return 64L;
        if (value instanceof OresMutex.Local<?> || value instanceof OresMutex.Guard<?>) {
            throw new IllegalArgumentException("actor-local mutex state cannot be frozen");
        }
        if (value instanceof Shared<?> shared) return estimateFrozenBytes(shared.value(), seen, depth + 1);

        if (seen.put(value, Boolean.TRUE) != null) return 0L;

        long bytes = 24L;
        if (value instanceof List<?> list) {
            bytes = Math.addExact(bytes, 8L * list.size());
            for (Object item : list) bytes = Math.addExact(bytes, estimateFrozenBytes(item, seen, depth + 1));
            return bytes;
        }
        if (value instanceof Set<?> set) {
            bytes = Math.addExact(bytes, 16L * set.size());
            for (Object item : set) bytes = Math.addExact(bytes, estimateFrozenBytes(item, seen, depth + 1));
            return bytes;
        }
        if (value instanceof Map<?, ?> map) {
            bytes = Math.addExact(bytes, 32L * map.size());
            for (Map.Entry<?, ?> entry : map.entrySet()) {
                bytes = Math.addExact(bytes, estimateFrozenBytes(entry.getKey(), seen, depth + 1));
                bytes = Math.addExact(bytes, estimateFrozenBytes(entry.getValue(), seen, depth + 1));
            }
            return bytes;
        }
        if (value.getClass().isArray()) {
            int length = Array.getLength(value);
            bytes = Math.addExact(bytes, 8L * length);
            for (int i = 0; i < length; i++) {
                bytes = Math.addExact(bytes, estimateFrozenBytes(Array.get(value, i), seen, depth + 1));
            }
            return bytes;
        }
        return 64L;
    }

    private static void zeroBuffer(ByteBuffer buffer) {
        if (buffer == null) return;
        ByteBuffer duplicate = buffer.duplicate();
        duplicate.clear();
        while (duplicate.hasRemaining()) duplicate.put((byte) 0);
    }

    private static boolean isScalar(Object value) {
        return value == null || value instanceof String || value instanceof Boolean || value instanceof Character
                || value instanceof Byte || value instanceof Short || value instanceof Integer || value instanceof Long
                || value instanceof Float || value instanceof Double || value instanceof BigInteger || value instanceof BigDecimal
                || value instanceof Enum<?> || value instanceof UUID || value instanceof ActorId
                || value instanceof ActorGroupId;
    }

    @Override
    public void close() {
        requireSupervisorContext("close an ActorRuntime");

        final boolean firstClose;
        final List<ActorCell<?>> snapshot;
        synchronized (runtimeLifecycleLock) {
            firstClose = closed.compareAndSet(false, true);
            snapshot = List.copyOf(actors.values());
        }
        for (ActorCell<?> cell : snapshot) cell.stop();

        if (firstClose) {
            // Interrupt carrier workers. Actor turns that deliberately consume
            // the interrupt are still tracked below and prevent close from
            // reporting success until they actually leave the runtime.
            privateDispatcher.shutdownNow();
            sharedDispatcher.shutdownNow();
        }

        long deadline = System.nanoTime() + CLOSE_WAIT_NANOS;
        boolean interrupted = false;
        List<ActorId> stillRunning = new ArrayList<>();
        for (ActorCell<?> cell : snapshot) {
            long remaining = deadline - System.nanoTime();
            if (remaining > 0) {
                try {
                    cell.awaitFinalized(remaining);
                } catch (InterruptedException waitInterrupted) {
                    interrupted = true;
                    break;
                }
            }
            if (!cell.finalized()) stillRunning.add(cell.ref.id());
        }

        List<SyncCell<?>> syncSnapshot;
        synchronized (syncCells) {
            syncSnapshot = List.copyOf(syncCells);
        }
        for (SyncCell<?> cell : syncSnapshot) cell.invalidateFromRuntime();
        syncCells.clear();
        List<Shared<?>> sharedSnapshot;
        synchronized (sharedValues) {
            sharedSnapshot = List.copyOf(sharedValues);
        }
        for (Shared<?> shared : sharedSnapshot) shared.closeFromRuntime();
        sharedValues.clear();
        garbageCollector.close();
        sharedMemoryBytes.set(0L);

        if (interrupted) Thread.currentThread().interrupt();
        if (!stillRunning.isEmpty() || interrupted) {
            if (interrupted) {
                for (ActorCell<?> cell : snapshot) {
                    if (!cell.finalized() && !stillRunning.contains(cell.ref.id())) {
                        stillRunning.add(cell.ref.id());
                    }
                }
            }
            throw new IllegalStateException(
                    "ActorRuntime close did not observe full actor termination: "
                            + stillRunning.size() + " actor(s) still running");
        }
        actors.clear();
        actorCount.set(0);
    }

    private ExecutorService dispatcherFor(ActorKind kind) {
        return kind == ActorKind.PRIVATE ? privateDispatcher : sharedDispatcher;
    }

    private static ExecutorService newDispatcher(
            int parallelism,
            int readyQueueCapacity,
            String threadPrefix) {
        ThreadPoolExecutor executor = new ThreadPoolExecutor(
                parallelism,
                parallelism,
                0L,
                TimeUnit.MILLISECONDS,
                new ArrayBlockingQueue<>(readyQueueCapacity),
                namedFactory(threadPrefix),
                new ThreadPoolExecutor.AbortPolicy());
        // Core workers are created lazily on first scheduled actor turn.
        return executor;
    }

    private static ThreadFactory namedFactory(String prefix) {
        AtomicInteger next = new AtomicInteger();
        return task -> {
            Thread thread = new Thread(task, prefix + next.incrementAndGet());
            thread.setDaemon(true);
            return thread;
        };
    }

    private final class ActorCell<M> {
        private final ActorRef<M> ref;
        private final ActorKind kind;
        private final IsolatePolicy policy;
        private final BehaviorFactory<M> behaviorFactory;
        private final boolean trustedFactory;
        private final BlockingQueue<MessageEnvelope> mailbox;
        private final ActorMemorySlice memorySlice;
        private final AtomicBoolean scheduled = new AtomicBoolean();
        private final AtomicBoolean stopped = new AtomicBoolean();
        private final AtomicInteger queuedMessages = new AtomicInteger();
        private final AtomicLong sharedMailboxBytes = new AtomicLong();
        private final Object lifecycleLock = new Object();
        private final Object executionDomain = new Object();
        private int activeTurns;
        private boolean finalized;
        private Behavior<M> behavior;
        private ReceiveFuture pendingReceive;
        private int manuallyReceivedThisBatch;

        private ActorCell(
                ActorRef<M> ref,
                ActorKind kind,
                IsolatePolicy policy,
                BehaviorFactory<M> behaviorFactory,
                boolean trustedFactory) {
            this.ref = ref;
            this.kind = kind;
            this.policy = policy;
            this.behaviorFactory = behaviorFactory;
            this.trustedFactory = trustedFactory;
            this.mailbox = new LinkedBlockingQueue<>(policy.maxMailboxMessages());
            this.memorySlice = kind == ActorKind.PRIVATE
                    ? new ActorMemorySlice(ref.id(), policy.maxHeapBytes())
                    : null;
        }

        private boolean reserveMailboxSlot() {
            while (true) {
                int current = queuedMessages.get();
                if (current >= policy.maxMailboxMessages()) return false;
                if (queuedMessages.compareAndSet(current, current + 1)) return true;
            }
        }

        private void releaseMailboxSlot() {
            int remaining = queuedMessages.decrementAndGet();
            if (remaining < 0) {
                queuedMessages.incrementAndGet();
                throw new IllegalStateException(
                        "actor mailbox accounting underflow for " + ref.id());
            }
        }

        private boolean beginTurn() {
            synchronized (lifecycleLock) {
                if (stopped.get() || finalized) return false;
                activeTurns++;
                return true;
            }
        }

        private void endTurn() {
            synchronized (lifecycleLock) {
                if (activeTurns <= 0) {
                    throw new IllegalStateException("actor active-turn accounting underflow for " + ref.id());
                }
                activeTurns--;
                if (stopped.get() && activeTurns == 0) finalizeStopLocked();
                lifecycleLock.notifyAll();
            }
        }

        private void finalizeStopLocked() {
            if (finalized || activeTurns != 0) return;
            finalized = true;
            drainMailboxReservations();
            if (memorySlice != null) memorySlice.close();
            garbageCollector.releaseActor(ref.id());
            unregisterActor(this);
            lifecycleLock.notifyAll();
        }

        private boolean finalized() {
            synchronized (lifecycleLock) {
                return finalized;
            }
        }

        private void awaitFinalized(long remainingNanos) throws InterruptedException {
            long deadline = System.nanoTime() + Math.max(0L, remainingNanos);
            synchronized (lifecycleLock) {
                while (!finalized) {
                    long remaining = deadline - System.nanoTime();
                    if (remaining <= 0) return;
                    long millis = Math.max(1L, TimeUnit.NANOSECONDS.toMillis(remaining));
                    lifecycleLock.wait(millis);
                }
            }
        }

        private void reserveSharedMailbox(long bytes) {
            if (bytes < 0) throw new IllegalArgumentException("shared mailbox reservation cannot be negative");
            synchronized (lifecycleLock) {
                if (closed.get()) throw new IllegalStateException("actor runtime is closed");
                if (stopped.get()) throw terminated(ref);
                long current = sharedMailboxBytes.get();
                long next;
                try {
                    next = Math.addExact(current, bytes);
                } catch (ArithmeticException overflow) {
                    throw new IllegalStateException("shared actor mailbox memory accounting overflow");
                }
                if (next > policy.maxHeapBytes()) {
                    throw new IllegalStateException("shared actor mailbox memory limit exceeded for " + ref.id()
                            + ": requested=" + bytes + " used=" + current + " limit=" + policy.maxHeapBytes());
                }
                reserveSharedRuntimeBytes(bytes, "shared actor mailbox");
                sharedMailboxBytes.set(next);
            }
        }

        private void releaseSharedMailbox(long bytes) {
            if (bytes == 0) return;
            synchronized (lifecycleLock) {
                long current = sharedMailboxBytes.get();
                long next = Math.max(0L, current - bytes);
                sharedMailboxBytes.set(next);
                releaseSharedRuntimeBytes(bytes);
            }
        }

        private void schedule() {
            if (stopped.get() || closed.get()) return;
            if (!scheduled.compareAndSet(false, true)) return;
            try {
                dispatcherFor(kind).execute(this::runBatch);
            } catch (RejectedExecutionException rejected) {
                scheduled.set(false);
                stop();
                if (!closed.get()) throw rejected;
            }
        }

        private void runBatch() {
            ACTOR_CARRIER.set(Boolean.TRUE);
            try {
                turnExecutor.execute(this::runBatchEntered);
            } catch (Throwable failure) {
                fail(failure);
                scheduled.set(false);
                if (failure instanceof VirtualMachineError fatal) throw fatal;
                if (failure instanceof ThreadDeath fatal) throw fatal;
                if (failure instanceof LinkageError fatal) throw fatal;
            } finally {
                ACTOR_CARRIER.remove();
            }
        }

        @SuppressWarnings("unchecked")
        private void runBatchEntered() {
            currentActor.set(this);
            CURRENT_ACTOR_EXECUTION.set(new ActorExecutionContext(
                    ActorRuntime.this, ref.id(), kind, policy, executionDomain));
            boolean turnActive = beginTurn();
            try {
                if (!turnActive) return;

                ActorContext<M> context = new ActorContext<>() {
                    @Override public ActorRef<M> self() { return ref; }
                    @Override public ActorRuntime runtime() { return ActorRuntime.this; }
                    @Override public IsolatePolicy policy() { return policy; }
                    @Override public ActorKind kind() { return kind; }
                    @Override public Optional<ActorMemorySlice> privateMemory() {
                        return Optional.ofNullable(memorySlice);
                    }
                    @Override public Optional<M> tryReceive() { return ActorCell.this.tryReceive(); }
                    @Override public CompletionStage<M> receiveAsync() { return ActorCell.this.receiveAsync(); }
                };

                manuallyReceivedThisBatch = 0;
                if (behavior == null) {
                    Behavior<M> created = Objects.requireNonNull(
                            behaviorFactory.create(context),
                            "actor behaviorFactory returned null");
                    if (kind == ActorKind.PRIVATE && !trustedFactory) {
                        validatePrivateBehaviorState(ref.id(), created);
                    }
                    behavior = created;
                }

                int processed = 0;
                while (processed + manuallyReceivedThisBatch < dispatcherConfig.throughput()
                        && !stopped.get()) {
                    MessageEnvelope envelope = mailbox.poll();
                    if (envelope == null) break;
                    releaseMailboxSlot();
                    ReceiveFuture receiver = claimPendingReceive();
                    if (receiver != null) {
                        M message;
                        try (envelope) { message = (M) envelope.value(); }
                        if (!receiver.completeFromRuntime(message)) {
                            throw new IllegalStateException(
                                    "actor receive completion lost runtime ownership for " + ref.id());
                        }
                        if (kind == ActorKind.PRIVATE && !trustedFactory) {
                            validatePrivateBehaviorState(ref.id(), behavior);
                        }
                    } else {
                        try (envelope) {
                            behavior.onMessage((M) envelope.value(), context);
                            if (kind == ActorKind.PRIVATE && !trustedFactory) {
                                validatePrivateBehaviorState(ref.id(), behavior);
                            }
                        }
                    }
                    processed++;
                }
            } catch (Throwable failure) {
                // Fail-stop supervision for ordinary actor failures. Fatal VM
                // errors are cleaned up and then rethrown rather than swallowed.
                fail(failure);
                if (failure instanceof VirtualMachineError fatal) throw fatal;
                if (failure instanceof ThreadDeath fatal) throw fatal;
                if (failure instanceof LinkageError fatal) throw fatal;
            } finally {
                if (turnActive) endTurn();
                CURRENT_ACTOR_EXECUTION.remove();
                currentActor.remove();
                scheduled.set(false);
                garbageCollector.safepoint();

                if (!stopped.get() && !closed.get() && !mailbox.isEmpty()) {
                    // Bounded batch/throughput handoff for dispatcher fairness.
                    schedule();
                }
            }
        }

        /**
         * Runtime-owned completion for one mailbox pull.
         *
         * Callers may attach dependent stages and await it, but cannot forge a
         * mailbox delivery, cancel it from an arbitrary thread, inject timeout
         * completion, or obtrude a value. Those operations would otherwise race
         * the mailbox claim and could silently drop or fabricate messages.
         */
        private final class ReceiveFuture extends CompletableFuture<M> {
            private boolean completeFromRuntime(M value) {
                return super.complete(value);
            }

            private boolean failFromRuntime(Throwable failure) {
                return super.completeExceptionally(failure);
            }

            @Override public boolean complete(M value) {
                throw new UnsupportedOperationException("actor receive completion is runtime-owned");
            }

            @Override public boolean completeExceptionally(Throwable ex) {
                throw new UnsupportedOperationException("actor receive completion is runtime-owned");
            }

            @Override public CompletableFuture<M> completeAsync(
                    java.util.function.Supplier<? extends M> supplier) {
                throw new UnsupportedOperationException("actor receive completion is runtime-owned");
            }

            @Override public CompletableFuture<M> completeAsync(
                    java.util.function.Supplier<? extends M> supplier,
                    java.util.concurrent.Executor executor) {
                throw new UnsupportedOperationException("actor receive completion is runtime-owned");
            }

            @Override public boolean cancel(boolean mayInterruptIfRunning) {
                throw new UnsupportedOperationException(
                        "actor receive cancellation must be coordinated by the actor runtime");
            }

            @Override public CompletableFuture<M> orTimeout(long timeout, TimeUnit unit) {
                throw new UnsupportedOperationException(
                        "actor receive timeouts must be expressed by actor/runtime timeout semantics");
            }

            @Override public CompletableFuture<M> completeOnTimeout(
                    M value, long timeout, TimeUnit unit) {
                throw new UnsupportedOperationException(
                        "actor receive timeout completion is runtime-owned");
            }

            @Override public void obtrudeValue(M value) {
                throw new UnsupportedOperationException("actor receive completion is runtime-owned");
            }

            @Override public void obtrudeException(Throwable ex) {
                throw new UnsupportedOperationException("actor receive completion is runtime-owned");
            }
        }

        private void requireCurrentTurn(String operation) {
            if (currentActor.get() != this || CURRENT_ACTOR_EXECUTION.get() == null) {
                throw new IllegalStateException(operation + " is valid only during this actor's mailbox turn");
            }
            if (stopped.get() || closed.get()) throw terminated(ref);
        }

        @SuppressWarnings("unchecked")
        private Optional<M> tryReceive() {
            requireCurrentTurn("tryReceive");
            if (manuallyReceivedThisBatch >= dispatcherConfig.throughput()) return Optional.empty();
            MessageEnvelope envelope = mailbox.poll();
            if (envelope == null) return Optional.empty();
            releaseMailboxSlot();
            manuallyReceivedThisBatch++;
            try (envelope) { return Optional.ofNullable((M) envelope.value()); }
        }

        private CompletionStage<M> receiveAsync() {
            requireCurrentTurn("receiveAsync");
            synchronized (lifecycleLock) {
                if (stopped.get() || closed.get()) throw terminated(ref);
                if (pendingReceive != null && !pendingReceive.isDone()) {
                    throw new IllegalStateException(
                            "actor already has an outstanding receiveAsync; one mailbox must have exactly one pull consumer");
                }
                pendingReceive = new ReceiveFuture();
                return pendingReceive;
            }
        }

        private ReceiveFuture claimPendingReceive() {
            synchronized (lifecycleLock) {
                ReceiveFuture receiver = pendingReceive;
                if (receiver == null) return null;
                pendingReceive = null;
                return receiver.isDone() ? null : receiver;
            }
        }

        private ReceiveFuture detachPendingReceiveLocked() {
            ReceiveFuture receiver = pendingReceive;
            pendingReceive = null;
            return receiver;
        }

        private void drainMailboxReservations() {
            MessageEnvelope envelope;
            while ((envelope = mailbox.poll()) != null) {
                releaseMailboxSlot();
                envelope.close();
            }
        }

        private void fail(Throwable failure) {
            ReceiveFuture receiver;
            synchronized (lifecycleLock) {
                ref.terminationCause.compareAndSet(null, failure);
                stopped.set(true);
                receiver = detachPendingReceiveLocked();
                drainMailboxReservations();
                finalizeStopLocked();
            }
            if (receiver != null && !receiver.isDone()) receiver.failFromRuntime(terminated(ref));
        }

        private void stop() {
            ReceiveFuture receiver;
            synchronized (lifecycleLock) {
                stopped.set(true);
                receiver = detachPendingReceiveLocked();
                drainMailboxReservations();
                finalizeStopLocked();
            }
            if (receiver != null && !receiver.isDone()) receiver.failFromRuntime(terminated(ref));
        }
    }
}
