// A small LSP client for the compat suite (java.txt): starts Eclipse JDT LS
// (jdtls, unpacked under /opt/jdtls) on a Java file with a type error, collects
// the diagnostics, asks for the hover on a call, shuts the server down, and
// prints what happened, one word or message a line:
//
//   java JdtlsClient.java FILE.java
//
// There is no JSON library in the JDK, so the messages are built by hand and
// the answers are searched as text.
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.TimeUnit;
import java.util.regex.*;

public class JdtlsClient {
    static InputStream in;
    static OutputStream out;
    static final StringBuilder diagnostics = new StringBuilder();
    static final Pattern RESPONSE = Pattern.compile("\\{\\s*\"jsonrpc\"\\s*:\\s*\"2\\.0\"\\s*,\\s*\"id\"\\s*:\\s*(\\d+)\\s*,\\s*\"(?:result|error)\"");
    static final Pattern REQUEST = Pattern.compile("\\{\\s*\"jsonrpc\"\\s*:\\s*\"2\\.0\"\\s*,\\s*(?:\"id\"\\s*:\\s*(\\d+|\"[^\"]*\")\\s*,\\s*)?\"method\"\\s*:\\s*\"([^\"]*)\"");

    static void send(String json) throws IOException {
        byte[] b = json.getBytes(StandardCharsets.UTF_8);
        out.write(("Content-Length: " + b.length + "\r\n\r\n").getBytes(StandardCharsets.US_ASCII));
        out.write(b);
        out.flush();
    }

    static String read() throws IOException {
        int len = -1;
        StringBuilder line = new StringBuilder();
        while (true) {
            int c = in.read();
            if (c < 0) throw new EOFException("the server closed its output");
            if (c == '\n') {
                String l = line.toString().trim();
                line.setLength(0);
                if (l.isEmpty()) break;
                if (l.toLowerCase().startsWith("content-length:")) len = Integer.parseInt(l.substring(15).trim());
            } else {
                line.append((char) c);
            }
        }
        byte[] body = in.readNBytes(len);
        return new String(body, StandardCharsets.UTF_8);
    }

    // The answer to request ID. What the server sends meanwhile is dealt with:
    // its own requests are answered, its diagnostics are kept.
    static String await(int id) throws IOException {
        while (true) {
            String m = read();
            if (Boolean.getBoolean("debug")) System.err.println("<< " + (m.length() > 160 ? m.substring(0, 160) : m));
            // The server writes {"jsonrpc":"2.0","id":N,"result"|"error":...} for an
            // answer and {"jsonrpc":"2.0",["id":N,]"method":...} for a request or a
            // notification: the words in the rest of a message prove nothing.
            Matcher resp = RESPONSE.matcher(m);
            if (resp.lookingAt()) {
                if (resp.group(1).equals(String.valueOf(id))) return m;
                continue;
            }
            Matcher req = REQUEST.matcher(m);
            if (!req.lookingAt()) continue;
            String sid = req.group(1);
            String method = req.group(2);
            if (sid != null) {
                send("{\"jsonrpc\":\"2.0\",\"id\":" + sid + ",\"result\":" + (method.equals("workspace/configuration") ? "[]" : "null") + "}");
            } else if (method.equals("textDocument/publishDiagnostics")) {
                diagnostics.append(m).append('\n');
            }
        }
    }

    static String request(int id, String method, String params) throws IOException {
        send("{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"method\":\"" + method + "\",\"params\":" + params + "}");
        return await(id);
    }

    static void notify(String method, String params) throws IOException {
        send("{\"jsonrpc\":\"2.0\",\"method\":\"" + method + "\",\"params\":" + params + "}");
    }

    static String quote(String s) {
        return "\"" + s.replace("\\", "\\\\").replace("\"", "\\\"").replace("\n", "\\n").replace("\t", "\\t") + "\"";
    }

