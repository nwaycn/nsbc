/*
 * NSBC Unified Client Library (Java 8, pure stdlib).
 *
 * Architecture: THIS FILE is the library — no System.out, no argparse, no sys.exit().
 * Callers (call-center integrations, own CLIs) import nsbc.NsbcClient and reuse.
 * A companion EventClient.java provides a thin CLI shell.
 *
 * Design choice: HTTP/SSE only (no Unix Domain Socket).
 *   - nsbc-web is the official external integration surface — HTTPS + Bearer token.
 *   - UDS is nsbc-web internal → nsbcd core path (nsbc-web itself lives on /var/run/nsbc/*.sock).
 *   - Java 8 has no stdlib Unix Socket API (Java 16+ added UnixDomainSocketAddress).
 *   - Remote call-center integrations cannot reach UDS anyway (different host / socket file perms).
 *   - Result: Java SDK targets the correct production interface, not a local-debug convenience.
 *
 * Zero third-party deps. Compiles with javac 8+:
 *   javac -d out src/nsbc/*.java
 *
 * Public API mirrors Python/Go/Rust:
 *   Frame f = parseFrame(line)              // not needed for SSE path
 *   String url = buildCallUrl(new OriginateOpts(...))
 *   OriginateResult r = originate(url, "web", apiBase, apiToken)
 *   NsbcClient sse = new NsbcClient(apiBase + "/calls/stream", 30, null)
 *   sse.runForeach(frame -> { print(frame); ... })
 *   CallCorrelator cc = new CallCorrelator(timelineCallback)
 *   WebhookSink  wh = new WebhookSink("http://cc.example.com/nsbc", 5, 2)
 *   setSslVerify(false)                     // for self-signed nsbc-web certs
 */
package nsbc;

import java.io.*;
import java.net.*;
import java.security.cert.X509Certificate;
import java.util.*;
import java.util.concurrent.atomic.AtomicBoolean;
import javax.net.ssl.*;

// ===========================================================================
// Constants
// ===========================================================================
public class NsbcClient {
    public static final String DEFAULT_API_BASE =
        System.getenv().getOrDefault("NSBC_API_BASE", "https://127.0.0.1:443");
    public static final String DEFAULT_SSE_URL =
        DEFAULT_API_BASE + "/api/v1/calls/stream";
    public static final String DEFAULT_AUDIO_CODECS = "PCMU,PCMA,G722,OPUS";
    public static final String DEFAULT_VIDEO_CODECS = "H264,VP8";

    /** Per-process TLS verify toggle — flip before connect. */
    private static final AtomicBoolean SSL_VERIFY = new AtomicBoolean(true);

    public static void setSslVerify(boolean verify) { SSL_VERIFY.set(verify); }
    public static boolean isSslVerify() { return SSL_VERIFY.get(); }

    /** Build call_url per REQ-ORIGINATE-004 — same codec defaults as Python/Go/Rust. */
    public static String buildCallUrl(OriginateOpts opts) {
        StringBuilder sb = new StringBuilder();
        sb.append('{');
        sb.append("audio_codecs=").append(opts.audioCodecs == null ? DEFAULT_AUDIO_CODECS : opts.audioCodecs);
        sb.append(",video_codecs=").append(opts.videoCodecs == null ? DEFAULT_VIDEO_CODECS : opts.videoCodecs);
        sb.append("}");
        sb.append("user/").append(opts.origTo == null ? "7gja30rf" : opts.origTo);
        if (opts.app != null) sb.append('/').append(opts.app);
        sb.append("&echo");
        return sb.toString();
    }

    // =========================================================================
    // Frame: the canonical parsed event/CDR record (same shape across languages)
    // =========================================================================
    public static class Frame {
        public String ts;       // "2026-10-26T15:30:01.123"
        public String domain;   // CALL_INCOMING / CALL_ANSWERED / CALL_END / CDR ...
        public String kind;     // call / cdr / register / cmd / other
        public Map<String, Object> fields = new HashMap<>();
        public String raw;
        public String source;   // "sse" | "http" | "uds"
    }

    // =========================================================================
    // Originate: dual-path entry (Java only has HTTP path, but keep signature)
    // =========================================================================
    public static class OriginateOpts {
        public String origTo;
        public String app = "echo";
        public String audioCodecs;
        public String videoCodecs;
        public Integer timeout;
        public String callerId;
        public String callerName;
        public String callUrl;
    }

