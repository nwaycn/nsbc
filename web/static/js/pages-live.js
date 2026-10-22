// =============================================================================
// NSBC Web — 实时监控 / 只读面板组件
// 加载顺序：必须在 app-core.js 之后
// 依赖：app-core.js 中声明的全局 const app, livePanel, API, ref, onMounted
// =============================================================================

// ---- 辅助：epoch ms → 人类可读时间 ----
function fmtTime(ms) {
  if (!ms) return '-';
  const d = new Date(ms);
  const pad = n => String(n).padStart(2, '0');
  return `${d.getFullYear()}-${pad(d.getMonth()+1)}-${pad(d.getDate())} ${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`;
}
function fmtClock(ms) {
  if (!ms) return '-';
  const d = new Date(ms);
  const pad = n => String(n).padStart(2, '0');
  return `${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`;
}
function fmtDuration(ms) {
  if (!ms || ms < 0) return '0s';
  const s = Math.floor(ms / 1000);
  if (s < 60) return `${s}s`;
  const m = Math.floor(s / 60); const sec = s % 60;
  if (m < 60) return `${m}m${sec}s`;
  const h = Math.floor(m / 60); const min = m % 60;
  return `${h}h${min}m${sec}s`;
}

// ---- 辅助：判断是否为"核心（nsbcd）未连接/启动"类错误，
// 这类错误属于系统级（C 核心没跑、UDS 不存在、502 Bad Gateway），
// 面板应静默（保持显示空/上次数据）+ console.warn，不渲染红色报错条，
// 避免核心离线时 6 个轮询组件每 5s 各自刷一条"弹错"。
function isCoreOfflineError(msg) {
  if (!msg) return false;
  const m = String(msg).toLowerCase();
  return m.includes('bad gateway') || m.includes('core') || m.includes('unavailable')
      || m.includes('timeout') || m.includes('connect') || m.includes('no such file');
}

