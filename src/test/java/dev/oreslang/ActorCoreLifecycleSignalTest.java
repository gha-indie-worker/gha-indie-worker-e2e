package dev.oreslang;

import dev.oreslang.compiler.OresCompiler;
import org.graalvm.polyglot.Context;
import org.graalvm.polyglot.PolyglotException;
import org.graalvm.polyglot.Source;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.Timeout;

import java.io.ByteArrayOutputStream;
import java.nio.charset.StandardCharsets;

import static org.junit.jupiter.api.Assertions.*;

@Timeout(20)
final class ActorCoreLifecycleSignalTest {
    @Test
    void inheritedSignalsAreAsyncOptionValuesForEveryActorKind() throws Exception {
        String program = """
                define actor Shared as
                  receive(ActorMail<String> mail): void {
                    self.end();
                    return;
                  }
                end

                define isoactor Private as
                  receive(ActorMail<String> mail): void {
                    self.end();
                    return;
                  }
                end

                define untrusted actor Untrusted as
                  receive(ActorMail<String> mail): void {
                    self.end();
                    return;
                  }
                end

                pub async routine main(): void {
                  val a = spawn Shared();
                  val b = spawn Private();
                  val c = spawn Untrusted();

                  val Future<Option<bool>> pending = a.get_ready_signal();
                  val Option<bool> aReady = await pending;
                  val Option<bool> bReady = await b.get_ready_signal();
                  val Option<bool> cReady = await c.get_ready_signal();
                  stdio.stdout.write(aReady.unwrap());
                  stdio.stdout.write(":");
                  stdio.stdout.write(bReady.unwrap());
                  stdio.stdout.write(":");
                  stdio.stdout.write(cReady.unwrap());

                  a.send("end");
                  b.send("end");
                  c.send("end");

                  val Option<bool> aDone = await a.get_done_signal();
                  val Option<bool> bDone = await b.get_done_signal();
                  val Option<bool> cDone = await c.get_done_signal();
                  stdio.stdout.write(":");
                  stdio.stdout.write(aDone.unwrap());
                  stdio.stdout.write(":");
                  stdio.stdout.write(bDone.unwrap());
                  stdio.stdout.write(":");
                  stdio.stdout.write(cDone.unwrap());
                  return;
                }
                """;
        assertDoesNotThrow(() -> OresCompiler.parseAndTypeCheck(program));
        assertEquals("true:true:true:true:true:true", run(program));
    }

    @Test
    void normalStartupFailureMapsToNoneWithoutFakingSuccessfulReadinessOrDone() throws Exception {
        String program = """
                define actor Worker as
                  val int required;

                  on_start(): void {
                    return;
                  }

                  receive(ActorMail<String> mail): void {
                    self.end();
                    return;
                  }
                end

                pub async routine main(): void {
                  val worker = spawn Worker();
                  val Option<bool> ready = await worker.get_ready_signal();
                  val Option<bool> done = await worker.get_done_signal();
                  stdio.stdout.write(ready.is_none());
                  stdio.stdout.write(":");
                  stdio.stdout.write(done.is_none());
                  return;
                }
                """;
        assertDoesNotThrow(() -> OresCompiler.parseAndTypeCheck(program));
        assertEquals("true:true", run(program));
    }

    @Test
    void inheritedSelfReadinessRunsOnTheActorExecutionLane() throws Exception {
        String program = """
                define actor Worker as
                  receive(ActorMail<String> mail): void {
                    val Option<bool> ownReady = await self.get_ready_signal();
                    self.send(ownReady.unwrap());
                    self.end();
                    return;
                  }
                end

                pub async routine main(): void {
                  val worker = spawn Worker();
                  await worker.get_ready_signal();
                  worker.send("go");
                  val output = (await worker.outputs.next()).value.unwrap();
                  stdio.stdout.write(output.value);
                  await worker.get_done_signal();
                  return;
                }
                """;
        assertDoesNotThrow(() -> OresCompiler.parseAndTypeCheck(program));
        assertEquals("true", run(program));
    }

    @Test
    void sealedHiddenCoreMethodsCannotBeShadowedByUserActorMembers() {
        IllegalArgumentException method = assertThrows(IllegalArgumentException.class,
                () -> OresCompiler.parseAndTypeCheck("""
                        define actor Worker as
                          get_ready_signal(): bool {
                            return false;
                          }

                          receive(ActorMail<int> mail): void {
                            self.end();
                            return;
                          }
                        end
                        """));
        assertTrue(method.getMessage().contains("sealed hidden actor-core"));

        IllegalArgumentException field = assertThrows(IllegalArgumentException.class,
                () -> OresCompiler.parseAndTypeCheck("""
                        define actor Worker as
                          let bool get_done_signal = false;

                          receive(ActorMail<int> mail): void {
                            self.end();
                            return;
                          }
                        end
                        """));
        assertTrue(field.getMessage().contains("shadows a sealed hidden actor-core"));
    }

    @Test
    void signalsAreZeroArgumentAndActorReferencesRemainDirectCallOnly() {
        assertThrows(IllegalArgumentException.class,
                () -> OresCompiler.parseAndTypeCheck("""
                        define actor Worker as
                          receive(ActorMail<int> mail): void {
                            self.end();
                            return;
                          }
                        end

                        fnc bad(): void {
                          val worker = spawn Worker();
                          worker.get_ready_signal(true);
                          return;
                        }
                        """));
        assertThrows(IllegalArgumentException.class,
                () -> OresCompiler.parseAndTypeCheck("""
                        define actor Worker as
                          receive(ActorMail<int> mail): void {
                            self.end();
                            return;
                          }
                        end

                        fnc bad(): void {
                          val worker = spawn Worker();
                          val method = worker.get_done_signal;
                          return;
                        }
                        """));
    }

    private static String run(String sourceText) throws Exception {
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        Source source = Source.newBuilder(OresLanguage.ID, sourceText, "actor-core-signals.ores")
                .mimeType(OresLanguage.MIME_TYPE)
                .build();
        try (Context context = Context.newBuilder(OresLanguage.ID)
                .allowAllAccess(false)
                .out(output)
                .build()) {
            context.eval(source);
        }
        return output.toString(StandardCharsets.UTF_8);
    }
}