    public static class OriginateResult {
        public boolean ok;
        public Map<String, Object> payload;   // parsed JSON on success, error map on failure
        public String via;                    // "web"
    }

    /** Originate via HTTP (the only path in Java). */
    public static OriginateResult originate(String callUrl, String via,
                                            String apiBase, String apiToken) {
        return originateViaWeb(apiBase, callUrl, 10, apiToken);
    }

    public static OriginateResult originateViaWeb(String apiBase, String callUrl,
                                                   int timeoutSec, String apiToken) {
        OriginateResult r = new OriginateResult();
        r.via = "web";
        String url = apiBase == null ? DEFAULT_API_BASE : apiBase;
        url = url.replaceAll("/+$", "") + "/ext/v1/originate";

        String body = JsonUtil.object("call_url", callUrl);
        try {
            HttpURLConnection conn = openHttps(new URL(url), timeoutSec);
            conn.setRequestMethod("POST");
            conn.setRequestProperty("Content-Type", "application/json");
            conn.setRequestProperty("Accept", "application/json");
            if (apiToken != null && !apiToken.isEmpty()) {
                conn.setRequestProperty("Authorization", "Bearer " + apiToken);
            }
            conn.setDoOutput(true);
            try (OutputStream os = conn.getOutputStream()) {
                os.write(body.getBytes("UTF-8"));
            }
            int code = conn.getResponseCode();
            String text;
            try (BufferedReader br = new BufferedReader(new InputStreamReader(
                    code < 400 ? conn.getInputStream() : conn.getErrorStream(), "UTF-8"))) {
                StringBuilder sb = new StringBuilder();
                String ln;
                while ((ln = br.readLine()) != null) sb.append(ln).append('\n');
                text = sb.toString().trim();
            }
            if (code < 400) {
                Map<String, Object> parsed = JsonUtil.parseObject(text);
                if (parsed != null) { r.ok = true; r.payload = parsed; }
                else { r.ok = true; r.payload = new HashMap<>(); r.payload.put("raw", text); }
            } else {
                r.ok = false;
                Map<String, Object> err = new HashMap<>();
                err.put("http_error", code);
                err.put("detail", text);
                r.payload = err;
            }
        } catch (Exception e) {
            r.ok = false;
            Map<String, Object> err = new HashMap<>();
            err.put("network_error", e.getMessage());
            r.payload = err;
        }
        return r;
    }

    // =========================================================================
    // SSE: consume nsbc-web /calls/stream
    // =========================================================================
    public static class SseEvent {
        public String event = "message";
        public String data = "";
        public Integer retry; // null = not set by server
    }

    /**
     * Connect once, iterate forever, auto-reconnect on disconnect.
     * <pre>
     *   NsbcClient sse = new NsbcClient("https://remote/api/v1/calls/stream",
     *                                    30, Map.of("Authorization", "Bearer xxx"));
     *   sse.runForeach(frame -> { System.out.println(frame.ts + " " + frame.domain); });
     * </pre>
     */
    public static class SseClient {
        private final String url;
        private final int timeoutSec;
        private final Map<String, String> headers;

        public SseClient(String url, int timeoutSec, Map<String, String> extraHeaders) {
            this.url = url;
            this.timeoutSec = timeoutSec;
            this.headers = extraHeaders == null ? new HashMap<>() : new HashMap<>(extraHeaders);
        }

        /** Run forever — reconnects on any error. Does not return. */
        public void runForeach(java.util.function.Consumer<Frame> onFrame) {
            while (true) {
                try {
                    runOnce(onFrame);
                } catch (Exception e) {
                    System.err.println("[sse] stream broken: " + e.getMessage() + " — reconnecting in 3s");
                }
                sleepSec(3);
            }
        }