// ===== 实时总览（综合状态） =====
app.component('realtime', {
  template: `
    <div>
      <div class="flex items-center justify-between mb-4">
        <h2 class="text-lg font-semibold"><i class="fa-solid fa-gauge-high"></i> 实时总览</h2>
        <div class="flex items-center gap-2">
          <button @click="showOrigDlg = true" class="px-3 py-1.5 text-sm bg-blue-600 hover:bg-blue-700 text-white rounded shadow-sm">
            <i class="fa-solid fa-phone-plus mr-1"></i> 外呼测试
          </button>
          <span class="text-xs text-gray-500">上次更新：{{ lastUpdated }}</span>
        </div>
      </div>
      <div v-if="loading" class="text-gray-400">加载中...</div>
      <div v-else-if="error" class="text-red-500 mb-4">⚠ {{ error }}</div>
      <div v-else class="grid grid-cols-2 md:grid-cols-4 gap-4">
        <div class="bg-blue-50 border border-blue-200 rounded-lg p-4">
          <div class="text-xs text-blue-600 font-medium">活跃通话</div>
          <div class="text-3xl font-bold text-blue-700 mt-1">{{ calls && calls.active != null ? calls.active : '-' }}</div>
        </div>
        <div class="bg-green-50 border border-green-200 rounded-lg p-4">
          <div class="text-xs text-green-600 font-medium">累计通话</div>
          <div class="text-3xl font-bold text-green-700 mt-1">{{ calls && calls.total != null ? calls.total : '-' }}</div>
        </div>
        <div class="bg-red-50 border border-red-200 rounded-lg p-4">
          <div class="text-xs text-red-600 font-medium">失败</div>
          <div class="text-3xl font-bold text-red-700 mt-1">{{ calls && calls.failed != null ? calls.failed : '-' }}</div>
        </div>
        <div class="bg-purple-50 border border-purple-200 rounded-lg p-4">
          <div class="text-xs text-purple-600 font-medium">注册数</div>
          <div class="text-3xl font-bold text-purple-700 mt-1">{{ regs && regs.registrations != null ? regs.registrations : '-' }}</div>
        </div>
      </div>

      <!-- 外呼测试弹窗 -->
      <div v-if="showOrigDlg" class="fixed inset-0 bg-black/40 flex items-center justify-center z-50" @click.self="showOrigDlg = false">
        <div class="bg-white rounded-xl shadow-2xl w-[420px] overflow-hidden">
          <div class="px-5 py-3 bg-blue-600 text-white flex items-center justify-between">
            <h3 class="font-semibold"><i class="fa-solid fa-phone-plus mr-2"></i>外呼测试</h3>
            <button @click="showOrigDlg = false" class="hover:bg-white/20 rounded px-2">✕</button>
          </div>
          <div class="p-5 space-y-4">
            <div>
              <label class="field-label">被叫号 / call-url</label>
              <input v-model="origForm.call_url" class="field-input" placeholder="如 1003 或 sip:user@trunk">
            </div>
            <div class="grid grid-cols-2 gap-3">
              <div>
                <label class="field-label">主叫号覆盖</label>
                <input v-model="origForm.caller_id" class="field-input" placeholder="如 8888">
              </div>
              <div>
                <label class="field-label">主叫显示名</label>
                <input v-model="origForm.caller_name" class="field-input" placeholder="如 Switch">
              </div>
            </div>
            <div>
              <label class="field-label">振铃超时（秒）</label>
              <input v-model="origForm.timeout" type="number" min="1" max="3600"
                     class="field-input" placeholder="如 50（不填用核心默认）">
            </div>
            <div>
              <label class="field-label">应用（可选）</label>
              <select v-model="origForm.app" class="field-input">
                <option value="park">park（保持）</option>
                <option value="echo">echo（回声）</option>
                <option value="hangup">hangup（挂断）</option>
                <option value="answer">answer（应答）</option>
              </select>
            </div>
            <div class="flex items-center gap-2 text-xs text-gray-500 bg-blue-50 border border-blue-200 rounded px-3 py-2">
              <i class="fa-solid fa-info-circle text-blue-600"></i>
              <span>发送到核心 <code class="font-mono">originate {var=val,...}&lt;dest&gt; &amp;&lt;app&gt;</code>，ESL/api 命令实时生效</span>
            </div>
            <div v-if="origMsg" class="text-sm" :class="origOk ? 'text-green-600 bg-green-50' : 'text-red-600 bg-red-50'"
                 class="rounded px-3 py-2 border" :class="origOk ? 'border-green-200' : 'border-red-200'">
              {{ origMsg }}
            </div>
          </div>
          <div class="px-5 py-3 bg-gray-50 flex justify-end gap-2">
            <button @click="showOrigDlg = false" class="btn btn-ghost">取消</button>
            <button @click="submitOriginate" :disabled="origLoading || !origForm.call_url"
                    class="btn btn-primary disabled:opacity-50">
              <i v-if="origLoading" class="fa-solid fa-spinner fa-spin mr-1"></i>
              {{ origLoading ? '发送中…' : '一键外呼' }}
            </button>
          </div>
        </div>
      </div>
    </div>
  `,
  setup() {
    const calls = ref(null), regs = ref(null), loading = ref(false), error = ref(null), lastUpdated = ref('');
    // 外呼测试弹窗状态
    const showOrigDlg = ref(false);
    const origForm = ref({ call_url: '', app: 'park', caller_id: '', caller_name: '', timeout: '' });
    const origLoading = ref(false);
    const origMsg = ref('');
    const origOk = ref(false);

    async function submitOriginate() {
      const f = origForm.value;
      const dest = (f.call_url || '').trim();
      if (!dest) { return; }
      // 组装 FreeSWITCH 风格命令：{var=val,...}dest &app
      // 核心 parse_fs_brace_vars 支持 origination_caller_id_number /
      // origination_caller_id_name / originate_timeout（秒）。
      const vars = [];
      if (f.caller_id)   vars.push('origination_caller_id_number=' + String(f.caller_id).trim());
      if (f.caller_name) vars.push('origination_caller_id_name=' + String(f.caller_name).trim());
      if (f.timeout)     vars.push('originate_timeout=' + parseInt(f.timeout, 10));
      let url = (vars.length ? '{' + vars.join(',') + '}' : '') + dest;
      // answer 不是核心支持的后置 app（核心里它会跳过），仅支持 park/echo/hangup
      if (f.app && f.app !== 'answer') url += ' &' + f.app;
      origLoading.value = true; origMsg.value = '';
      try {
        const r = await API.post('/originate', { call_url: url });
        origMsg.value = r.status || 'originate-dispatched';
        origOk.value = true;
      } catch (e) {
        origMsg.value = e.message || '外呼失败';
        origOk.value = false;
      }
      origLoading.value = false;
    }

    let timer;
    function onSseRefresh() { load(); }
    async function load() {
      loading.value = true; error.value = null;
      try {
        const [c, r] = await Promise.all([API.get('/calls'), API.get('/registrations')]);
        calls.value = c; regs.value = r;
        lastUpdated.value = new Date().toLocaleTimeString();
      } catch (e) {
        if (isCoreOfflineError(e.message)) { console.warn('[live] core offline:', e.message); }
        else error.value = e.message;
      }
      loading.value = false;
    }
    onMounted(() => { load(); timer = setInterval(load, 5000); });
    // 修复：清定时器须在卸载时（原误用 onMounted 会在建表后立即清除，5s 轮询失效）；
    // 同时挂 SSE 推送（nsbc-refresh）构成 push+pull 双路径。
    onMounted(() => document.addEventListener('nsbc-refresh', onSseRefresh));
    onBeforeUnmount(() => { clearInterval(timer); document.removeEventListener('nsbc-refresh', onSseRefresh); });
    return { calls, regs, loading, error, lastUpdated,
             showOrigDlg, origForm, origLoading, origMsg, origOk, submitOriginate };
  },
});

