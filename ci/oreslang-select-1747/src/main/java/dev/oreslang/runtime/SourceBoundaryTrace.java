package dev.oreslang.runtime;

import java.util.Objects;

/** Bounded source diagnostics that never capture guest frames, receivers or heaps. */
public final class SourceBoundaryTrace extends RuntimeException {
    private static final int MAX_BOUNDARIES = 64;
    private static final int MAX_CODE_UNIT_LENGTH = 160;
    private static final int MAX_CALLABLE_LENGTH = 96;
    private static final int MAX_BOUNDARY_LENGTH = 64;

    private SourceBoundaryTrace(String codeUnit, String callable, String boundary) {
        super("ores " + sanitized(boundary, MAX_BOUNDARY_LENGTH) + " "
                + sanitized(callable, MAX_CALLABLE_LENGTH) + " ["
                + sanitized(codeUnit, MAX_CODE_UNIT_LENGTH) + "]", null, false, false);
    }

    /**
     * Avoid control-sequence, newline, bidi-spoofing and unbounded label injection
     * in stack traces. Metadata is diagnostic-only and must never invoke guest
     * formatting or capture guest values.
     */
    private static String sanitized(String value, int maxLength) {
        if (value == null || value.isEmpty()) return "<unknown>";
        StringBuilder label = new StringBuilder(Math.min(value.length(), maxLength));
        for (int offset = 0; offset < value.length();) {
            int point = value.codePointAt(offset);
            int width = Character.charCount(point);
            if (label.length() + width > maxLength) {
                label.append('…');
                break;
            }
            offset += width;
            int kind = Character.getType(point);
            if (Character.isISOControl(point) || kind == Character.FORMAT
                    || kind == Character.LINE_SEPARATOR || kind == Character.PARAGRAPH_SEPARATOR
                    || kind == Character.SURROGATE) {
                label.append('?');
            } else {
                label.appendCodePoint(point);
            }
        }
        return label.toString();
    }

    /** Preserve the original failure type, cause and message, including cancellation. */
    public static <T extends Throwable> T record(
            T failure, String codeUnit, String callable, String boundary) {
        Objects.requireNonNull(failure, "failure");
        synchronized (failure) {
            if (failure.getSuppressed().length < MAX_BOUNDARIES) {
                failure.addSuppressed(new SourceBoundaryTrace(codeUnit, callable, boundary));
            }
        }
        return failure;
    }
}