        private void runOnce(java.util.function.Consumer<Frame> onFrame) throws Exception {
            HttpURLConnection conn = openHttps(new URL(url), timeoutSec * 10);
            conn.setRequestMethod("GET");
            conn.setRequestProperty("Accept", "text/event-stream");
            conn.setRequestProperty("Cache-Control", "no-cache");
            for (Map.Entry<String, String> h : headers.entrySet()) {
                conn.setRequestProperty(h.getKey(), h.getValue());
            }
            try {
                int code = conn.getResponseCode();
                if (code >= 400) {
                    String body = readAll(conn.getErrorStream());
                    throw new IOException("HTTP " + code + " " + body);
                }
                try (BufferedReader reader = new BufferedReader(
                        new InputStreamReader(conn.getInputStream(), "UTF-8"))) {
                    readSseStream(reader, onFrame);
                }
            } finally {
                conn.disconnect();
            }
        }

        private void readSseStream(BufferedReader reader,
                                    java.util.function.Consumer<Frame> onFrame) throws Exception {
            String eventName = "message";
            List<String> dataLines = new ArrayList<>();
            Integer retryMs = null;
            String line;

            while ((line = reader.readLine()) != null) {
                if (line.isEmpty()) {
                    if (!dataLines.isEmpty() || !"message".equals(eventName)) {
                        SseEvent ev = new SseEvent();
                        ev.event = eventName;
                        ev.data = String.join("\n", dataLines);
                        ev.retry = retryMs;
                        Frame f = sseEventToFrame(ev);
                        if (f != null) onFrame.accept(f);
                    }
                    eventName = "message";
                    dataLines.clear();
                    retryMs = null;
                    continue;
                }
                if (line.startsWith(":")) continue;
                if (line.startsWith("event:")) {
                    String v = line.substring(6).trim();
                    if (!v.isEmpty()) eventName = v;
                } else if (line.startsWith("data:")) {
                    String v = line.substring(5);
                    if (v.startsWith(" ")) v = v.substring(1);
                    dataLines.add(v);
                } else if (line.startsWith("retry:")) {
                    try { retryMs = Integer.parseInt(line.substring(6).trim()); } catch (NumberFormatException ignored) {}
                }
            }
            // EOF → caller will reconnect
        }
    }

    /** Convert raw SSE event → canonical Frame (source="sse"). Returns null if JSON parse fails. */
    public static Frame sseEventToFrame(SseEvent ev) {
        Map<String, Object> blob = JsonUtil.parseObject(ev.data);
        if (blob == null) return null;
        String kindOuter = stringField(blob, "kind");
        Map<String, Object> payload = objField(blob, "payload");
        if (payload == null) payload = blob;

        Map<String, String> sseToDomain = new HashMap<>();
        sseToDomain.put("ring", "CALL_INCOMING");
        sseToDomain.put("answer", "CALL_ANSWERED");
        sseToDomain.put("hangup", "CALL_END");
        sseToDomain.put("originate", "CALL_OUTGOING");

        String sseEvt = stringField(payload, "event");
        if (sseEvt != null) sseEvt = sseEvt.toLowerCase(Locale.ROOT);

        String domain = sseToDomain.get(sseEvt);
        if (domain == null) {
            domain = sseEvt != null && !sseEvt.isEmpty() ? sseEvt.toUpperCase(Locale.ROOT)
                   : kindOuter != null && !kindOuter.isEmpty() ? kindOuter.toUpperCase(Locale.ROOT)
                   : "EVENT";
        }

        String ts = stringField(payload, "ts");
        if (ts == null) ts = stringField(payload, "timestamp");
        if (ts == null) ts = new java.text.SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss.SSS")
                               .format(new java.util.Date());

        Frame f = new Frame();
        f.ts = ts;
        f.domain = domain;
        f.kind = classifyDomain(domain);
        f.fields = payload;
        f.raw = JsonUtil.serialize(blob);
        f.source = "sse";
        return f;
    }

    private static String classifyDomain(String domain) {
        if (domain.startsWith("CALL_") || domain.equals("CALL_OUTGOING")) return "call";
        if (domain.equals("CDR")) return "cdr";
        if (domain.equals("REGISTER")) return "register";
        return "other";
    }

    // =========================================================================
    // CallCorrelator: groups frames by call_id → timeline callback on CDR
    // =========================================================================
    public static class CallTimeline {
        public String callId;
        public List<Map<String, Object>> events = new ArrayList<>();
        public Map<String, Object> cdr;
    }

    /** Groups frames by call_id; fires onComplete callback when CDR arrives. Thread-safe. */
    public static class CallCorrelator {
        private final java.util.function.Consumer<CallTimeline> onComplete;
        private final Map<String, CallTimeline> active =
            Collections.synchronizedMap(new HashMap<>());