    public static void main(String[] args) throws Exception {
        Path file = Paths.get(args[0]).toAbsolutePath();
        Path root = file.getParent();
        String uri = file.toUri().toString();
        String text = Files.readString(file);
        Path home = Paths.get("/opt/jdtls");
        Path launcher;
        try (var s = Files.list(home.resolve("plugins"))) {
            launcher = s.filter(p -> p.getFileName().toString().startsWith("org.eclipse.equinox.launcher_")
                    && p.getFileName().toString().endsWith(".jar")).findFirst().get();
        }
        Path data = Files.createTempDirectory("jdtls-data");
        List<String> cmd = new ArrayList<>(List.of(Paths.get(System.getProperty("java.home"), "bin", "java").toString(),
                "-Declipse.application=org.eclipse.jdt.ls.core.id1", "-Dosgi.bundles.defaultStartLevel=4",
                "-Declipse.product=org.eclipse.jdt.ls.core.product", "-Dlog.level=ERROR", "-Xmx512m",
                "--add-modules=ALL-SYSTEM", "--add-opens", "java.base/java.util=ALL-UNNAMED",
                "--add-opens", "java.base/java.lang=ALL-UNNAMED",
                "-jar", launcher.toString(), "-configuration", home.resolve("config_linux").toString(),
                "-data", data.toString()));
        ProcessBuilder pb = new ProcessBuilder(cmd);
        Path err = Files.createTempFile("jdtls-err", ".txt");
        pb.redirectError(err.toFile());
        pb.environment().remove("JAVA_TOOL_OPTIONS");
        Process p = pb.start();
        in = new BufferedInputStream(p.getInputStream());
        out = p.getOutputStream();
        boolean killed = false;
        try {
            String init = request(1, "initialize", "{\"processId\":null,\"rootUri\":" + quote(root.toUri().toString())
                    + ",\"capabilities\":{\"textDocument\":{\"hover\":{\"contentFormat\":[\"plaintext\"]}}},"
                    + "\"workspaceFolders\":[{\"uri\":" + quote(root.toUri().toString()) + ",\"name\":\"p\"}]}");
            System.out.println(init.contains("\"hoverProvider\"") ? "initialized" : "no-init");
            notify("initialized", "{}");
            notify("textDocument/didOpen", "{\"textDocument\":{\"uri\":" + quote(uri)
                    + ",\"languageId\":\"java\",\"version\":1,\"text\":" + quote(text) + "}}");
            // The diagnostics come when the file has been compiled, the hover
            // is answered once the server is ready: ask until it does.
            long end = System.nanoTime() + TimeUnit.SECONDS.toNanos(Long.getLong("wait", 420));
            String hover = "no-reply";
            int id = 2;
            while (System.nanoTime() < end) {
                String h = request(id++, "textDocument/hover", "{\"textDocument\":{\"uri\":" + quote(uri)
                        + "},\"position\":{\"line\":4,\"character\":28}}");
                if (h.contains("add(int a, int b)")) { hover = "hover-add"; break; }
                Thread.sleep(2000);
            }
            System.out.println(hover);
            while (System.nanoTime() < end && !diagnostics.toString().contains("cannot convert from String to int")) {
                // keep reading: the diagnostics of the file may still be on their way
                request(id++, "textDocument/hover", "{\"textDocument\":{\"uri\":" + quote(uri)
                        + "},\"position\":{\"line\":0,\"character\":0}}");
                Thread.sleep(1000);
            }
            Matcher dm = Pattern.compile("Type mismatch: cannot convert from String to int").matcher(diagnostics);
            System.out.println(dm.find() ? "diagnostic: Type mismatch: cannot convert from String to int" : "no-diagnostic");
            request(id++, "shutdown", "null");
            notify("exit", "null");
        } finally {
            if (!p.waitFor(150, TimeUnit.SECONDS)) {
                killed = true;
                p.destroyForcibly();
            }
        }
        // jdtls exits with 0 or 1 after "exit" depending on how far its shutdown
        // got; what matters is that it ends on its own.
        if (!killed) {
            System.out.println("stopped");
        } else {
            System.out.println("exit " + p.exitValue());
            List<String> lines = Files.readAllLines(err);
            for (String l : lines.subList(Math.max(0, lines.size() - 12), lines.size())) System.out.println("stderr: " + l);
        }
    }
}
