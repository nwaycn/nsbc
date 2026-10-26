/*
 * NSBC Event/CDR subscription & originate client (Java CLI demo).
 *
 * Thin shell — all real API lives in NsbcClient.java.
 * This file is ONLY flag parsing + mode dispatch + pretty printing.
 *
 * Usage:
 *   javac -d out src/nsbc/*.java
 *   java -cp out nsbc.EventClient --mode subscribe --transport sse --insecure --api-token xxx
 *   java -cp out nsbc.EventClient --orig-to 7gja30rf --no-video --insecure
 */
package nsbc;

import java.util.*;
import java.util.concurrent.atomic.AtomicBoolean;

import static nsbc.NsbcClient.*;

public class EventClient {

    // ANSI colors (disabled if stdout not a tty — Java 8 has no native isatty; default to off)
    static final boolean COLOR = System.console() != null;

    static final Map<String, String> COLORS = new HashMap<>();
    static {
        COLORS.put("CALL_INCOMING", "\033[36m");
        COLORS.put("CALL_ANSWERED", "\033[32m");
        COLORS.put("CALL_END",      "\033[31m");
        COLORS.put("REGISTER",      "\033[33m");
        COLORS.put("CDR",           "\033[35m");
    }
    static final String RESET = "\033[0m";
    static final String DIM   = "\033[2m";
    static final String BOLD  = "\033[1m";

    // -----------------------------------------------------------------------
    // Pretty printers
    // -----------------------------------------------------------------------

    static String color(String domain) {
        return COLOR ? COLORS.getOrDefault(domain, "\033[37m") : "";
    }

    static void banner(String msg) {
        System.out.printf((COLOR ? "%s[nsbc] %s%s%n" : "[nsbc] %s%n"), DIM, msg, RESET);
    }

    @SuppressWarnings("unchecked")
    static void printFrame(Frame f) {
        String shortTs = f.ts != null && f.ts.length() >= 23 ? f.ts.substring(11, 23) : (f.ts == null ? "?" : f.ts);
        String cc = color(f.domain);

        if ("CDR".equals(f.domain)) {
            System.out.printf("%s %s%12s%s call_id=%s %s→%s disp=%s dur=%ss bill=%ss%n",
                shortTs, cc, "CDR", RESET,
                strField(f.fields, "call_id"),
                strField(f.fields, "from"), strField(f.fields, "to"),
                strField(f.fields, "disposition"),
                strField(f.fields, "duration_s"),
                strField(f.fields, "billsec"));
            return;
        }

        List<String> priority = Arrays.asList("call_id", "orig", "to", "src_ip", "trunk", "profile", "hangup_by");
        List<String> parts = new ArrayList<>();
        for (String k : priority) {
            Object v = f.fields.get(k);
            if (v != null) parts.add(k + "=" + v);
        }
        for (Map.Entry<String, Object> e : f.fields.entrySet()) {
            if (!priority.contains(e.getKey())) {
                parts.add(e.getKey() + "=" + e.getValue());
            }
        }
        String body = parts.isEmpty() ? f.raw : String.join(" ", parts);
        System.out.printf("%s %s%15s%s %s%n", shortTs, cc + f.domain, f.domain, RESET, body);
    }

    static String strField(Map<String, Object> m, String k) {
        Object v = m.get(k);
        return v == null ? "?" : String.valueOf(v);
    }

    static void printTimeline(NsbcClient.CallTimeline tl) {
        System.out.println();
        System.out.printf((COLOR ? "%s══════ Call Lifecycle: %s ══════%s%n" : "══════ Call Lifecycle: %s ══════%n"),
            BOLD, tl.callId, RESET);
        for (Map<String, Object> ev : tl.events) {
            String d = String.valueOf(ev.get("domain"));
            String ts = String.valueOf(ev.get("ts"));
            System.out.printf("  %s%15s%s  %s%n", color(d), d, RESET, ts);
        }
        if (tl.cdr != null) {
            System.out.printf("  %s%12s%s  CDR record%n", color("CDR"), "CDR", RESET);
            System.out.printf("    %s → %s  disp=%s  dur=%ss  bill=%ss%n",
                strField(tl.cdr, "from"), strField(tl.cdr, "to"),
                strField(tl.cdr, "disposition"), strField(tl.cdr, "duration_s"), strField(tl.cdr, "billsec"));
        }
        System.out.printf((COLOR ? "%s──────────────────────────────────%s%n" : "──────────────────────────────────%n"), BOLD, RESET);
        System.out.println();
    }