// MOS×1000 → "3.62"（保留 2 位）
function fmtMos(v) {
  if (v == null || v < 0) return '-';
  return (v / 1000).toFixed(2);
}
// 丢包率 %（0-100）
function fmtLoss(v) {
  if (v == null || v === 0) return '-';
  return v + '%';
}

// 状态字符串 → 带颜色徽章（stateBadge）。
// 覆盖 C 侧 LEG_* 状态名 + hold/unhold 自定义值：
//   LEG_NONE/CALLING → calling | LEG_RINGING → ringing | LEG_EARLY → early
//   LEG_CONFIRMED → confirmed | LEG_HELD → held | LEG_TERMINATING → terminated
//   LEG_TERMINATED/NULL → terminated
//   hold → held | unhold → active（通话中，语义同 confirmed）
function stateBadge(v) {
  if (!v) return '<span class="text-gray-400">-</span>';
  const s = String(v).toLowerCase();
  const map = {
    calling:    ['bg-blue-100 text-blue-700',    '💬 Calling'],
    ringing:    ['bg-yellow-100 text-yellow-700','🔔 Ringing'],
    early:      ['bg-amber-100 text-amber-700',  '📢 Early'],
    confirmed:  ['bg-green-100 text-green-700',  '✅ Confirmed'],
    active:     ['bg-green-100 text-green-700',  '✅ Active'],
    held:       ['bg-purple-100 text-purple-700','⏸ Held'],
    terminated: ['bg-red-100 text-red-700',      '❌ Terminated'],
    failed:     ['bg-red-100 text-red-700',      '❌ Failed'],
    ended:      ['bg-gray-100 text-gray-600',    '⏹ Ended'],
  };
  const [cls, label] = map[s] || ['bg-gray-100 text-gray-600', v];
  return `<span class="inline-block px-2 py-0.5 rounded text-xs font-medium ${cls}">${label}</span>`;
}

// ===== 在途呼叫 =====
app.component('channels-live', livePanel('在途呼叫', 'fa-solid fa-phone-volume',
  [
    { key: 'a_call_id',    label: 'A-Leg Call-ID' },
    { key: 'a_state',      label: 'A 状态', fmt: stateBadge },
    { key: 'b_call_id',    label: 'B-Leg Call-ID' },
    { key: 'b_state',      label: 'B 状态', fmt: stateBadge },
    { key: 'caller',       label: '主叫' },
    { key: 'callee',       label: '被叫' },
    { key: 'start_ms',     label: '开始时间', fmt: fmtTime },
    { key: 'answer_ms',    label: '接通时间', fmt: fmtClock },
    { key: 'duration_ms',  label: '持续时长', fmt: fmtDuration },
    { key: 'codec_a',      label: 'A 编码' },
    { key: 'codec_b',      label: 'B 编码' },
    { key: 'from_ip',      label: '来源 IP' },
    { key: 'a_peer_ip',    label: 'A-RTP' },
    { key: 'b_peer_ip',    label: 'B-RTP' },
    { key: 'mos_x1000',    label: 'MOS', fmt: fmtMos },
    { key: 'rtp_loss_pct', label: '丢包', fmt: fmtLoss },
    { key: 'rtp_jitter_ms',label: '抖动(ms)' },
  ], '/channels', {
    refreshMs: 5000,
    sortBy: 'start_ms', sortDir: 'desc',
    pageSize: 10,
    actionCol: (row) => {
      const callId = row.a_call_id || row.b_call_id || '';
      if (!callId) return '';
      return `<button onclick="nsbcHangup('${callId}')" class="px-2 py-1 text-xs text-red-600 border border-red-300 rounded hover:bg-red-50 hover:text-red-800"><i class="fa-solid fa-phone-slash"></i> 挂断</button>`;
    },
  }));

