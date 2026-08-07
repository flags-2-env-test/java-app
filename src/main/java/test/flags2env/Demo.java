package test.flags2env;

import com.oresoftware.flags2env.Flags2Env;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;

/**
 * Java consumer of oresoftware/flags-2-env.
 *
 * <p>Asserts the contract in EXPECTED.md. Exits non-zero on the first
 * disagreement, which is what makes {@code docker run} the whole test.
 *
 * <p>Java is the only client here that does not load the shared object the FFI
 * clients load. It binds through a JNI shim, so the artifact is
 * {@code libflags2env_jni.so} and it is resolved from {@code java.library.path}
 * rather than from {@code FLAGS2ENV_NATIVE_LIB}.
 */
public final class Demo {

  private record Case(String label, List<String> flags, Map<String, String> expected) {}

  private static Map<String, String> envMap(String port, String debug, String appEnv, String color) {
    Map<String, String> values = new TreeMap<>();
    values.put("PORT", port);
    values.put("DEBUG", debug);
    values.put("APP_ENV", appEnv);
    values.put("COLOR", color);
    return values;
  }

  public static void main(String[] args) {
    String config = args.length > 0 ? args[0] : ".cli-flags.toml";

    Map<String, String> defaults = envMap("3000", "false", "development", "true");
    Map<String, String> overridden = envMap("8181", "true", "production", "true");
    Map<String, String> negated = envMap("3000", "false", "development", "false");

    List<Case> cases =
        List.of(
            new Case("defaults", List.of(), defaults),
            new Case(
                "long flags",
                List.of("--port", "8181", "--debug=t", "--mode", "production"),
                overridden),
            new Case(
                "short flags", List.of("-p", "8181", "-d", "1", "--env", "production"), overridden),
            new Case(
                "long aliases",
                List.of("--listen-port", "8181", "--debug", "1", "--mode", "production"),
                overridden),
            new Case(
                "joined by =",
                List.of("--port=8181", "--debug=yes", "--mode=production"),
                overridden),
            new Case("negation", List.of("--no-color"), negated));

    int failures = 0;

    for (Case testCase : cases) {
      List<String> argv = new ArrayList<>();
      argv.add("demo");
      argv.addAll(testCase.flags());

      Map<String, String> got = new TreeMap<>(Flags2Env.parse(config, argv.toArray(new String[0])));
      boolean ok = got.equals(testCase.expected());
      if (!ok) {
        failures++;
      }

      System.out.printf(
          "%-4s %-13s demo %s%n",
          ok ? "ok" : "FAIL", testCase.label(), String.join(" ", testCase.flags()));
      for (String key : testCase.expected().keySet()) {
        System.out.printf("       %s=%s%n", key, got.getOrDefault(key, "<missing>"));
      }
      if (!ok) {
        System.err.printf("       expected %s%n", new LinkedHashMap<>(testCase.expected()));
        System.err.printf("       got      %s%n", got);
      }
    }

    if (failures > 0) {
      System.err.printf(
          "%njava-app: %d of %d cases disagree with the contract%n", failures, cases.size());
      System.exit(1);
    }

    System.out.printf(
        "%njava-app OK: %d cases, via JNI into oresoftware/flags-2-env%n", cases.size());
  }

  private Demo() {}
}
