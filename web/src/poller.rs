//! CDR 拉取后台 poller（REQ-WEB-014 / REQ-OBS-006 P1）。
//!
//! 核心侧暂未把 obs_sub TCP 推送接到 Web 的 UDS 短连接客户端（需要长连
//! 生命周期重构），改为轮询式降级：每 `interval` 秒拉一次 `cdr.recent N`，
//! 写 `cdr` 表（ON CONFLICT DO NOTHING 幂等去重）。核心侧 `cdr.recent` 未
//! 实现 → CoreRejected（ERR unknown command），poller 只首报一次 warn，
//! 后续静默跳过，避免刷日志。
use std::collections::HashSet;
use std::sync::Arc;
use std::sync::atomic::Ordering;
use std::time::Duration;

use tokio::time::MissedTickBehavior;
use tracing::{debug, warn};

use crate::coreclient::{CoreApi, CoreError};
use crate::AppState;

/// 启动 poller 后台任务。`interval_s` 建议 10-30s；`limit_per_poll` 每次拉取数
/// （核心侧 cdr.recent [N] 命令上限，默认 100）。DB 未配置时静默跳过写入。
pub fn spawn<C>(
    state: Arc<AppState<C>>,
    interval_s: u64,
    limit_per_poll: u32,
) -> tokio::task::JoinHandle<()>
where
    C: CoreApi + Clone + 'static,
{
    tokio::spawn(async move {
        let mut interval = tokio::time::interval(Duration::from_secs(interval_s.max(1)));
        // 错过的 tick 合并立即触发（机器挂起恢复或 GC 暂停）。
        interval.set_missed_tick_behavior(MissedTickBehavior::Skip);
        // 首 tick 立即跑一次（不用等第一个 interval）。
        interval.tick().await;

        let mut not_implemented_warned = false;
        let mut last_unsupported_warn = false;
        let mut consecutive_failures: u32 = 0;

        loop {
            interval.tick().await;

            // C→Web data.sock 通道已建立（uds_server accept 时置 true）→ 跳过轮询
            // （write-through 实时入库比 poller 更及时；UDS 断连后不会自动回落轮询，
            // 需重启 nsbc-web；避免两套路径同时写库造成重复/乱序）。
            if state.data_pushed.load(Ordering::Relaxed) {
                continue;
            }

            match state.core.cdr_recent(limit_per_poll).await {
                Ok(cdrs) => {
                    // 成功后重置连续失败计数，下次失败会立刻报
                    consecutive_failures = 0;
                    if cdrs.is_empty() { continue; }
                    debug!(count = cdrs.len(), "cdr poller got {} entries", cdrs.len());

                    // 批次内去重（同一 poll 可能有重复 call_id）。
                    let mut seen = HashSet::with_capacity(cdrs.len());
                    for c in cdrs {
                        if !seen.insert(c.call_id.clone()) { continue; }
                        if let Err(e) = state
                            .db
                            .insert_cdr(
                                &c.call_id,
                                c.tenant.as_deref(),
                                &c.from_num,
                                &c.to_num,
                                c.trunk.as_deref(),
                                c.rec_path.as_deref(),
                                &c.dir,
                                &c.phase,
                                &c.disposition,
                                c.start_ms,
                                c.answer_ms,
                                c.end_ms,
                                c.billsec,
                                c.duration_s,
                                c.cause,
                                c.emergency,
                                None, // from_ip：poller 兜底路径拿不到，只靠 UDS 完整推送
                                None, // codec_a
                                None, // codec_b
                                None, // a_peer_ip
                                None, // b_peer_ip
                            )
                            .await
                        {
                            // 单条插入失败（唯一键冲突已过滤，真正的错误是后端写入
                            // 故障）：warn 一条，不中断批次。
                            warn!(error = %e, call_id = %c.call_id, "cdr insert failed");
                        }
                    }
                }
                Err(CoreError::Unsupported(reason)) => {
                    // 非 Unix 平台（Windows 开发机）UDS 不可用；每 60s 报一次即可。
                    if !last_unsupported_warn {
                        warn!(error = %reason, "cdr poller skipped: RealCore not supported on this platform");
                        last_unsupported_warn = true;
                    }
                }
                Err(CoreError::CoreRejected(msg)) if msg.contains("unknown") => {
                    // 核心侧 cdr.recent 命令尚未实现；只首报一次。
                    if !not_implemented_warned {
                        warn!("cdr poller skipped: core does not implement cdr.recent yet ({msg})");
                        not_implemented_warned = true;
                    }
                }
                Err(e) => {
                    // 其他错误（I/O、超时、协议、UDS 不存在）：首报立刻，后续
                    // 每 16 次（默认 interval=15s → ~4min）才 warn 一次，避免
                    // 核心未启动时刷爆日志。成功后立即重置。
                    consecutive_failures = consecutive_failures.saturating_add(1);
                    if consecutive_failures == 1 || consecutive_failures % 16 == 0 {
                        warn!(error = %e, consecutive_failures,
                              "cdr poller fetch failed (core offline or UDS missing)");
                    }
                }
            }
        }
    })
}
