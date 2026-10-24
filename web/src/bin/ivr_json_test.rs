// ivr_json_test.rs — Rust 精确复现 C 侧 jarray_at/jobj_at/jstr/jaction
// 运行: cargo run --bin ivr_json_test --manifest-path web/Cargo.toml

fn jstr<'a>(json: &'a str, key: &str) -> Option<&'a str> {
    let kp = format!("\"{}\"", key);
    let bytes = json.as_bytes();
    let kbytes = kp.as_bytes();
    let mut i = 0;
    while i < bytes.len() {
        while i < bytes.len() && bytes[i] != b'"' { i += 1; }
        if i >= bytes.len() { return None; }
        if i + kbytes.len() <= bytes.len()
            && &bytes[i..i + kbytes.len()] == kbytes
        {
            let mut j = i + kbytes.len() + 1;
            while j < bytes.len() && bytes[j] != b':' { j += 1; }
            if j >= bytes.len() { return None; }
            j += 1;
            while j < bytes.len() && bytes[j] == b' ' { j += 1; }
            if j >= bytes.len() || bytes[j] != b'"' { return None; }
            j += 1;
            let start = j;
            while j < bytes.len() {
                if bytes[j] == b'\\' { j += 2; continue; }
                if bytes[j] == b'"' { return Some(&json[start..j]); }
                j += 1;
            }
            return None;
        }
        i += 1;
    }
    None
}

fn jint(json: &str, key: &str, dflt: i64) -> i64 {
    match jstr(json, key) {
        Some(v) => v.parse::<i64>().unwrap_or(dflt),
        None => dflt,
    }
}

fn jarray_at<'a>(json: &'a str, idx: usize) -> Option<&'a str> {
    let bytes = json.as_bytes();
    let mut p = 0usize;
    while p < bytes.len() && bytes[p] != b'[' { p += 1; }
    if p >= bytes.len() { return None; }

    let mut arr_depth: i32 = 0;
    let mut obj_depth: i32 = 0;
    let mut in_str = false;
    let mut cur: usize = 0;
    let mut elem_start: Option<usize> = None;
    let mut q = p;

    while q < bytes.len() {
        let c = bytes[q];
        if c == b'\\' && in_str { q += 2; continue; }
        if c == b'"' { in_str = !in_str; q += 1; continue; }
        if in_str { q += 1; continue; }

        match c {
            b'[' => {
                arr_depth += 1;
                if arr_depth == 1 && elem_start.is_none() {
                    elem_start = Some(q + 1);
                    cur = 0;
                }
            }
            b']' => {
                if arr_depth == 1 {
                    return match elem_start {
                        Some(s) if cur == idx => Some(&json[s..q]),
                        _ => None,
                    };
                }
                arr_depth -= 1;
            }
            b'{' => { obj_depth += 1; }
            b'}' => { obj_depth -= 1; }
            b',' if arr_depth == 1 && obj_depth == 0 => {
                if cur == idx {
                    return match elem_start {
                        Some(s) => Some(&json[s..q]),
                        None => None,
                    };
                }
                cur += 1;
                let mut s = q + 1;
                while s < bytes.len() && bytes[s] == b' ' { s += 1; }
                elem_start = Some(s);
            }
            _ => {}
        }
        q += 1;
    }
    None
}

fn jobj_at<'a>(json: &'a str, key: &str) -> Option<&'a str> {
    let kp = format!("\"{}\"", key);
    let bytes = json.as_bytes();
    let kbytes = kp.as_bytes();
    let mut i = 0;
    while i < bytes.len() {
        while i < bytes.len() && bytes[i] != b'"' { i += 1; }
        if i >= bytes.len() { return None; }
        if i + kbytes.len() <= bytes.len()
            && &bytes[i..i + kbytes.len()] == kbytes
        {
            let mut ci = i + kbytes.len() + 1;
            while ci < bytes.len() && bytes[ci] != b':' { ci += 1; }
            if ci >= bytes.len() { return None; }
            let mut bi = ci + 1;
            while bi < bytes.len() && bytes[bi] != b'{' { bi += 1; }
            if bi >= bytes.len() { return None; }
            let mut depth: i32 = 0;
            let mut in_str = false;
            let mut q = bi;
            while q < bytes.len() {
                let c = bytes[q];
                if c == b'\\' && in_str { q += 2; continue; }
                if c == b'"' { in_str = !in_str; q += 1; continue; }
                if in_str { q += 1; continue; }
                if c == b'{' { depth += 1; }
                else if c == b'}' {
                    depth -= 1;
                    if depth == 0 { return Some(&json[bi..=q]); }
                }
                q += 1;
            }
            return None;
        }
        i += 1;
    }
    None
}