        public CallCorrelator(java.util.function.Consumer<CallTimeline> onComplete) {
            this.onComplete = onComplete;
        }

        public void feed(Frame frame) {
            String callId = frame.fields == null ? null : stringField(frame.fields, "call_id");
            if (callId == null || callId.isEmpty()) return;
            CallTimeline tl = active.computeIfAbsent(callId, k -> {
                CallTimeline t = new CallTimeline();
                t.callId = k;
                return t;
            });
            Map<String, Object> ev = new HashMap<>();
            ev.put("domain", frame.domain);
            ev.put("ts", frame.ts);
            tl.events.add(ev);

            if ("CDR".equals(frame.domain)) {
                tl.cdr = new HashMap<>(frame.fields);
                active.remove(callId);
                if (onComplete != null) onComplete.accept(tl);
            }
        }

        /** Snapshot of active (not-yet-completed) calls. */
        public Map<String, CallTimeline> snapshot() {
            synchronized (active) { return new HashMap<>(active); }
        }
    }

    // =========================================================================
    // WebhookSink: fire-and-forget POST every frame to external URL
    // =========================================================================
    public static class WebhookSink {
        private final String url;
        private final int timeoutSec;
        private final int retries;
        private final AtomicBoolean stopped = new AtomicBoolean(false);
        private final Map<String, java.util.concurrent.atomic.AtomicInteger> stats =
            Collections.synchronizedMap(new HashMap<>());

        public WebhookSink(String url, int timeoutSec, int retries) {
            this.url = url;
            this.timeoutSec = timeoutSec;
            this.retries = retries;
            stats.put("ok", new java.util.concurrent.atomic.AtomicInteger(0));
            stats.put("fail", new java.util.concurrent.atomic.AtomicInteger(0));
        }

        public void send(Frame frame) {
            if (stopped.get()) return;
            new Thread(() -> doPost(frame), "webhook-sink").start();
        }

        private void doPost(Frame frame) {
            String body = JsonUtil.serialize(frame);
            for (int attempt = 0; attempt <= retries; attempt++) {
                try {
                    HttpURLConnection conn = openHttps(new URL(url), timeoutSec);
                    conn.setRequestMethod("POST");
                    conn.setRequestProperty("Content-Type", "application/json");
                    conn.setRequestProperty("X-NSBC-Event", frame.domain);
                    conn.setDoOutput(true);
                    try (OutputStream os = conn.getOutputStream()) {
                        os.write(body.getBytes("UTF-8"));
                    }
                    int code = conn.getResponseCode();
                    conn.disconnect();
                    if (code >= 200 && code < 300) {
                        stats.get("ok").incrementAndGet();
                        return;
                    }
                } catch (Exception ignored) {}
                if (attempt < retries) sleepMs(500L * (attempt + 1));
            }
            stats.get("fail").incrementAndGet();
        }

        public void stop() { stopped.set(true); }
        public int ok() { return stats.get("ok").get(); }
        public int fail() { return stats.get("fail").get(); }
    }

    // =========================================================================
    // HTTPS helper: respects SSL_VERIFY toggle
    // =========================================================================
    static HttpURLConnection openHttps(URL url, int timeoutSec) throws Exception {
        HttpURLConnection conn = (HttpURLConnection) url.openConnection();
        conn.setConnectTimeout(timeoutSec * 1000);
        conn.setReadTimeout(timeoutSec * 1000);
        if (conn instanceof HttpsURLConnection) {
            HttpsURLConnection hc = (HttpsURLConnection) conn;
            if (!SSL_VERIFY.get()) {
                hc.setSSLSocketFactory(allTrustSslFactory());
                hc.setHostnameVerifier((h, s) -> true);
            }
        }
        return conn;
    }

    private static SSLSocketFactory TRUST_ALL_FACTORY;
    private static SSLSocketFactory allTrustSslFactory() throws Exception {
        if (TRUST_ALL_FACTORY != null) return TRUST_ALL_FACTORY;
        TrustManager[] tm = { new X509TrustManager() {
            public X509Certificate[] getAcceptedIssuers() { return new X509Certificate[0]; }
            public void checkClientTrusted(X509Certificate[] c, String a) {}
            public void checkServerTrusted(X509Certificate[] c, String a) {}
        }};
        SSLContext ctx = SSLContext.getInstance("TLS");
        ctx.init(null, tm, null);
        TRUST_ALL_FACTORY = ctx.getSocketFactory();
        return TRUST_ALL_FACTORY;
    }