// ISO8601 字符串 → 人类可读时间
function fmtIso(s) {
  if (!s) return '-';
  const d = new Date(s);
  if (isNaN(d.getTime())) return '-';
  const pad = n => String(n).padStart(2, '0');
  return `${d.getFullYear()}-${pad(d.getMonth()+1)}-${pad(d.getDate())} ${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`;
}
// ISO8601 字符串 → epoch ms
function isoMs(s) {
  if (!s) return 0;
  const d = new Date(s);
  return isNaN(d.getTime()) ? 0 : d.getTime();
}

// ===== 通话记录（CDR） =====
app.component('cdrs-live', {
  template: `
    <div>
      <div class="flex items-center justify-between mb-4">
        <h2 class="text-lg font-semibold"><i class="fa-solid fa-file-lines"></i> 最近通话记录</h2>
        <div class="text-xs text-gray-500">共 {{ total }} 条 · 每 10 秒刷新</div>
      </div>
      <div v-if="loading" class="text-gray-400">加载中...</div>
      <div v-else-if="error" class="text-red-500">⚠ {{ error }}</div>
      <div v-else-if="!rows.length" class="text-center text-gray-400 py-12">暂无 CDR</div>
      <table v-else class="w-full text-sm border-collapse">
        <thead><tr class="bg-gray-50 border-b">
          <th class="px-3 py-2 text-left font-medium text-gray-600">主叫</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">被叫</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">来源 IP</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">A 编码</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">B 编码</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">Call-ID</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">呼入时间</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">应答时间</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">接续时长</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">挂机时间</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">总时长</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">计费时长</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">处置</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">录音</th>
        </tr></thead>
        <tbody>
          <tr v-for="(row, idx) in rows" :key="idx" class="border-b hover:bg-blue-50/50">
            <td class="px-3 py-2 font-mono text-xs">{{ row.caller || '-' }}</td>
            <td class="px-3 py-2 font-mono text-xs">{{ row.callee || '-' }}</td>
            <td class="px-3 py-2 font-mono text-xs text-blue-600">{{ getMeta(row, 'from_ip') || '-' }}</td>
            <td class="px-3 py-2 font-mono text-xs">{{ getMeta(row, 'codec_a') || '-' }}</td>
            <td class="px-3 py-2 font-mono text-xs">{{ getMeta(row, 'codec_b') || '-' }}</td>
            <td class="px-3 py-2 font-mono text-[10px] text-gray-400 max-w-[120px] truncate" :title="row.call_id">{{ row.call_id || '-' }}</td>
            <td class="px-3 py-2 text-xs">{{ fmtIso(row.start_time) }}</td>
            <td class="px-3 py-2 text-xs">{{ fmtIso(row.answer_time) }}</td>
            <td class="px-3 py-2 text-xs text-gray-600">{{ ringTime(row.start_time, row.answer_time) }}</td>
            <td class="px-3 py-2 text-xs">{{ fmtIso(row.end_time) }}</td>
            <td class="px-3 py-2 text-xs">{{ row.duration_sec != null ? row.duration_sec + 's' : '-' }}</td>
            <td class="px-3 py-2 text-xs font-medium text-blue-700">{{ formatBillsec(row) }}</td>
            <td class="px-3 py-2 text-xs">
              <span :class="{
                'text-green-600': row.cause_text === 'answered' || row.cause_text === 'NORMAL_CLEARING',
                'text-yellow-600': row.cause_text === 'noanswer',
                'text-red-600': ['busy','failed'].includes(row.cause_text),
              }">{{ row.cause_text || '-' }}</span>
            </td>
            <td class="px-3 py-2 text-xs">
              <template v-if="getRecUrl(row)">
                <audio :src="getRecUrl(row)" controls preload="none" class="h-6 align-middle"></audio>
                <a :href="getRecUrl(row)" download target="_blank" class="ml-2 text-blue-600 hover:underline" title="下载">
                  <i class="fa-solid fa-download"></i>
                </a>
              </template>
              <template v-else>-</template>
            </td>
          </tr>
        </tbody>
      </table>
      <!-- 分页控件 -->
      <div v-if="total > pageSize" class="flex items-center justify-between mt-3 text-xs text-gray-500">
        <div>共 {{ total }} 条 · 每页 {{ pageSize }} 条</div>
        <div class="flex items-center gap-1">
          <button @click="goPage(1)" :disabled="currentPage===1" class="px-2 py-1 border rounded disabled:opacity-30">«</button>
          <button @click="goPage(currentPage-1)" :disabled="currentPage===1" class="px-2 py-1 border rounded disabled:opacity-30">‹</button>
          <span class="px-2">{{ currentPage }} / {{ totalPages }}</span>
          <button @click="goPage(currentPage+1)" :disabled="currentPage===totalPages" class="px-2 py-1 border rounded disabled:opacity-30">›</button>
          <button @click="goPage(totalPages)" :disabled="currentPage===totalPages" class="px-2 py-1 border rounded disabled:opacity-30">»</button>
        </div>
      </div>
    </div>
  `,
  setup() {
    const rows = ref([]), total = ref(0), loading = ref(false), error = ref(null);
    const pageSize = ref(20);
    const currentPage = ref(1);
    const totalPages = computed(() => Math.max(1, Math.ceil(total.value / pageSize.value)));
    let timer;

    function goPage(p) {
      currentPage.value = p;
      load();
    }
    // 从 row.metadata JSON 里取扩展字段（from_ip / codec_a / codec_b 等）
    function getMeta(row, key) {
      if (!row.metadata) return null;
      try {
        const m = typeof row.metadata === 'string' ? JSON.parse(row.metadata) : row.metadata;
        return m[key] || null;
      } catch { return null; }
    }
    function getRecUrl(row) {
      if (!row.metadata) return null;
      let recPath = null;
      try {
        const m = typeof row.metadata === 'string' ? JSON.parse(row.metadata) : row.metadata;
        recPath = m.rec_path || null;
      } catch { return null; }
      if (!recPath) return null;
      // rec_path 是 nsbcd 写入 recording.path 下的相对路径（如 20261011/xxx.wav）
      // 后端公开文件路由是 /recordings/*path
      return '/recordings/' + recPath.replace(/^\/+/, '');
    }
    // 接续时长（响铃到应答）= answer_time - start_time
    function ringTime(startIso, answerIso) {
      if (!answerIso) return '-';
      const d = Math.round((isoMs(answerIso) - isoMs(startIso)) / 1000);
      return d >= 0 ? d + 's' : '-';
    }
    // 计费时长 = 优先用后端 billsec，否则用 duration_sec - 接续时长
    function formatBillsec(row) {
      if (row.billsec != null) return row.billsec + 's';
      if (row.duration_sec != null && row.answer_time) {
        const ring = Math.round((isoMs(row.answer_time) - isoMs(row.start_time)) / 1000);
        return Math.max(0, row.duration_sec - ring) + 's';
      }
      return row.duration_sec != null ? row.duration_sec + 's' : '-';
    }
    async function load() {
      loading.value = true; error.value = null;
      try {
        const offset = (currentPage.value - 1) * pageSize.value;
        const data = await API.get(`/cdrs?limit=${pageSize.value}&offset=${offset}`);
        rows.value = data.items || [];
        total.value = data.total || data.items?.length || 0;
      } catch (e) {
        if (isCoreOfflineError(e.message)) { console.warn('[live] core offline:', e.message); }
        else error.value = e.message;
      }
      loading.value = false;
    }
    onMounted(() => { load(); timer = setInterval(load, 10000); });
    onMounted(() => clearInterval(timer));
    return { rows, total, loading, error, currentPage, totalPages, goPage, getRecUrl, getMeta, ringTime, formatBillsec, fmtIso, isoMs };
  },
});

