package dev.oreslang.runtime;

import java.util.Objects;
import java.util.concurrent.CancellationException;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.Future;

/** Central adapter for the single Oreslang await surface. */
public final class AwaitSupport {
    private AwaitSupport() { }

    public interface Awaitable<T> {
        CompletionStage<T> completion();
    }

    public static Object await(Object value) {
        if (value instanceof Awaitable<?> awaitable) {
            return awaitStage(Objects.requireNonNull(awaitable.completion(), "Awaitable.completion returned null"));
        }
        if (value instanceof CompletionStage<?> stage) return awaitStage(stage);
        if (value instanceof Future<?> future) return awaitFuture(future);
        if (value instanceof Thread thread) return awaitThread(thread);
        return value;
    }

    private static Object awaitStage(CompletionStage<?> stage) {
        var future = stage.toCompletableFuture();
        rejectPendingActorWait(future.isDone(), "CompletionStage");
        try {
            return future.join();
        } catch (CompletionException failure) {
            throw propagate(failure.getCause() == null ? failure : failure.getCause());
        }
    }

    private static Object awaitFuture(Future<?> future) {
        rejectPendingActorWait(future.isDone(), "Future");
        try {
            return future.get();
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
            CancellationException cancelled = new CancellationException("await interrupted");
            cancelled.initCause(interrupted);
            throw cancelled;
        } catch (ExecutionException failure) {
            throw propagate(failure.getCause() == null ? failure : failure.getCause());
        }
    }

    private static Object awaitThread(Thread thread) {
        if (thread.getState() == Thread.State.NEW) {
            throw new IllegalArgumentException("cannot await a thread that has not been started");
        }
        rejectPendingActorWait(!thread.isAlive(), thread.isVirtual() ? "virtual thread" : "thread");
        try {
            thread.join();
            return null;
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
            CancellationException cancelled = new CancellationException("await interrupted");
            cancelled.initCause(interrupted);
            throw cancelled;
        }
    }

    private static void rejectPendingActorWait(boolean complete, String kind) {
        if (ActorRuntime.inActorExecution() && !complete) {
            throw new IllegalStateException(
                    "await on pending " + kind + " would block an actor dispatcher carrier; "
                            + "actor continuation lowering must suspend/resume the mailbox turn");
        }
    }

    private static RuntimeException propagate(Throwable failure) {
        if (failure instanceof RuntimeException runtime) return runtime;
        if (failure instanceof Error error) throw error;
        return new CompletionException(failure);
    }
}
