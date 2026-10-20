package dev.oreslang;

import dev.oreslang.compiler.OresCompiler;
import org.graalvm.polyglot.Context;
import org.graalvm.polyglot.Source;
import org.junit.jupiter.api.Test;
import java.io.ByteArrayOutputStream;
import java.nio.charset.StandardCharsets;
import static org.junit.jupiter.api.Assertions.*;

/**
 * Hoisting is legal only for loop-invariant READ descriptors. Source-site FAIR
 * tickets and every per-invocation waiter/winner remain unique.
 */
final class SelectLoopDescriptorHoistTest {
    @Test
    void immutableChannelCanBeReusedInForOfAndLoop() throws Exception {
        String program = """
                pub fnc main(): void {
                  val Channel<int> inbox = Channel.new<int>(4);
                  for val number of [10, 20, 30] {
                    writech inbox, number;
                    select first {
                      when readch inbox: val message {
                        stdio.stdout.write(message);
                        stdio.stdout.write(":");
                      }
                    }
                  }
                  let int remaining = 2;
                  loop {
                    writech inbox, remaining;
                    try select first {
                      when readch inbox: val message {
                        stdio.stdout.write(message);
                        stdio.stdout.write(":");
                      }
                    }
                    remaining = remaining - 1;
                    if remaining == 0 then break; fi
                  }
                  return;
                }
                """;
        assertDoesNotThrow(() -> OresCompiler.parseAndTypeCheck(program));
        assertEquals("10:20:30:2:1:", run(program));
    }

    @Test
    void shadowingPerIterationChannelCannotBeHoisted() throws Exception {
        String program = """
                pub fnc main(): void {
                  for val turn of [1, 2, 3] {
                    val Channel<int> inbox = Channel.new<int>(1);
                    writech inbox, turn;
                    select first {
                      when readch inbox: val value {
                        stdio.stdout.write(value);
                      }
                    }
                  }
                  return;
                }
                """;
        assertDoesNotThrow(() -> OresCompiler.parseAndTypeCheck(program));
        assertEquals("123", run(program));
    }

    @Test
    void mutableChannelBindingIsObservedEveryIteration() throws Exception {
        String program = """
                pub fnc main(): void {
                  val Channel<int> a = Channel.new<int>(1);
                  val Channel<int> b = Channel.new<int>(1);
                  writech a, 11;
                  writech b, 22;
                  let Channel<int> chosen = a;
                  for val turn of [1, 2] {
                    if turn == 2 then chosen = b; fi
                    select first {
                      when readch chosen: val value {
                        stdio.stdout.write(value);
                        stdio.stdout.write(":");
                      }
                      default: {
                        stdio.stdout.write("miss:");
                      }
                    }
                  }
                  return;
                }
                """;
        assertDoesNotThrow(() -> OresCompiler.parseAndTypeCheck(program));
        assertEquals("11:22:", run(program));
    }

    @Test
    void cacheIsPerFunctionInvocationNotPerSharedAst() throws Exception {
        String program = """
                pub fnc drain(Channel<int> source): void {
                  for val turn of [1] {
                    select first {
                      when readch source: val value {
                        stdio.stdout.write(value);
                      }
                      default: { stdio.stdout.write("miss"); }
                    }
                  }
                  return;
                }
                pub fnc main(): void {
                  val Channel<int> a = Channel.new<int>(1);
                  val Channel<int> b = Channel.new<int>(1);
                  writech a, 3;
                  writech b, 4;
                  drain(a);
                  drain(b);
                  return;
                }
                """;
        assertDoesNotThrow(() -> OresCompiler.parseAndTypeCheck(program));
        assertEquals("34", run(program));
    }

    @Test
    void fairSiteTicketsStillAlternateWhenDescriptorsAreHoisted() throws Exception {
        String program = """
                pub fnc main(): void {
                  val Channel<int> a = Channel.new<int>(8);
                  val Channel<int> b = Channel.new<int>(8);
                  for val turn of [1, 2, 3, 4] {
                    writech a, turn;
                    writech b, turn;
                    select fair {
                      when readch a: val value { stdio.stdout.write("A"); }
                      when readch b: val value { stdio.stdout.write("B"); }
                    }
                  }
                  return;
                }
                """;
        assertDoesNotThrow(() -> OresCompiler.parseAndTypeCheck(program));
        assertEquals("ABAB", run(program));
    }

    private static String run(String source) throws Exception {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        Source program = Source.newBuilder(OresLanguage.ID, source, "select-hoist.ores")
                .mimeType(OresLanguage.MIME_TYPE).build();
        try (Context context = Context.newBuilder(OresLanguage.ID)
                .allowAllAccess(false).out(out).build()) {
            context.eval(program);
        }
        return out.toString(StandardCharsets.UTF_8);
    }
}