    // -----------------------------------------------------------------------
    // Flag parsing (minimal — zero deps beyond java.util)
    // -----------------------------------------------------------------------

    static class Args {
        String mode = "auto";          // auto | originate+subscribe | subscribe | originate
        String transport = "sse";      // sse (Java has no UDS)
        String apiBase = DEFAULT_API_BASE;
        String sseUrl = DEFAULT_SSE_URL;
        String origVia = "web";        // auto | web (Java: web only)
        String origTo = "1000";
        String app = "echo";
        String audioCodecs = null;
        String videoCodecs = null;
        String apiToken = null;
        String webhookUrl = null;
        boolean insecure = false;
        boolean video = true;
        boolean audio = true;
        boolean exitOnCdr = false;
    }

    static Args parseArgs(String[] argv) {
        Args a = new Args();
        for (int i = 0; i < argv.length; i++) {
            String arg = argv[i];
            String next = i + 1 < argv.length ? argv[i + 1] : null;
            switch (arg) {
                case "-h": case "--help": printHelp(); System.exit(0);
                case "--mode":         if (next != null) { a.mode = next; i++; } break;
                case "--transport":    if (next != null) { a.transport = next; i++; } break;
                case "--api-base":     if (next != null) { a.apiBase = next; i++; } break;
                case "--sse-url":      if (next != null) { a.sseUrl = next; i++; } break;
                case "--orig-via":     if (next != null) { a.origVia = next; i++; } break;
                case "--orig-to":      if (next != null) { a.origTo = next; i++; } break;
                case "--app":          if (next != null) { a.app = next; i++; } break;
                case "--audio-codecs": if (next != null) { a.audioCodecs = next; i++; } break;
                case "--video-codecs": if (next != null) { a.videoCodecs = next; i++; } break;
                case "--api-token":    if (next != null) { a.apiToken = next; i++; } break;
                case "--webhook-url":  if (next != null) { a.webhookUrl = next; i++; } break;
                case "--insecure":     a.insecure = true; break;
                case "--no-video":     a.video = false; break;
                case "--no-audio":     a.audio = false; break;
                case "--exit-on-cdr":  a.exitOnCdr = true; break;
                default:
                    // tolerate unknown flags — ignore
            }
        }
        return a;
    }

    static void printHelp() {
        System.out.println("NSBC Event Client (Java 8+, stdlib only)");
        System.out.println();
        System.out.println("Usage:");
        System.out.println("  java -cp out nsbc.EventClient --mode subscribe --transport sse --insecure");
        System.out.println("  java -cp out nsbc.EventClient --orig-to 7gja30rf --insecure --api-token xxx");
        System.out.println();
        System.out.println("Flags:");
        System.out.println("  --mode          auto | originate+subscribe | subscribe | originate (default: auto)");
        System.out.println("  --transport      sse (Java: only SSE; no UDS in Java 8 stdlib)");
        System.out.println("  --api-base       nsbc-web HTTP base (default: https://127.0.0.1:443)");
        System.out.println("  --sse-url        SSE stream URL (default: <api-base>/api/v1/calls/stream)");
        System.out.println("  --orig-via       web | auto (Java: web only)");
        System.out.println("  --orig-to        destination extension (default: 1000)");
        System.out.println("  --app            post-originate app (default: echo)");
        System.out.println("  --audio-codecs   override default audio codec list");
        System.out.println("  --video-codecs   override default video codec list");
        System.out.println("  --no-video       disable video codec in call_url");
        System.out.println("  --no-audio       disable multi-audio in call_url");
        System.out.println("  --api-token      Bearer token for HTTP originate + SSE auth");
        System.out.println("  --webhook-url    POST every frame as JSON to this URL");
        System.out.println("  --insecure       skip TLS certificate verification");
        System.out.println("  --exit-on-cdr    exit after first CDR received");
        System.out.println("  -h / --help      show this help");
    }