fn collect_nodes(json: &str) -> Vec<&str> {
    let mut v = Vec::new();
    for i in 0..100 {
        match jarray_at(json, i) {
            Some(e) => v.push(e),
            None => break,
        }
    }
    v
}

fn main() {
    let nodes_json = r##"[
  {"name":"main","prompt":"welcome.wav","initial_timeout_s":5,
"interdigit_timeout_s":3,"max_retries":3,
"choices":[
{"key":"1","act":"submenu","arg":"sales"},
{"key":"2","act":"transfer","arg":"1000"}
],
"timeout_action":{"act":"hangup","arg":""},
"invalid_action":{"act":"play","arg":"invalid.wav"}}
  ,
  {"name":"sales","prompt":"sales.wav","initial_timeout_s":6,
"interdigit_timeout_s":4,"max_retries":2,
"choices":[
{"key":"1","act":"transfer","arg":"2000"},
{"key":"0","act":"submenu","arg":"main"}
],
"timeout_action":{"act":"hangup","arg":""},
"invalid_action":{"act":"hangup","arg":""}}
]"##;

    println!("=== TEST 1: 顶级节点数组 ===");
    let nodes = collect_nodes(nodes_json);
    for (i, n) in nodes.iter().enumerate() {
        println!("idx={} name={} len={}", i, jstr(n, "name").unwrap_or("?"), n.len());
    }
    assert_eq!(nodes.len(), 2);
    assert_eq!(jstr(nodes[0], "name"), Some("main"));
    assert_eq!(jstr(nodes[1], "name"), Some("sales"));
    assert!(jarray_at(nodes_json, 2).is_none());
    println!("✅ 2 节点正确切分");

    println!("\n=== TEST 2: main 节点 choices ===");
    let choices = collect_nodes(nodes[0]);
    for (i, c) in choices.iter().enumerate() {
        let key = jstr(c, "key").unwrap_or("?");
        let act = jstr(c, "act").unwrap_or("?");
        let arg = jstr(c, "arg").unwrap_or("");
        println!("  choice[{}]: key={} act={} arg={}", i, key, act, arg);
    }
    assert_eq!(choices.len(), 2);
    assert_eq!(jstr(choices[0], "key"), Some("1"));
    assert_eq!(jstr(choices[0], "act"), Some("submenu"));
    assert_eq!(jstr(choices[0], "arg"), Some("sales"));
    assert_eq!(jstr(choices[1], "key"), Some("2"));
    assert_eq!(jstr(choices[1], "act"), Some("transfer"));
    assert_eq!(jstr(choices[1], "arg"), Some("1000"));
    println!("✅ main 2 choice 正确");

    println!("\n=== TEST 3: jobj_at 嵌套对象 ===");
    let ta = jobj_at(nodes[0], "timeout_action").expect("ta");
    assert_eq!(jstr(ta, "act"), Some("hangup"));
    assert_eq!(jstr(ta, "arg"), Some(""));
    let ia = jobj_at(nodes[0], "invalid_action").expect("ia");
    assert_eq!(jstr(ia, "act"), Some("play"));
    assert_eq!(jstr(ia, "arg"), Some("invalid.wav"));
    println!("timeout_action={:?}", ta);
    println!("invalid_action={:?}", ia);
    println!("✅ 嵌套对象正确");

    println!("\n=== TEST 4: jint ===");
    assert_eq!(jint(nodes[0], "initial_timeout_s", 0), 5);
    assert_eq!(jint(nodes[0], "interdigit_timeout_s", 0), 3);
    assert_eq!(jint(nodes[0], "max_retries", 0), 3);
    println!("✅ jint 正确");

    println!("\n=== TEST 5: sales 完整 ===");
    let sales = nodes[1];
    assert_eq!(jstr(sales, "name"), Some("sales"));
    assert_eq!(jint(sales, "initial_timeout_s", 0), 6);
    assert_eq!(jint(sales, "interdigit_timeout_s", 0), 4);
    assert_eq!(jint(sales, "max_retries", 0), 2);
    let scs = collect_nodes(sales);
    assert_eq!(scs.len(), 2);
    assert_eq!(jstr(scs[0], "key"), Some("1"));
    assert_eq!(jstr(scs[0], "act"), Some("transfer"));
    assert_eq!(jstr(scs[0], "arg"), Some("2000"));
    assert_eq!(jstr(scs[1], "key"), Some("0"));
    assert_eq!(jstr(scs[1], "act"), Some("submenu"));
    assert_eq!(jstr(scs[1], "arg"), Some("main"));
    println!("✅ sales 完整正确");

    println!("\n=== TEST 6: 空 choices 数组 ===");
    let single = r##"[{"name":"x","prompt":"p.wav","initial_timeout_s":5,"choices":[],"timeout_action":{"act":"hangup","arg":""}}]"##;
    let sn = collect_nodes(single);
    assert_eq!(sn.len(), 1);
    assert_eq!(jstr(sn[0], "name"), Some("x"));
    assert!(jarray_at(single, 1).is_none());
    assert!(collect_nodes(sn[0]).is_empty());
    println!("✅ 空数组/越界正确返回 None");

    println!("\n=== TEST 7: 字符串逗号不误切 ===");
    let weird = r##"[{"name":"a,b","prompt":"hello, world.wav","choices":[{"key":"1","act":"play","arg":"file,with,commas"}]}]"##;
    let wn = collect_nodes(weird);
    assert_eq!(wn.len(), 1);
    assert_eq!(jstr(wn[0], "name"), Some("a,b"));
    assert_eq!(jstr(wn[0], "prompt"), Some("hello, world.wav"));
    let wc = collect_nodes(wn[0]);
    assert_eq!(wc.len(), 1);
    assert_eq!(jstr(wc[0], "arg"), Some("file,with,commas"));
    println!("✅ 字符串内逗号被正确跳过");

    println!("\n=== TEST 8: 嵌套数组逗号不影响顶级切分 ===");
    let three = r##"[
  {"name":"n1","choices":[{"key":"1","act":"transfer","arg":"a"},{"key":"2","act":"play","arg":"b"},{"key":"3","act":"hangup","arg":""}]},
  {"name":"n2","choices":[{"key":"*","act":"submenu","arg":"n1"},{"key":"#","act":"hangup","arg":""}]}
]"##;
    let ns = collect_nodes(three);
    assert_eq!(ns.len(), 2);
    assert_eq!(jstr(ns[0], "name"), Some("n1"));
    assert_eq!(jstr(ns[1], "name"), Some("n2"));
    let n1cs = collect_nodes(ns[0]);
    assert_eq!(n1cs.len(), 3);
    assert_eq!(jstr(n1cs[0], "key"), Some("1"));
    assert_eq!(jstr(n1cs[2], "key"), Some("3"));
    let n2cs = collect_nodes(ns[1]);
    assert_eq!(n2cs.len(), 2);
    assert_eq!(jstr(n2cs[0], "key"), Some("*"));
    assert_eq!(jstr(n2cs[1], "key"), Some("#"));
    println!("✅ 嵌套数组逗号不影响顶级，内部 choice 也正确");

    println!("\n=== TEST 9: 前端 default nodes_json ===");
    let default = r##"[{"name":"main","prompt":"welcome.wav","initial_timeout_s":5,"interdigit_timeout_s":3,"max_retries":3,"choices":[{"key":"1","act":"submenu","arg":"sales"},{"key":"2","act":"transfer","arg":"1000"},{"key":"*","act":"hangup","arg":""}],"timeout_action":{"act":"hangup","arg":""},"invalid_action":{"act":"play","arg":"invalid.wav"}}]"##;
    let dn = collect_nodes(default);
    assert_eq!(dn.len(), 1);
    assert_eq!(jstr(dn[0], "name"), Some("main"));
    let dta = jobj_at(dn[0], "timeout_action").unwrap();
    assert_eq!(jstr(dta, "act"), Some("hangup"));
    assert_eq!(jstr(dta, "arg"), Some(""));
    let dia = jobj_at(dn[0], "invalid_action").unwrap();
    assert_eq!(jstr(dia, "act"), Some("play"));
    assert_eq!(jstr(dia, "arg"), Some("invalid.wav"));
    let dcs = collect_nodes(dn[0]);
    assert_eq!(dcs.len(), 3);
    assert_eq!(jstr(dcs[0], "key"), Some("1"));
    assert_eq!(jstr(dcs[0], "act"), Some("submenu"));
    assert_eq!(jstr(dcs[0], "arg"), Some("sales"));
    assert_eq!(jstr(dcs[1], "key"), Some("2"));
    assert_eq!(jstr(dcs[1], "act"), Some("transfer"));
    assert_eq!(jstr(dcs[1], "arg"), Some("1000"));
    assert_eq!(jstr(dcs[2], "key"), Some("*"));
    assert_eq!(jstr(dcs[2], "act"), Some("hangup"));
    println!("✅ 前端 default nodes_json 完整正确");

    println!("\n{}", "=".repeat(50));
    println!("🎉 ALL 9 TESTS PASSED — C 侧 jarray_at/jobj_at 逻辑正确");
    println!("{}", "=".repeat(50));
}