// ===== 性能指标（Prometheus） =====
app.component('metrics-live', {
  template: `
    <div>
      <h2 class="text-lg font-semibold mb-4"><i class="fa-solid fa-chart-line"></i> Prometheus 指标</h2>
      <div v-if="loading" class="text-gray-400">加载中...</div>
      <div v-else-if="error" class="text-red-500">⚠ {{ error }}</div>
      <div v-else class="grid grid-cols-1 md:grid-cols-2 gap-2">
        <div v-for="(v, k) in metrics" :key="k" class="flex justify-between px-3 py-2 border rounded hover:bg-blue-50/50">
          <span class="font-mono text-xs text-gray-600">{{ k }}</span>
          <span class="font-mono text-sm font-semibold text-blue-700">{{ v }}</span>
        </div>
      </div>
    </div>
  `,
  setup() {
    const metrics = ref({}), loading = ref(false), error = ref(null);
    let timer;
    async function load() {
      loading.value = true; error.value = null;
      try {
        metrics.value = await API.get('/metrics');
      } catch (e) {
        if (isCoreOfflineError(e.message)) { console.warn('[live] core offline:', e.message); }
        else error.value = e.message;
      }
      loading.value = false;
    }
    onMounted(() => { load(); timer = setInterval(load, 10000); });
    onMounted(() => clearInterval(timer));
    return { metrics, loading, error };
  },
});