    // -----------------------------------------------------------------------
    // Main
    // -----------------------------------------------------------------------

    public static void main(String[] argv) {
        Args args = parseArgs(argv);
        if (args.insecure) setSslVerify(false);

        String mode = "originate+subscribe".equals(args.mode) || "auto".equals(args.mode) ? "originate+subscribe" : args.mode;

        OriginateOpts opts = new OriginateOpts();
        opts.origTo = args.origTo;
        opts.app = args.app;
        if (args.audioCodecs != null) opts.audioCodecs = args.audioCodecs;
        else opts.audioCodecs = args.audio ? DEFAULT_AUDIO_CODECS : "";
        if (args.videoCodecs != null) opts.videoCodecs = args.videoCodecs;
        else opts.videoCodecs = args.video ? DEFAULT_VIDEO_CODECS : "";

        switch (mode) {
            case "originate":
                runOriginate(opts, args);
                break;
            case "subscribe":
                runSubscribe(args);
                break;
            default: // originate+subscribe
                runOriginate(opts, args);
                runSubscribe(args);
        }
    }

    static void runOriginate(OriginateOpts opts, Args args) {
        String callUrl = buildCallUrl(opts);
        banner("[" + "originate via=web  call_url=" + callUrl + "]");
        OriginateResult r = originate(callUrl, args.origVia, args.apiBase, args.apiToken);
        if (r.ok) {
            System.out.printf("[OK] via=%s → %s%n", r.via, JsonUtil.serialize(r.payload));
        } else {
            System.err.printf("[FAIL] via=%s %s%n", r.via, JsonUtil.serialize(r.payload));
            System.exit(3);
        }
    }

    static void runSubscribe(Args args) {
        banner("[transport=SSE, url=" + args.sseUrl + " — Ctrl-C to quit]");

        AtomicBoolean running = new AtomicBoolean(true);
        CallCorrelator correlator = new CallCorrelator(EventClient::printTimeline);
        WebhookSink webhook = null;
        if (args.webhookUrl != null && !args.webhookUrl.isEmpty()) {
            webhook = new WebhookSink(args.webhookUrl, 5, 2);
            banner("[webhook] every event → POST " + args.webhookUrl);
        }

        Map<String, String> headers = new HashMap<>();
        if (args.apiToken != null && !args.apiToken.isEmpty()) {
            headers.put("Authorization", "Bearer " + args.apiToken);
        }
        SseClient sse = new SseClient(args.sseUrl, 30, headers);

        Runtime.getRuntime().addShutdownHook(new Thread(() -> {
            System.err.println("\n[quit]");
            if (webhook != null) {
                webhook.stop();
                banner("[webhook stats] ok=" + webhook.ok() + " fail=" + webhook.fail());
            }
            running.set(false);
        }));

        // runForeach never returns (reconnect loop). ShutdownHook handles Ctrl-C.
        NsbcClient.SseClient finalSse = sse;
        CallCorrelator finalCorrelator = correlator;
        WebhookSink finalWebhook = webhook;
        boolean finalExitOnCdr = args.exitOnCdr;
        // We can't actually run forever without UDS; just run and let JVM handle shutdown
        finalSse.runForeach(frame -> {
            printFrame(frame);
            finalCorrelator.feed(frame);
            if (finalWebhook != null) finalWebhook.send(frame);
            if (finalExitOnCdr && "CDR".equals(frame.domain)) {
                System.out.println();
                System.exit(0);
            }
        });
    }
}
