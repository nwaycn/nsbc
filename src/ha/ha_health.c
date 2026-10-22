/* HA 健康评分——粗骨架（REQ-HA-002，P0 健康门；BFD/VIP 为 P1）
 *
 * 判定规则（粗骨架固定阈值，配置覆盖见 ha_hc_cfg_t）：
 *  任一 fail 阈值命中 → UNHEALTHY；任一 warn 命中 → DEGRADED；
 *  对端存活用连续探测计数（默认 3 连失败不可见，1 次成功即恢复，
 *  正式 BFD 用 detect-multiplier×interval 口径）。
 * 防脑裂：TAKE_OVER 只在本机 HEALTHY（非 DEGRADED）时给出。
 */
#include "ha_health.h"

#define HC_PEER_FAIL_NEED 3

struct ha_hc {
    ha_hc_cfg_t cfg;
    ha_hc_state_t local;
    ha_hc_sample_t last;
    int peer_fail_run;
    int peer_ok_run;
    int peer_visible;
    int peer_rtt_ms;
};

static void apply_defaults(ha_hc_cfg_t *c)
{
    if (c->evlag_warn_ms <= 0) c->evlag_warn_ms = 200;
    if (c->evlag_fail_ms <= 0) c->evlag_fail_ms = 1000;
    if (c->cpu_warn_pct <= 0) c->cpu_warn_pct = 80;
    if (c->cpu_fail_pct <= 0) c->cpu_fail_pct = 95;
    if (c->repl_lag_warn == 0) c->repl_lag_warn = 10000;
}

int ha_hc_create(const ha_hc_cfg_t *cfg, ha_hc_t **out)
{
    ha_hc_t *h;
    if (out == NULL)
        return NSBC_INVAL;
    h = (ha_hc_t *)calloc(1, sizeof(*h));
    if (h == NULL)
        return NSBC_NOMEM;
    if (cfg != NULL)
        h->cfg = *cfg;
    apply_defaults(&h->cfg);
    h->peer_visible = 1; /* 初始假设对端可见，靠探测纠偏防误升主 */
    h->peer_rtt_ms = -1;
    *out = h;
    return NSBC_OK;
}

void ha_hc_destroy(ha_hc_t *h)
{
    free(h);
}

ha_hc_state_t ha_hc_sample_local(ha_hc_t *h, const ha_hc_sample_t *s)
{
    int warn = 0;
    int fail = 0;
    if (h == NULL || s == NULL)
        return HA_HC_UNHEALTHY;
    h->last = *s;
    if (s->cpu_pct >= h->cfg.cpu_fail_pct)
        fail = 1;
    else if (s->cpu_pct >= h->cfg.cpu_warn_pct)
        warn = 1;
    if (s->evloop_lag_ms >= h->cfg.evlag_fail_ms)
        fail = 1;
    else if (s->evloop_lag_ms >= h->cfg.evlag_warn_ms)
        warn = 1;
    if (h->cfg.rss_warn_mb > 0 && s->rss_mb >= h->cfg.rss_warn_mb)
        warn = 1;
    if (s->overload_reject_rate_pct >= 50)
        fail = 1;
    else if (s->overload_reject_rate_pct >= 5)
        warn = 1;
    if (s->repl_lag > h->cfg.repl_lag_warn)
        warn = 1;
    h->local = fail ? HA_HC_UNHEALTHY
                    : warn ? HA_HC_DEGRADED : HA_HC_HEALTHY;
    return h->local;
}

ha_hc_state_t ha_hc_local(ha_hc_t *h)
{
    return h == NULL ? HA_HC_UNHEALTHY : h->local;
}

void ha_hc_peer_probe(ha_hc_t *h, int ok, int rtt_ms)
{
    if (h == NULL)
        return;
    if (ok) {
        h->peer_ok_run++;
        h->peer_fail_run = 0;
        h->peer_visible = 1;
        h->peer_rtt_ms = rtt_ms;
    } else {
        h->peer_fail_run++;
        h->peer_ok_run = 0;
        if (h->peer_fail_run >= HC_PEER_FAIL_NEED) {
            h->peer_visible = 0;
            h->peer_rtt_ms = -1;
        }
    }
}

int ha_hc_peer_visible(ha_hc_t *h)
{
    return h == NULL ? 1 : h->peer_visible;
}

int ha_hc_peer_rtt_ms(ha_hc_t *h)
{
    return h == NULL ? -1 : h->peer_rtt_ms;
}

ha_hc_action_t ha_hc_recommend(ha_hc_t *h, int i_am_active)
{
    if (h == NULL)
        return HA_HC_HOLD;
    if (h->local == HA_HC_UNHEALTHY)
        return HA_HC_RELEASE_VIP;
    if (!i_am_active && !h->peer_visible &&
        h->local == HA_HC_HEALTHY)
        return HA_HC_TAKE_OVER;
    return HA_HC_HOLD;
}

const char *ha_hc_state_name(ha_hc_state_t st)
{
    switch (st) {
    case HA_HC_HEALTHY:   return "healthy";
    case HA_HC_DEGRADED:  return "degraded";
    case HA_HC_UNHEALTHY: return "unhealthy";
    }
    return "unknown";
}