// ===== 注册状态 =====
app.component('registrations-live', {
  template: `
    <div>
      <div class="flex items-center justify-between mb-4">
        <h2 class="text-lg font-semibold"><i class="fa-solid fa-user-check"></i> 注册状态
          <span v-if="shown" class="text-sm text-gray-500">
            ({{ shown }} 个分机<span v-if="total > shown"> · {{ total }} 个注册端点</span>)
          </span>
        </h2>
        <div v-if="truncated" class="text-xs text-yellow-600">⚠ 输出被截断</div>
      </div>
      <div v-if="loading" class="text-gray-400">加载中...</div>
      <div v-else-if="error" class="text-red-500">⚠ {{ error }}</div>
      <div v-else-if="!rows.length" class="text-center text-gray-400 py-12">暂无注册（未分机注册）</div>
      <table v-else class="w-full text-sm border-collapse">
        <thead><tr class="bg-gray-50 border-b">
          <th class="px-3 py-2 text-left font-medium text-gray-600">分机</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">AoR</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">Contact（原始）</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">NAT</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">Received</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">可达 Contact</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">Transport</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">已注册</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">剩余</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">状态</th>
        </tr></thead>
        <tbody>
          <tr v-for="(row, idx) in rows" :key="idx" class="border-b hover:bg-blue-50/50">
            <td class="px-3 py-2 font-mono text-sm font-semibold text-blue-700">{{ row.user || '-' }}</td>
            <td class="px-3 py-2 font-mono text-xs text-gray-600">{{ row.aor || '-' }}</td>
            <td class="px-3 py-2 font-mono text-xs text-gray-500 truncate max-w-[240px]" :title="row.contact">{{ row.contact || '-' }}</td>
            <!-- NAT 标记（REQ-NAT-001） -->
            <td class="px-3 py-2 text-xs">
              <span v-if="row.nat_detected" class="inline-block px-1.5 py-0.5 rounded text-white bg-amber-500 font-semibold">NAT</span>
              <span v-else-if="row.received_ip && row.received_ip.length" class="text-green-600 font-medium">无</span>
              <span v-else class="text-gray-400">-</span>
            </td>
            <!-- Received：NAT 后真实到达地址（FreeSWITCH Via received/rport 等效） -->
            <td class="px-3 py-2 font-mono text-xs" :class="row.nat_detected ? 'text-amber-600' : 'text-gray-500'">
              <div v-if="row.received_ip || row.received_port">
                <div>{{ row.received_ip || '-' }}<span class="text-gray-400">:</span>{{ row.received_port || '-' }}</div>
              </div>
              <span v-else class="text-gray-400">-</span>
            </td>
            <!-- 可达 Contact：NAT 改写后的公网可达地址（FreeSWITCH fs_nat=1 等效） -->
            <td class="px-3 py-2 font-mono text-xs truncate max-w-[240px]"
                :class="row.nat_detected ? 'text-amber-700' : 'text-gray-500'"
                :title="row.public_contact">
              {{ row.public_contact && row.public_contact !== row.contact ? row.public_contact : (row.contact || '-') }}
            </td>
            <td class="px-3 py-2 text-xs text-gray-500">{{ row.transport || '-' }}</td>
            <td class="px-3 py-2 text-xs">{{ fmtDur(row.alive_sec) }}</td>
            <td class="px-3 py-2 text-xs" :class="renewClass(row.expires_sec)">{{ fmtDur(row.expires_sec) }}</td>
            <td class="px-3 py-2 text-xs">
              <span class="inline-block w-2 h-2 rounded-full mr-1" :class="row.expires_sec > 30 ? 'bg-green-500' : 'bg-yellow-500'"></span>
              {{ row.expires_sec > 0 ? '在线' : '即将过期' }}
            </td>
          </tr>
        </tbody>
      </table>
    </div>
  `,
  setup() {
    const data = ref(null), loading = ref(false), error = ref(null);
    // 按分机号去重：同一分机多终端注册时，保留剩余有效时间最长（最新注册）那条
    // loc_db 允许多 binding（多终端/GRUU），UI 只显示每分机一条
    const rows = computed(() => {
      const list = (data.value && data.value.bindings) || [];
      const map = new Map();
      for (const b of list) {
        const key = b.user || b.aor || b.contact || '';
        const exist = map.get(key);
        // 剩余时间更长 = 最新注册，保留
        if (!exist || (b.expires_sec ?? 0) > (exist.expires_sec ?? 0)) {
          map.set(key, b);
        }
      }
      return Array.from(map.values());
    });
    // 总数 = 原始 binding 数（真实注册数，不去重）
    const total = computed(() => data.value?.registrations || ((data.value && data.value.bindings) || []).length);
    const shown = computed(() => rows.value.length);
    const truncated = computed(() => data.value?.truncated || false);
    let timer;

    function fmtDur(sec) {
      if (sec == null) return '-';
      sec = Math.max(0, sec);
      if (sec < 60) return sec + 's';
      if (sec < 3600) return Math.floor(sec/60) + 'm' + (sec%60 ? (sec%60) + 's' : '');
      const h = Math.floor(sec/3600), m = Math.floor((sec%3600)/60);
      return h + 'h' + (m ? m + 'm' : '');
    }
    function renewClass(sec) {
      if (sec == null || sec < 0) return 'text-yellow-600 font-medium';
      if (sec < 60) return 'text-red-600 font-medium';
      return 'text-gray-600';
    }

    async function load() {
      loading.value = true; error.value = null;
      try { data.value = await API.get('/registrations'); }
      catch (e) { error.value = e.message; }
      loading.value = false;
    }
    onMounted(() => { load(); timer = setInterval(load, 5000); });
    onMounted(() => clearInterval(timer));
    return { data, rows, total, shown, truncated, loading, error, fmtDur, renewClass };
  },
});