    // =========================================================================
    // Utility: field extractors from parsed JSON map
    // =========================================================================
    @SuppressWarnings("unchecked")
    static String stringField(Map<String, Object> m, String key) {
        Object v = m.get(key);
        return v instanceof String ? (String) v : v == null ? null : v.toString();
    }
    @SuppressWarnings("unchecked")
    static Map<String, Object> objField(Map<String, Object> m, String key) {
        Object v = m.get(key);
        return v instanceof Map ? (Map<String, Object>) v : null;
    }

    static void sleepSec(int s) { try { Thread.sleep(s * 1000L); } catch (InterruptedException ignored) {} }
    static void sleepMs(long ms) { try { Thread.sleep(ms); } catch (InterruptedException ignored) {} }
    static String readAll(InputStream in) throws IOException {
        if (in == null) return "";
        try (BufferedReader br = new BufferedReader(new InputStreamReader(in, "UTF-8"))) {
            StringBuilder sb = new StringBuilder();
            String ln;
            while ((ln = br.readLine()) != null) sb.append(ln).append('\n');
            return sb.toString().trim();
        }
    }

    // =========================================================================
    // Tiny JSON parser (hand-rolled, zero deps)
    // Supports: object {}, array [], string "", number, boolean, null.
    // Designed for nsbc-web payloads — stable, known structure.
    // =========================================================================
    public static final class JsonUtil {

        /** Parse a JSON object string → Map. Returns null if not a valid object. */
        @SuppressWarnings("unchecked")
        public static Map<String, Object> parseObject(String s) {
            Object v = parse(s);
            return v instanceof Map ? (Map<String, Object>) v : null;
        }

        public static Object parse(String s) {
            if (s == null) return null;
            Parser p = new Parser(s);
            p.skipWs();
            Object v = p.parseValue();
            return p.err == null ? v : null;
        }

        /** Build JSON: {"key": value, ...} — values are Strings. */
        public static String object(String... kvPairs) {
            StringBuilder sb = new StringBuilder("{");
            for (int i = 0; i + 1 < kvPairs.length; i += 2) {
                if (i > 0) sb.append(',');
                sb.append('"').append(escape(kvPairs[i])).append("\":\"").append(escape(kvPairs[i+1])).append('"');
            }
            sb.append('}');
            return sb.toString();
        }

        /** Serialize Map/List/String/Number/Boolean/null → JSON string. */
        @SuppressWarnings("unchecked")
        public static String serialize(Object v) {
            StringBuilder sb = new StringBuilder();
            writeValue(sb, v);
            return sb.toString();
        }

        private static void writeValue(StringBuilder sb, Object v) {
            if (v == null) { sb.append("null"); return; }
            if (v instanceof String) { sb.append('"').append(escape((String) v)).append('"'); return; }
            if (v instanceof Number || v instanceof Boolean) { sb.append(v); return; }
            if (v instanceof Map) {
                sb.append('{');
                boolean first = true;
                for (Map.Entry<?, ?> e : ((Map<?, ?>) v).entrySet()) {
                    if (!first) sb.append(',');
                    first = false;
                    sb.append('"').append(escape(String.valueOf(e.getKey()))).append("\":");
                    writeValue(sb, e.getValue());
                }
                sb.append('}');
                return;
            }
            if (v instanceof Iterable) {
                sb.append('[');
                boolean first = true;
                for (Object o : (Iterable<?>) v) {
                    if (!first) sb.append(',');
                    first = false;
                    writeValue(sb, o);
                }
                sb.append(']');
                return;
            }
            sb.append('"').append(escape(String.valueOf(v))).append('"');
        }

        private static String escape(String s) {
            StringBuilder sb = new StringBuilder(s.length() + 2);
            for (int i = 0; i < s.length(); i++) {
                char c = s.charAt(i);
                switch (c) {
                    case '"': sb.append("\\\""); break;
                    case '\\': sb.append("\\\\"); break;
                    case '\n': sb.append("\\n"); break;
                    case '\r': sb.append("\\r"); break;
                    case '\t': sb.append("\\t"); break;
                    case '\b': sb.append("\\b"); break;
                    case '\f': sb.append("\\f"); break;
                    default:
                        if (c < 0x20) sb.append(String.format("\\u%04x", (int) c));
                        else sb.append(c);
                }
            }
            return sb.toString();
        }

