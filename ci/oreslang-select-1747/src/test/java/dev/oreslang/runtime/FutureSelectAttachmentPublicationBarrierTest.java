package dev.oreslang.runtime;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.Timeout;

import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.List;

import static org.junit.jupiter.api.Assertions.*;

/**
 * A winning select must not become observable until any outstanding listener
 * attachment has reclaimed its late losing registration.
 */
@Timeout(15)
final class FutureSelectAttachmentPublicationBarrierTest {
    @Test
    @SuppressWarnings("unchecked")
    void channelWinnerIsWithheldUntilFutureAttachmentBarrierIsReleased() throws Exception {
        var producer = new OresFuture<Integer>();
        var channel = new ChannelRuntime.Channel<Integer>(1);
        var set = ChannelRuntime.SelectSet.of(
                ChannelRuntime.await(producer), ChannelRuntime.read(channel));

        // Reflection is intentional: deterministically hold the exact window
        // that normal concurrent attachment exposes, without a production-only
        // test hook or probabilistic thread scheduling.
        Class<?> registrationClass = Class.forName(
                "dev.oreslang.runtime.ChannelRuntime$SelectRegistration");
        Constructor<?> constructor = registrationClass.getDeclaredConstructor(
                ChannelRuntime.SelectSet.class, ChannelRuntime.SelectPolicy.class);
        constructor.setAccessible(true);
        Object registration = constructor.newInstance(
                set, ChannelRuntime.SelectPolicy.PRIORITY);

        Field outstanding = registrationClass.getDeclaredField("attachingFutureWaiters");
        outstanding.setAccessible(true);
        outstanding.setInt(registration, 1);

        Method start = registrationClass.getDeclaredMethod("start", boolean.class);
        start.setAccessible(true);
        start.invoke(registration, false);

        Field selectedFutureField = registrationClass.getDeclaredField("future");
        selectedFutureField.setAccessible(true);
        OresFuture<ChannelRuntime.SelectResult> selected =
                (OresFuture<ChannelRuntime.SelectResult>) selectedFutureField.get(registration);
        assertEquals(1, producer.pendingRuntimeWaiterCount());

        assertTrue(channel.tryWrite(31));
        assertFalse(selected.isDone(), "winner published during a claimed listener attachment");
        assertEquals(0, producer.pendingRuntimeWaiterCount(),
                "already attached losing subscribers must be reclaimed before publication");

        Field deferredField = registrationClass.getDeclaredField("deferredPublication");
        deferredField.setAccessible(true);
        ArrayList<Runnable> deferred = (ArrayList<Runnable>) deferredField.get(registration);
        assertEquals(1, deferred.size());

        // Model the final attachment returning: after the losing listener has
        // detached, its single deferred terminal publication may become visible.
        List<Runnable> publish = List.copyOf(deferred);
        deferred.clear();
        outstanding.setInt(registration, 0);
        publish.forEach(Runnable::run);

        var result = selected.join();
        assertEquals(1, result.index());
        assertEquals(31, result.value());
        assertEquals(0, producer.pendingRuntimeWaiterCount());
        assertFalse(producer.isDone(), "losing source Future must not be cancelled");
    }

    @Test
    @SuppressWarnings("unchecked")
    void terminalFailureIsAlsoWithheldBehindAttachmentBarrier() throws Exception {
        var producer = new OresFuture<Integer>();
        var channel = new ChannelRuntime.Channel<Integer>(0);
        var set = ChannelRuntime.SelectSet.of(
                ChannelRuntime.await(producer), ChannelRuntime.read(channel));

        Class<?> registrationClass = Class.forName(
                "dev.oreslang.runtime.ChannelRuntime$SelectRegistration");
        Constructor<?> constructor = registrationClass.getDeclaredConstructor(
                ChannelRuntime.SelectSet.class, ChannelRuntime.SelectPolicy.class);
        constructor.setAccessible(true);
        Object registration = constructor.newInstance(
                set, ChannelRuntime.SelectPolicy.PRIORITY);
        Field outstanding = registrationClass.getDeclaredField("attachingFutureWaiters");
        outstanding.setAccessible(true);
        outstanding.setInt(registration, 1);
        Method start = registrationClass.getDeclaredMethod("start", boolean.class);
        start.setAccessible(true);
        start.invoke(registration, false);
        Field futureField = registrationClass.getDeclaredField("future");
        futureField.setAccessible(true);
        OresFuture<ChannelRuntime.SelectResult> selected =
                (OresFuture<ChannelRuntime.SelectResult>) futureField.get(registration);

        channel.close(new IllegalStateException("closed while attaching"));
        assertFalse(selected.isDone(), "terminal failure was published before detachment");
        assertEquals(0, producer.pendingRuntimeWaiterCount());

        Field deferredField = registrationClass.getDeclaredField("deferredPublication");
        deferredField.setAccessible(true);
        ArrayList<Runnable> deferred = (ArrayList<Runnable>) deferredField.get(registration);
        assertEquals(1, deferred.size());
        List<Runnable> publish = List.copyOf(deferred);
        deferred.clear();
        outstanding.setInt(registration, 0);
        publish.forEach(Runnable::run);

        var failure = assertThrows(java.util.concurrent.CompletionException.class, selected::join);
        assertInstanceOf(ChannelRuntime.ChannelClosedException.class, failure.getCause());
        assertFalse(producer.isDone());
    }
}
