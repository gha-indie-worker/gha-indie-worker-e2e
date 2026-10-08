package dev.oreslang.nodes;

import org.junit.jupiter.api.Test;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import static org.junit.jupiter.api.Assertions.*;

/** Guest output and debug formatting must tolerate cyclic owned graphs. */
final class CycleSafeDisplayTest {
    @Test
    void unchangedAcyclicContainerDisplay() {
        Map<String, Object> map = new LinkedHashMap<>();
        map.put("name", "Node");
        map.put("values", List.of(1, 2));
        assertEquals("{name=Node, values=[1, 2]}",
                OresEvalRootNode.CycleSafeDisplay.render(map));
    }

    @Test
    void selfAndMutualCyclesAreBounded() {
        List<Object> self = new ArrayList<>();
        self.add(self);
        assertEquals("[<cycle>]", OresEvalRootNode.CycleSafeDisplay.render(self));
        Map<String, Object> left = new LinkedHashMap<>();
        List<Object> right = new ArrayList<>();
        left.put("right", right);
        right.add(left);
        assertEquals("{right=[<cycle>]}", OresEvalRootNode.CycleSafeDisplay.render(left));
    }

    @Test
    void duplicatedAcyclicSubgraphsAreNotCycles() {
        List<Integer> leaf = List.of(1, 2);
        assertEquals("[[1, 2], [1, 2]]",
                OresEvalRootNode.CycleSafeDisplay.render(List.of(leaf, leaf)));
    }

    @Test
    void outputIsBoundedByDepthAndCharacters() {
        Object nested = "leaf";
        for (int i = 0; i < 150; i++) nested = List.of(nested);
        assertTrue(OresEvalRootNode.CycleSafeDisplay.render(nested).contains("<truncated>"));
        assertTrue(OresEvalRootNode.CycleSafeDisplay.render("x".repeat(200000)).length() <= 65536);
    }
}