        private static final class Parser {
            final String s; int pos; String err;
            Parser(String s) { this.s = s; this.pos = 0; }

            void skipWs() { while (pos < s.length() && Character.isWhitespace(s.charAt(pos))) pos++; }

            Object parseValue() {
                skipWs();
                if (pos >= s.length()) return null;
                char c = s.charAt(pos);
                switch (c) {
                    case '{': return parseObject();
                    case '[': return parseArray();
                    case '"': return parseString();
                    case 't': case 'f': return parseBool();
                    case 'n': return parseNull();
                    default: return parseNumber();
                }
            }

            Map<String, Object> parseObject() {
                Map<String, Object> m = new LinkedHashMap<>();
                pos++; // skip '{'
                skipWs();
                if (peek() == '}') { pos++; return m; }
                while (pos < s.length()) {
                    skipWs();
                    String key = parseString();
                    if (key == null) return m;
                    skipWs();
                    if (peek() != ':') { err = "expected ':'"; return m; }
                    pos++;
                    Object val = parseValue();
                    m.put(key, val);
                    skipWs();
                    if (peek() == ',') { pos++; continue; }
                    if (peek() == '}') { pos++; return m; }
                    err = "expected ',' or '}'"; return m;
                }
                return m;
            }

            List<Object> parseArray() {
                List<Object> a = new ArrayList<>();
                pos++; // skip '['
                skipWs();
                if (peek() == ']') { pos++; return a; }
                while (pos < s.length()) {
                    Object v = parseValue();
                    a.add(v);
                    skipWs();
                    if (peek() == ',') { pos++; continue; }
                    if (peek() == ']') { pos++; return a; }
                    err = "expected ',' or ']'"; return a;
                }
                return a;
            }

            String parseString() {
                if (peek() != '"') { err = "expected '\"'"; return null; }
                pos++;
                StringBuilder sb = new StringBuilder();
                while (pos < s.length()) {
                    char c = s.charAt(pos++);
                    if (c == '"') return sb.toString();
                    if (c == '\\' && pos < s.length()) {
                        char esc = s.charAt(pos++);
                        switch (esc) {
                            case '"': sb.append('"'); break;
                            case '\\': sb.append('\\'); break;
                            case '/': sb.append('/'); break;
                            case 'n': sb.append('\n'); break;
                            case 'r': sb.append('\r'); break;
                            case 't': sb.append('\t'); break;
                            case 'b': sb.append('\b'); break;
                            case 'f': sb.append('\f'); break;
                            case 'u':
                                if (pos + 4 > s.length()) { err = "bad \\u escape"; return null; }
                                int cp = Integer.parseInt(s.substring(pos, pos + 4), 16);
                                sb.append((char) cp);
                                pos += 4; break;
                            default: sb.append(esc);
                        }
                    } else {
                        sb.append(c);
                    }
                }
                err = "unterminated string"; return null;
            }

            Object parseNumber() {
                int start = pos;
                if (peek() == '-') pos++;
                while (pos < s.length()) {
                    char c = s.charAt(pos);
                    if (Character.isDigit(c) || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') pos++;
                    else break;
                }
                String num = s.substring(start, pos);
                try {
                    if (num.contains(".") || num.contains("e") || num.contains("E")) return Double.parseDouble(num);
                    long l = Long.parseLong(num);
                    if (l >= Integer.MIN_VALUE && l <= Integer.MAX_VALUE) return (int) l;
                    return l;
                } catch (NumberFormatException e) { err = "bad number " + num; return 0; }
            }

            Object parseBool() {
                if (s.startsWith("true", pos)) { pos += 4; return true; }
                if (s.startsWith("false", pos)) { pos += 5; return false; }
                err = "expected bool"; return false;
            }

            Object parseNull() {
                if (s.startsWith("null", pos)) { pos += 4; return null; }
                err = "expected null"; return null;
            }

            char peek() { return pos < s.length() ? s.charAt(pos) : '\0'; }
        }
    }
}