// ===== 费率管理 =====
app.component('rates-live', {
  template: `
    <div>
      <div class="flex items-center justify-between mb-4">
        <h2 class="text-lg font-semibold"><i class="fa-solid fa-coins"></i> 费率表</h2>
        <span class="text-xs text-gray-500">共 {{ total }} 条 · 微元 (1e-6 元)</span>
      </div>
      <div v-if="loading" class="text-gray-400">加载中...</div>
      <div v-else-if="error" class="text-red-500">⚠ {{ error }}</div>
      <div v-else-if="!rows.length" class="text-center text-gray-400 py-12">暂无费率（通过 C 侧 rate_upsert 或后续 Web CRUD 添加）</div>
      <table v-else class="w-full text-sm border-collapse">
        <thead><tr class="bg-gray-50 border-b">
          <th class="px-3 py-2 text-left font-medium text-gray-600">前缀</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">名称</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">单价/分钟(微元)</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">接通费(微元)</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">计费块(s)</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">最小收费(s)</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">启用</th>
        </tr></thead>
        <tbody>
          <tr v-for="(row, idx) in rows" :key="idx" class="border-b hover:bg-blue-50/50">
            <td class="px-3 py-2 font-mono font-semibold">{{ row.prefix }}</td>
            <td class="px-3 py-2">{{ row.name || '-' }}</td>
            <td class="px-3 py-2 font-mono">{{ row.price_per_min_uc }}</td>
            <td class="px-3 py-2 font-mono">{{ row.connect_fee_uc }}</td>
            <td class="px-3 py-2">{{ row.bill_inc_s }}</td>
            <td class="px-3 py-2">{{ row.min_bill_s }}</td>
            <td class="px-3 py-2">
              <span v-if="row.enabled" class="text-green-600">✓</span>
              <span v-else class="text-gray-400">✗</span>
            </td>
          </tr>
        </tbody>
      </table>
    </div>
  `,
  setup() {
    const rows = ref([]), total = ref(0), loading = ref(false), error = ref(null);
    async function load() {
      loading.value = true; error.value = null;
      try {
        const data = await API.get('/rates');
        rows.value = data.rates || [];
        total.value = data.total || rows.value.length;
      } catch (e) {
        if (isCoreOfflineError(e.message)) { console.warn('[live] core offline:', e.message); }
        else error.value = e.message;
      }
      loading.value = false;
    }
    onMounted(load);
    return { rows, total, loading, error };
  },
});

