---
name: nsbc-performance-improvement
description: "Use when continuing implementation of NSBC performance or concurrency recommendations, optimizing C core, SIP/RTP, Worker/Reactor, registrar, event bus, or Web CDR queries based on 审计报告/性能与并发架构改进建议-20261025.md."
argument-hint: "Optional: name a report phase, hotspot, or subsystem; otherwise select the next safe, verifiable item."
user-invocable: true
disable-model-invocation: false
---

# NSBC Performance Improvement

Implement recommendations from `审计报告/性能与并发架构改进建议-20261025.md` as small, verified changes. This skill is an implementation workflow, not authorization to enable unvalidated concurrency or to claim performance gains without benchmarks.

## Procedure

1. **Inspect the current state**
   - Read the relevant report sections and current implementations, call sites, tests, configuration, and directly related documentation.
   - Check the worktree before editing. Preserve unrelated user changes; never revert or overwrite them. If an existing change conflicts with the requested work, stop and ask.
   - Verify report assumptions against current code instead of treating the report as guaranteed current.

2. **Choose a bounded objective**
   - If the user named a phase or hotspot, work only on that scope.
   - Otherwise, select the next incomplete, independently verifiable Stage 1 item in report order: registrar, SIP transaction, SIP dialog, RTP hooks, event bus, Web CDR, then Worker. Account for work already completed and do not redo it.
   - Deliver one coherent objective per run. Do not start broad multi-Reactor migration merely because it is the report's long-term goal.
   - Before a behavior change with material compatibility, lifecycle, ordering, data migration, or operational impact, ask the user to decide when more than one reasonable approach exists.

3. **Establish correctness and performance evidence**
   - Identify the observable behavior that must remain unchanged and the exact correctness test for it.
   - Find existing tests, benchmarks, and repository build commands before editing. Add focused tests for changed behavior when practical.
   - Use measured baselines and repeatable benchmarks for performance claims. If no benchmark or suitable runtime is available, report the limitation and do not state that performance improved.

4. **Implement surgically**
   - Reuse existing data structures, helpers, configuration conventions, and naming. Keep compatibility and rollback behavior explicit.
   - Bound queues and resources; define visible backpressure/error behavior. Do not add unbounded queues or silent success fallbacks.
   - For concurrency changes, establish ownership, synchronization, shutdown/drain order, and object reclamation before enabling parallel execution.
   - Keep single-Reactor behavior as the default and a working fallback. Do not enable multiple Reactors, alter SIP state ownership, or move callbacks to Workers without end-to-end tests for timers, reload, shutdown, retransmits, CANCEL/ACK, TCP/TLS, RTP/RTCP, and cross-thread state access.
   - Update directly related documentation and configuration examples.

5. **Validate and report**
   - Run the smallest relevant test/build/lint command, then inspect the final diff and check for whitespace errors.
   - If tooling, platform headers, dependencies, or test fixtures prevent validation, record the exact command and blocker; do not imply the change is verified beyond the checks that passed.
   - Summarize changed files, behavior, tests and results, remaining risks, and the next report item. Separate proven behavior from performance hypotheses.
