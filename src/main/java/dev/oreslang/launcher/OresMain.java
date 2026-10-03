package dev.oreslang.launcher;

import dev.oreslang.runtime.ExecutionProfile;
import dev.oreslang.runtime.IsolatePolicy;
import dev.oreslang.runtime.LinkedProgramRunner;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

public final class OresMain {
    private static final Pattern POSITIONED_DIAGNOSTIC = Pattern.compile(
            "^Oreslang\\s+(?:lexer|parse)\\s+error\\s+at\\s+(\\d+):(\\d+):\\s*(.*)$",
            Pattern.CASE_INSENSITIVE);

    private OresMain() { }

    public static void main(String[] args) throws Exception {
        boolean strict = false;
        boolean checkOnly = false;
        String mode = "jit";
        String platform = "server";
        List<IsolatePolicy.Capability> additionalCapabilities = new ArrayList<>();
        String filename = null;

        for (String arg : args) {
            if (arg.equals("--strict-isolate")) strict = true;
            else if (arg.equals("--check")) checkOnly = true;
            else if (arg.startsWith("--mode=")) mode = arg.substring("--mode=".length());
            else if (arg.startsWith("--platform=")) platform = arg.substring("--platform=".length());
            else if (arg.startsWith("--allow=")) {
                String raw = arg.substring("--allow=".length());
                if (!raw.isBlank()) {
                    for (String value : raw.split(",")) {
                        additionalCapabilities.add(IsolatePolicy.Capability.valueOf(value.trim().toUpperCase(Locale.ROOT)));
                    }
                }
            } else if (arg.startsWith("--")) {
                throw new IllegalArgumentException("unknown option: " + arg);
            } else if (filename == null) filename = arg;
            else throw new IllegalArgumentException("only one .ores file may be supplied");
        }

        if (filename == null) {
            System.err.println("usage: oreslang-compiler [--check] [--strict-isolate] [--mode=aot|jit|hybrid] [--platform=server|windows|macos|linux|android|ios] [--allow=CAP,...] <file.ores>");
            System.exit(2);
            return;
        }

        Path path = Path.of(filename);
        if (!Files.isRegularFile(path)) throw new IllegalArgumentException("not a file: " + path);

        if (checkOnly) {
            try {
                LinkedProgramRunner.validate(path);
            } catch (Exception error) {
                System.err.println(formatCheckDiagnostic(path, error));
                System.exit(1);
            }
            return;
        }

        ExecutionProfile profile = ExecutionProfile.parse(mode, platform);
        IsolatePolicy policy = strict ? IsolatePolicy.strictFaas() : IsolatePolicy.developer();
        if (!additionalCapabilities.isEmpty()) {
            policy = policy.withCapabilities(additionalCapabilities.toArray(IsolatePolicy.Capability[]::new));
        }

        LinkedProgramRunner.run(path, policy, profile, System.out, System.err);
    }

    static String formatCheckDiagnostic(Path path, Exception error) {
        String message = error.getMessage();
        if (message == null || message.isBlank()) message = error.getClass().getSimpleName();

        Matcher matcher = POSITIONED_DIAGNOSTIC.matcher(message);
        if (matcher.matches()) {
            return path.toAbsolutePath().normalize()
                    + ":" + matcher.group(1)
                    + ":" + matcher.group(2)
                    + ": error: " + matcher.group(3);
        }

        return path.toAbsolutePath().normalize() + ":1:1: error: " + message;
    }
}