// ===== 账户余额 =====
app.component('balances-live', {
  template: `
    <div>
      <div class="flex items-center justify-between mb-4">
        <h2 class="text-lg font-semibold"><i class="fa-solid fa-wallet"></i> 账户余额</h2>
        <span class="text-xs text-gray-500">共 {{ total }} 个账户 · 微元 (1e-6 元)</span>
      </div>
      <div v-if="loading" class="text-gray-400">加载中...</div>
      <div v-else-if="error" class="text-red-500">⚠ {{ error }}</div>
      <div v-else-if="!rows.length" class="text-center text-gray-400 py-12">暂无账户</div>
      <table v-else class="w-full text-sm border-collapse">
        <thead><tr class="bg-gray-50 border-b">
          <th class="px-3 py-2 text-left font-medium text-gray-600">账户</th>
          <th class="px-3 py-2 text-right font-medium text-gray-600">余额(微元)</th>
          <th class="px-3 py-2 text-right font-medium text-gray-600">已预占(微元)</th>
          <th class="px-3 py-2 text-right font-medium text-gray-600">透支额度(微元)</th>
          <th class="px-3 py-2 text-right font-medium text-gray-600">可用余额</th>
        </tr></thead>
        <tbody>
          <tr v-for="(row, idx) in rows" :key="idx" class="border-b hover:bg-blue-50/50">
            <td class="px-3 py-2 font-mono font-semibold">{{ row.key }}</td>
            <td class="px-3 py-2 font-mono text-right">{{ row.balance_uc }}</td>
            <td class="px-3 py-2 font-mono text-right text-orange-600">{{ row.reserved_uc }}</td>
            <td class="px-3 py-2 font-mono text-right">{{ row.credit_limit_uc }}</td>
            <td class="px-3 py-2 font-mono text-right"
                :class="(row.balance_uc - row.reserved_uc) < 0 ? 'text-red-600' : 'text-green-600'">
              {{ row.balance_uc - row.reserved_uc + row.credit_limit_uc }}
            </td>
          </tr>
        </tbody>
      </table>
    </div>
  `,
  setup() {
    const rows = ref([]), total = ref(0), loading = ref(false), error = ref(null);
    async function load() {
      loading.value = true; error.value = null;
      try {
        const data = await API.get('/balances');
        rows.value = data.accounts || [];
        total.value = data.total || rows.value.length;
      } catch (e) {
        if (isCoreOfflineError(e.message)) { console.warn('[live] core offline:', e.message); }
        else error.value = e.message;
      }
      loading.value = false;
    }
    onMounted(load);
    return { rows, total, loading, error };
  },
});

// ===== 媒体会话（bridge/transcode） =====
app.component('transcode-live', {
  template: `
    <div>
      <div class="flex items-center justify-between mb-4">
        <h2 class="text-lg font-semibold"><i class="fa-solid fa-microphone"></i> 媒体 Bridge 会话</h2>
        <span class="text-xs text-gray-500">活跃会话：{{ total }}</span>
      </div>
      <div v-if="loading" class="text-gray-400">加载中...</div>
      <div v-else-if="error" class="text-red-500">⚠ {{ error }}</div>
      <div v-else-if="!rows.length" class="text-center text-gray-400 py-12">无活跃媒体会话</div>
      <table v-else class="w-full text-sm border-collapse">
        <thead><tr class="bg-gray-50 border-b">
          <th class="px-3 py-2 text-left font-medium text-gray-600">#</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">Call Key</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">模式</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">A-RTP</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">B-RTP</th>
          <th class="px-3 py-2 text-right font-medium text-gray-600">转发包</th>
          <th class="px-3 py-2 text-right font-medium text-gray-600">丢弃包</th>
        </tr></thead>
        <tbody>
          <tr v-for="(row, idx) in rows" :key="idx" class="border-b hover:bg-blue-50/50">
            <td class="px-3 py-2 text-gray-400">{{ row.idx }}</td>
            <td class="px-3 py-2 font-mono text-xs">{{ row.call_key_ptr }}</td>
            <td class="px-3 py-2">
              <span :class="{
                'px-2 py-0.5 rounded text-xs font-medium': true,
                'bg-gray-100 text-gray-600': row.mode === 'bypass',
                'bg-blue-100 text-blue-700': row.mode === 'relay',
                'bg-orange-100 text-orange-700': row.mode === 'transcode',
              }">{{ row.mode }}</span>
            </td>
            <td class="px-3 py-2 font-mono">{{ row.a_rtp || '-' }}</td>
            <td class="px-3 py-2 font-mono">{{ row.b_rtp || '-' }}</td>
            <td class="px-3 py-2 font-mono text-right">{{ row.pkts }}</td>
            <td class="px-3 py-2 font-mono text-right"
                :class="row.drops > 0 ? 'text-red-600' : 'text-gray-400'">{{ row.drops }}</td>
          </tr>
        </tbody>
      </table>
    </div>
  `,
  setup() {
    const rows = ref([]), total = ref(0), loading = ref(false), error = ref(null);
    let timer;
    async function load() {
      loading.value = true; error.value = null;
      try {
        const data = await API.get('/transcode');
        rows.value = data.sessions || [];
        total.value = data.total || rows.value.length;
      } catch (e) {
        if (isCoreOfflineError(e.message)) { console.warn('[live] core offline:', e.message); }
        else error.value = e.message;
      }
      loading.value = false;
    }
    onMounted(() => { load(); timer = setInterval(load, 3000); });
    onMounted(() => clearInterval(timer));
    return { rows, total, loading, error };
  },
});
