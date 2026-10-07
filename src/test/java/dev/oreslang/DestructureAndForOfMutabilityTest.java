package dev.oreslang;

import dev.oreslang.parser.Parser;
import dev.oreslang.types.TypeChecker;
import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.assertDoesNotThrow;
import static org.junit.jupiter.api.Assertions.assertThrows;

final class DestructureAndForOfMutabilityTest {
    private static void accepts(String source) {
        assertDoesNotThrow(() -> TypeChecker.check(Parser.parse(source)));
    }

    private static void rejects(String source) {
        assertThrows(IllegalArgumentException.class, () -> TypeChecker.check(Parser.parse(source)));
    }

    @Test void prefixedDestructurePreservesAllFourModes() {
        accepts("""
                fnc ok(): void {
                  val [item] = [struct infer{foo: "a"}];
                  item.foo = "b";
                  return;
                }
                """);
        rejects("""
                fnc bad(): void {
                  val [item] = [struct infer{foo: "a"}];
                  item = struct infer{foo: "b"};
                  return;
                }
                """);

        accepts("""
                fnc ok(): void {
                  let [item] = [struct infer{foo: "a"}];
                  item = struct infer{foo: "b"};
                  return;
                }
                """);
        rejects("""
                fnc bad(): void {
                  let [item] = [struct infer{foo: "a"}];
                  item.foo = "b";
                  return;
                }
                """);

        accepts("""
                fnc ok(): void {
                  let mut [item] = [struct infer{foo: "a"}];
                  item.foo = "b";
                  item = struct infer{foo: "c"};
                  return;
                }
                """);
        rejects("""
                fnc bad(): void {
                  const [item] = [struct infer{foo: "a"}];
                  item.foo = "b";
                  return;
                }
                """);
    }

    @Test void inlineDestructureKindsPreserveReferentPermission() {
        accepts("""
                fnc ok(): void {
                  [val item] = [struct infer{foo: "a"}];
                  item.foo = "b";
                  return;
                }
                """);
        accepts("""
                fnc ok(): void {
                  [let mut item] = [struct infer{foo: "a"}];
                  item.foo = "b";
                  item = struct infer{foo: "c"};
                  return;
                }
                """);
    }

    @Test void forOfValIsFixedMutableAndLetIsRebindableReadonly() {
        accepts("""
                fnc ok(): void {
                  for val item of arr[struct infer{foo: "a"}] do
                    item.foo = "b";
                  done
                  return;
                }
                """);
        rejects("""
                fnc bad(): void {
                  for val item of arr[struct infer{foo: "a"}] do
                    item = struct infer{foo: "b"};
                  done
                  return;
                }
                """);

        accepts("""
                fnc ok(): void {
                  for let item of arr[struct infer{foo: "a"}] do
                    item = struct infer{foo: "b"};
                  done
                  return;
                }
                """);
        rejects("""
                fnc bad(): void {
                  for let item of arr[struct infer{foo: "a"}] do
                    item.foo = "b";
                  done
                  return;
                }
                """);

        accepts("""
                fnc ok(): void {
                  for let mut item of arr[struct infer{foo: "a"}] do
                    item.foo = "b";
                    item = struct infer{foo: "c"};
                  done
                  return;
                }
                """);
    }

    @Test void forOfDestructureCarriesInheritedMutability() {
        accepts("""
                fnc ok(): void {
                  for val [item] of arr[[struct infer{foo: "a"}]] do
                    item.foo = "b";
                  done
                  return;
                }
                """);
        accepts("""
                fnc ok(): void {
                  for let mut [item] of arr[[struct infer{foo: "a"}]] do
                    item.foo = "b";
                    item = struct infer{foo: "c"};
                  done
                  return;
                }
                """);
        rejects("""
                fnc bad(): void {
                  for const [item] of arr[[struct infer{foo: "a"}]] do
                    item.foo = "b";
                  done
                  return;
                }
                """);
    }
}
