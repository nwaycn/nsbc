# NSBC

**The all-in-one SIP powerhouse: SBC security, PBX features, and proxy performance in a single binary.**

## 📖 Overview

**NSBC** is a next-generation, unified SIP application engineered entirely from the ground up. Unlike traditional solutions that stitch together disparate components, NSBC features a **hand-crafted SIP protocol stack** designed to deliver three critical functions in one seamless platform:

- 🛡️ **Enterprise SBC Security:** Advanced topology hiding, DoS protection, and media anchoring.
- 📞 **Full-Featured PBX:** Modern call routing, voicemail, IVR, and endpoint management.
- ⚡ **High-Performance SIP Proxy:** Bare-metal speed with minimal latency and massive concurrency.

Built for engineers who demand absolute control over their SIP signaling and media paths. No legacy bloat. No third-party SIP stack dependencies. Just pure, optimized SIP.

## ✨ Key Features

- 🔧 **Custom-Built SIP Stack:** Not based on PJSIP, Sofia-SIP, or any existing library. Every parser, state machine, and transaction layer is written from scratch for maximum efficiency and auditability.
- 🏗️ **True Unified Architecture:** Eliminates the need to deploy and maintain separate SBC, PBX, and Proxy servers. One codebase, one configuration, one operational overhead.
- 🚀 **Bare-Metal Performance:** Optimized memory layout and zero-copy parsing where possible. Designed to handle high CPS (Calls Per Second) with predictable low latency.
- 🔒 **Security by Design:** SBC-grade threat mitigation baked into the core protocol handler, not bolted on as an afterthought.
- 📦 **Single Binary Deployment:** No complex dependency chains. Drop it on your server and run.
- 🌐 **NAT Traversal Built-In:** Integrated STUN/TURN/ICE handling without external helpers.
- 📊 **Real-Time Observability:** Native Prometheus metrics and structured logging out of the box.
