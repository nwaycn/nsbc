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

// ---- 辅助：判断是否为"Core（nsbcd）Not connected/启动"类Error，
// 这类Error属于系统级（C Core没跑、UDS 不存在、502 Bad Gateway），
// 面板应静默（Hold显示空/上次数据）+ console.warn，不渲染红色报错，
// 避免Core offline时 6 个轮询组件每 5s 各自刷一"弹错"。
function isCoreOfflineError(msg) {
  if (!msg) return false;
  const m = String(msg).toLowerCase();
  return m.includes('bad gateway') || m.includes('core') || m.includes('unavailable')
      || m.includes('timeout') || m.includes('connect') || m.includes('no such file');
}

// ===== Realtime Overview（综合Status） =====
app.component('realtime', {
  template: `
    <div>
      <div class="flex items-center justify-between mb-4">
        <h2 class="text-lg font-semibold"><i class="fa-solid fa-gauge-high"></i>{{ t(' Realtime Overview') }}</h2>
        <div class="flex items-center gap-2">
          <button @click="showOrigDlg = true" class="px-3 py-1.5 text-sm bg-blue-600 hover:bg-blue-700 text-white rounded shadow-sm">
            <i class="fa-solid fa-phone-plus mr-1"></i> Originate test
          </button>
          <span class="text-xs text-gray-500">{{ t('Last updated:') }} {{ lastUpdated }}</span>
        </div>
      </div>
      <div v-if="loading" class="text-gray-400">{{ t('Loading...') }}</div>
      <div v-else-if="error" class="text-red-500 mb-4">⚠ {{ error }}</div>
      <div v-else class="grid grid-cols-2 md:grid-cols-4 gap-4">
        <div class="bg-blue-50 border border-blue-200 rounded-lg p-4">
          <div class="text-xs text-blue-600 font-medium">{{ t('Active calls') }}</div>
          <div class="text-3xl font-bold text-blue-700 mt-1">{{ calls && calls.active != null ? calls.active : '-' }}</div>
        </div>
        <div class="bg-green-50 border border-green-200 rounded-lg p-4">
          <div class="text-xs text-green-600 font-medium">{{ t('Total calls') }}</div>
          <div class="text-3xl font-bold text-green-700 mt-1">{{ calls && calls.total != null ? calls.total : '-' }}</div>
        </div>
        <div class="bg-red-50 border border-red-200 rounded-lg p-4">
          <div class="text-xs text-red-600 font-medium">{{ t('Failed') }}</div>
          <div class="text-3xl font-bold text-red-700 mt-1">{{ calls && calls.failed != null ? calls.failed : '-' }}</div>
        </div>
        <div class="bg-purple-50 border border-purple-200 rounded-lg p-4">
          <div class="text-xs text-purple-600 font-medium">{{ t('Registrations') }}</div>
          <div class="text-3xl font-bold text-purple-700 mt-1">{{ regs && regs.registrations != null ? regs.registrations : '-' }}</div>
        </div>
      </div>

      <!-- Originate test弹窗 -->
      <div v-if="showOrigDlg" class="fixed inset-0 bg-black/40 flex items-center justify-center z-50" @click.self="showOrigDlg = false">
        <div class="bg-white rounded-xl shadow-2xl w-[420px] overflow-hidden">
          <div class="px-5 py-3 bg-blue-600 text-white flex items-center justify-between">
            <h3 class="font-semibold"><i class="fa-solid fa-phone-plus mr-2"></i>{{ t('Originate test') }}</h3>
            <button @click="showOrigDlg = false" class="hover:bg-white/20 rounded px-2">✕</button>
          </div>
          <div class="p-5 space-y-4">
            <div>
              <label class="field-label">{{ t('Destination / call-url') }}</label>
              <input v-model="origForm.call_url" class="field-input" placeholder="e.g. sip:1001@nsbc.local">
            </div>
            <div class="grid grid-cols-2 gap-3">
              <div>
                <label class="field-label">{{ t('Caller ID Override') }}</label>
                <input v-model="origForm.caller_id" class="field-input" placeholder="e.g. 1001 or empty">
              </div>
              <div>
                <label class="field-label">{{ t('CallerDisplay name') }}</label>
                <input v-model="origForm.caller_name" class="field-input" placeholder="e.g. Alice or empty">
              </div>
            </div>
            <div>
              <label class="field-label">{{ t('Ringing timeout (s)') }}</label>
              <input v-model="origForm.timeout" type="number" min="1" max="3600"
                     class="field-input" placeholder="30">
            </div>
            <div>
              <label class="field-label">{{ t('Application (optional)') }}</label>
              <select v-model="origForm.app" class="field-input">
                <option value="park">{{ t('park（Hold）') }}</option>
                <option value="echo">{{ t('echo (echo test)') }}</option>
                <option value="hangup">{{ t('hangup（Hang up）') }}</option>
                <option value="answer">{{ t('answer (auto-answer)') }}</option>
              </select>
            </div>
            <div class="flex items-center gap-2 text-xs text-gray-500 bg-blue-50 border border-blue-200 rounded px-3 py-2">
              <i class="fa-solid fa-info-circle text-blue-600"></i>
              <span>{{ t('Send to core ') }}<code class="font-mono">originate {var=val,...}&lt;dest&gt; &amp;&lt;app&gt;</code>{{ t('，ESL/api command takes effect immediately') }}</span>
            </div>
            <div v-if="origMsg" class="text-sm" :class="origOk ? 'text-green-600 bg-green-50' : 'text-red-600 bg-red-50'"
                 class="rounded px-3 py-2 border" :class="origOk ? 'border-green-200' : 'border-red-200'">
              {{ origMsg }}
            </div>
          </div>
          <div class="px-5 py-3 bg-gray-50 flex justify-end gap-2">
            <button @click="showOrigDlg = false" class="btn btn-ghost">{{ t('Cancel') }}</button>
            <button @click="submitOriginate" :disabled="origLoading || !origForm.call_url"
                    class="btn btn-primary disabled:opacity-50">
              <i v-if="origLoading" class="fa-solid fa-spinner fa-spin mr-1"></i>
              {{ origLoading ? t('Sending...') : t('Originate now') }}
            </button>
          </div>
        </div>
      </div>
    </div>
  `,
  setup() {
    const calls = ref(null), regs = ref(null), loading = ref(false), error = ref(null), lastUpdated = ref('');
    // 外呼测试弹窗Status
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
      // Core parse_fs_brace_vars 支持 origination_caller_id_number /
      // origination_caller_id_name / originate_timeout（s）。
      const vars = [];
      if (f.caller_id)   vars.push('origination_caller_id_number=' + String(f.caller_id).trim());
      if (f.caller_name) vars.push('origination_caller_id_name=' + String(f.caller_name).trim());
      if (f.timeout)     vars.push('originate_timeout=' + parseInt(f.timeout, 10));
      let url = (vars.length ? '{' + vars.join(',') + '}' : '') + dest;
      // answer 不是Core支持的后置 app（Core里它会跳过），仅支持 park/echo/hangup
      if (f.app && f.app !== 'answer') url += ' &' + f.app;
      origLoading.value = true; origMsg.value = '';
      try {
        const r = await API.post('/originate', { call_url: url });
        origMsg.value = r.status || 'originate-dispatched';
        origOk.value = true;
      } catch (e) {
        origMsg.value = e.message || 'Originate failed';
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

// Status字符串 → 带颜色徽章（stateBadge）。
// 覆盖 C 侧 LEG_* Status名 + hold/unhold 自定义Value：
//   LEG_NONE/CALLING → calling | LEG_RINGING → ringing | LEG_EARLY → early
//   LEG_CONFIRMED → confirmed | LEG_HELD → held | LEG_TERMINATING → terminated
//   LEG_TERMINATED/NULL → terminated
//   hold → held | unhold → active（Active，语义同 confirmed）
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

// ===== 在途Call =====
app.component('channels-live', livePanel(t('Live channels'), 'fa-solid fa-phone-volume',
  [
    { key: 'a_call_id',    label: 'A-Leg Call-ID' },
    { key: 'a_state',      label: t('A-State'), fmt: stateBadge },
    { key: 'b_call_id',    label: 'B-Leg Call-ID' },
    { key: 'b_state',      label: t('B-State'), fmt: stateBadge },
    { key: 'caller',       label: t('Caller') },
    { key: 'callee',       label: t('Callee') },
    { key: 'start_ms',     label: t('Start time'), fmt: fmtTime },
    { key: 'answer_ms',    label: t('Answer time'), fmt: fmtClock },
    { key: 'duration_ms',  label: t('Duration'), fmt: fmtDuration },
    { key: 'codec_a',      label: t('A-Codec') },
    { key: 'codec_b',      label: t('B-Codec') },
    { key: 'from_ip',      label: t('Source IP') },
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
      return `<button onclick="nsbcHangup('${callId}')" class="px-2 py-1 text-xs text-red-600 border border-red-300 rounded hover:bg-red-50 hover:text-red-800"><i class="fa-solid fa-phone-slash"></i>{{ t(' Hang up') }}</button>`;
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

// ===== Call Record（CDR） =====
app.component('cdrs-live', {
  template: `
    <div>
      <!-- 标题栏 + Tab 切换（全部/正常/恶意） -->
      <div class="flex items-center justify-between mb-3">
        <h2 class="text-lg font-semibold"><i class="fa-solid fa-file-lines"></i>{{ t(' Recent call records') }}</h2>
        <div class="flex items-center gap-4">
          <!-- Tab 切换 -->
          <div class="flex gap-1 bg-gray-100 rounded-lg p-1" role="tablist">
            <button v-for="tab in tabs" :key="tab.key"
              @click="activeTab = tab.key; currentPage = 1; load()"
              :class="[
                'px-3 py-1 text-xs rounded-md transition-colors',
                activeTab === tab.key ? 'bg-white shadow text-blue-700 font-medium' : 'text-gray-500 hover:text-gray-700'
              ]">
              {{ tab.icon }} {{ tab.label }}
            </button>
          </div>
          <!-- 总数 & 自动刷新 -->
          <div class="text-xs text-gray-500">
            {{ t('Total') }} {{ total }} · {{ t('refresh every 10s') }}
          </div>
        </div>
      </div>

      <!-- 搜索面板 -->
      <div class="flex flex-wrap gap-2 mb-3 p-3 bg-gray-50 rounded-lg text-xs">
        <input v-model="q.caller" @keyup.enter="currentPage=1; load()"
          :placeholder="t('Caller')" class="px-2 py-1 border rounded w-28">
        <input v-model="q.callee" @keyup.enter="currentPage=1; load()"
          :placeholder="t('Callee')" class="px-2 py-1 border rounded w-28">
        <input v-model="q.call_id" @keyup.enter="currentPage=1; load()"
          placeholder="Call-ID" class="px-2 py-1 border rounded w-40">
        <input v-model="q.from_ip" @keyup.enter="currentPage=1; load()"
          :placeholder="t('Source IP')" class="px-2 py-1 border rounded w-32">
        <select v-model="q.disposition" @change="currentPage=1; load()" class="px-2 py-1 border rounded">
          <option value="">{{ t('All disposition') }}</option>
          <option value="answered">{{ t('Answered') }}</option>
          <option value="failed">{{ t('Failed') }}</option>
          <option value="busy">{{ t('Busy') }}</option>
          <option value="noanswer">{{ t('No Answer') }}</option>
        </select>
        <span class="flex items-center gap-1">
          <input type="number" v-model.number="q.duration_min" placeholder="min" class="px-2 py-1 border rounded w-16 text-center">
          <span class="text-gray-400">s</span>
          <span class="text-gray-300">-</span>
          <input type="number" v-model.number="q.duration_max" placeholder="max" class="px-2 py-1 border rounded w-16 text-center">
          <span class="text-gray-400">s</span>
        </span>
        <select v-model="sortBy" @change="currentPage=1; load()" class="px-2 py-1 border rounded">
          <option value="start_time">{{ t('Sort by start time') }}</option>
          <option value="duration">{{ t('Sort by duration') }}</option>
          <option value="answer_time">{{ t('Sort by answer time') }}</option>
          <option value="caller">{{ t('Sort by caller') }}</option>
          <option value="callee">{{ t('Sort by callee') }}</option>
        </select>
        <select v-model="sortDir" @change="currentPage=1; load()" class="px-2 py-1 border rounded">
          <option value="desc">{{ t('Descending') }}</option>
          <option value="asc">{{ t('Ascending') }}</option>
        </select>
        <button @click="currentPage=1; load()" class="px-3 py-1 bg-blue-600 text-white rounded hover:bg-blue-700">
          <i class="fa-solid fa-magnifying-glass"></i> {{ t('Search') }}
        </button>
        <button @click="resetFilters" class="px-3 py-1 border rounded hover:bg-gray-100">
          <i class="fa-solid fa-rotate-left"></i> {{ t('Reset') }}
        </button>
      </div>

      <!-- 加载/错误/空状态 -->
      <div v-if="loading" class="text-gray-400">{{ t('Loading...') }}</div>
      <div v-else-if="error" class="text-red-500">⚠ {{ error }}</div>
      <div v-else-if="!rows.length" class="text-center text-gray-400 py-12">{{ t('No CDRs yet') }}</div>

      <!-- 表格 -->
      <table v-else class="w-full text-sm border-collapse">
        <thead><tr class="bg-gray-50 border-b">
          <th class="px-3 py-2 text-left font-medium text-gray-600">{{ t('Caller') }}</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">{{ t('Callee') }}</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">{{ t('Source IP') }}</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">{{ t('A-Codec') }}</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">{{ t('B-Codec') }}</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">Call-ID</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">{{ t('Start time') }}</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">{{ t('Answer time') }}</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">{{ t('Ringing time') }}</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">{{ t('End time') }}</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">{{ t('Total duration') }}</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">{{ t('Billing duration') }}</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">{{ t('Disposition') }}</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">{{ t('Threat') }}</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">{{ t('Recording') }}</th>
        </tr></thead>
        <tbody>
          <tr v-for="(row, idx) in rows" :key="idx"
              :class="[
                'border-b hover:bg-blue-50/50',
                row.is_malicious ? 'bg-red-50/40' : ''
              ]">
            <td class="px-3 py-2 font-mono text-xs">{{ row.caller || '-' }}</td>
            <td class="px-3 py-2 font-mono text-xs">{{ row.callee || '-' }}</td>
            <td class="px-3 py-2 font-mono text-xs"
                :class="row.is_malicious ? 'text-red-600 font-semibold' : 'text-blue-600'">
              {{ row.from_ip || getMeta(row, 'from_ip') || '-' }}
            </td>
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
            <!-- Threat 标记（REQ-SEC-005） -->
            <td class="px-3 py-2 text-xs">
              <template v-if="row.is_malicious">
                <span class="inline-flex items-center gap-1 px-2 py-0.5 bg-red-100 text-red-700 rounded-full text-[10px] font-medium">
                  <i class="fa-solid fa-triangle-exclamation"></i>
                  {{ row.threat_reason || 'malicious' }}
                </span>
              </template>
              <template v-else>
                <span class="text-gray-300">-</span>
              </template>
            </td>
            <td class="px-3 py-2 text-xs">
              <template v-if="getRecUrl(row)">
                <audio :src="getRecUrl(row)" controls preload="none" class="h-6 align-middle"></audio>
                <a :href="getRecUrl(row)" download target="_blank" class="ml-2 text-blue-600 hover:underline">
                  <i class="fa-solid fa-download"></i>
                </a>
              </template>
              <template v-else>-</template>
            </td>
          </tr>
        </tbody>
      </table>
      <!-- Pagination -->
      <div v-if="total > pageSize" class="flex items-center justify-between mt-3 text-xs text-gray-500">
        <div>{{ t('Total') }} {{ total }} · {{ t('Page size') }} {{ pageSize }}</div>
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

    // Tab 切换（REQ-SEC-005: malicious filter）
    const tabs = [
      { key: 'all',      label: '全部',   icon: '📋' },
      { key: 'normal',   label: '正常',   icon: '✅' },
      { key: 'malicious',label: '恶意',   icon: '⚠️' },
    ];
    const activeTab = ref('all');

    // 搜索条件
    const q = reactive({
      caller: '',
      callee: '',
      call_id: '',
      from_ip: '',
      disposition: '',
      duration_min: null,
      duration_max: null,
    });

    // 排序
    const sortBy = ref('start_time');
    const sortDir = ref('desc');

    function resetFilters() {
      q.caller = ''; q.callee = ''; q.call_id = ''; q.from_ip = '';
      q.disposition = ''; q.duration_min = null; q.duration_max = null;
      activeTab.value = 'all';
      sortBy.value = 'start_time'; sortDir.value = 'desc';
      currentPage.value = 1;
      load();
    }

    function goPage(p) {
      currentPage.value = p;
      load();
    }
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
      return '/recordings/' + recPath.replace(/^\/+/, '');
    }
    function ringTime(startIso, answerIso) {
      if (!answerIso) return '-';
      const d = Math.round((isoMs(answerIso) - isoMs(startIso)) / 1000);
      return d >= 0 ? d + 's' : '-';
    }
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
        const params = new URLSearchParams();
        params.set('limit', pageSize.value);
        params.set('offset', offset);
        // 搜索条件
        if (q.caller) params.set('caller', q.caller);
        if (q.callee) params.set('callee', q.callee);
        if (q.call_id) params.set('call_id', q.call_id);
        if (q.from_ip) params.set('from_ip', q.from_ip);
        if (q.disposition) params.set('disposition', q.disposition);
        if (q.duration_min != null && q.duration_min !== '') params.set('duration_min', q.duration_min);
        if (q.duration_max != null && q.duration_max !== '') params.set('duration_max', q.duration_max);
        // Tab 切换 → malicious= filter
        if (activeTab.value === 'malicious') params.set('malicious', '1');
        else if (activeTab.value === 'normal') params.set('malicious', '0');
        // 排序
        if (sortBy.value !== 'start_time') params.set('sort_by', sortBy.value);
        if (sortDir.value !== 'desc') params.set('sort_dir', sortDir.value);

        const data = await API.get(`/cdrs?${params.toString()}`);
        rows.value = data.items || [];
        total.value = data.total || 0;
      } catch (e) {
        if (isCoreOfflineError(e.message)) { console.warn('[live] core offline:', e.message); }
        else error.value = e.message;
      }
      loading.value = false;
    }
    onMounted(() => { load(); timer = setInterval(load, 10000); });
    onMounted(() => clearInterval(timer));
    return { rows, total, loading, error, currentPage, totalPages, goPage,
             getRecUrl, getMeta, ringTime, formatBillsec, fmtIso, isoMs,
             tabs, activeTab, q, sortBy, sortDir, resetFilters };
  },
});

// ===== 性能指标（Prometheus） =====
app.component('metrics-live', {
  template: `
    <div>
      <h2 class="text-lg font-semibold mb-4"><i class="fa-solid fa-chart-line"></i> Prometheus 指标</h2>
      <div v-if="loading" class="text-gray-400">{{ t('Loading...') }}</div>
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

// ===== Reg Status =====
app.component('registrations-live', {
  template: `
    <div>
      <div class="flex items-center justify-between mb-4">
        <h2 class="text-lg font-semibold"><i class="fa-solid fa-user-check"></i> Registration status
          <span v-if="shown" class="text-sm text-gray-500">
            ({{ shown }} <span v-if="total > shown"> · {{ total }} {{ t('registration endpoints') }}</span>)
          </span>
        </h2>
        <div v-if="truncated" class="text-xs text-yellow-600">{{ t('⚠ Output truncated') }}</div>
      </div>
      <div v-if="loading" class="text-gray-400">{{ t('Loading...') }}</div>
      <div v-else-if="error" class="text-red-500">⚠ {{ error }}</div>
      <div v-else-if="!rows.length" class="text-center text-gray-400 py-12">{{ t('No registrations yet') }}</div>
      <table v-else class="w-full text-sm border-collapse">
        <thead><tr class="bg-gray-50 border-b">
          <th class="px-3 py-2 text-left font-medium text-gray-600">{{ t('Extension') }}</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">AoR</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">Contact（原始）</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">NAT</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">Received</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">{{ t('Public contact') }}</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">Transport</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">{{ t('Registered') }}</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">{{ t('Remaining') }}</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">{{ t('Status') }}</th>
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
            <!-- Public contact：NAT 改写后的公网可达地址（FreeSWITCH fs_nat=1 等效） -->
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
              {{ row.expires_sec > 0 ? t('Online') : t('Expiring soon') }}
            </td>
          </tr>
        </tbody>
      </table>
    </div>
  `,
  setup() {
    const data = ref(null), loading = ref(false), error = ref(null);
    // 按Extension去重：同一Extension多终端Register时，保留剩余有效时间最长（最新Register）那
    // loc_db Allow多 binding（多终端/GRUU），UI 只显示每Extension一
    const rows = computed(() => {
      const list = (data.value && data.value.bindings) || [];
      const map = new Map();
      for (const b of list) {
        const key = b.user || b.aor || b.contact || '';
        const exist = map.get(key);
        // 剩余时间更长 = 最新Register，保留
        if (!exist || (b.expires_sec ?? 0) > (exist.expires_sec ?? 0)) {
          map.set(key, b);
        }
      }
      return Array.from(map.values());
    });
    // 总数 = 原始 binding 数（真实Register数，不去重）
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

// ===== 费率管理（完整 CRUD） =====
app.component('rates-live', crudTable(
  { label: t('Rate Table'), icon: 'fa-solid fa-coins' },
  '/rates',
  [
    { key: 'prefix', label: t('Prefix') },
    { key: 'name', label: t('Name') },
    { key: 'price_per_min_uc', label: t('Rate/min (µ)') },
    { key: 'connect_fee_uc',   label: t('Connect fee (µ)') },
    { key: 'bill_inc_s',      label: t('Billing block (s)') },
    { key: 'min_bill_s',      label: t('Min bill (s)') },
    { key: 'enabled',         label: t('Enable'), type: 'bool' },
  ],
  [
    { key: 'prefix',          label: t('Prefix'),              type: 'text',   required: true, placeholder: 'e.g. 86, 8610, 44' },
    { key: 'name',            label: t('Name'),                type: 'text',   default: '' },
    { key: 'price_per_min_uc',label: t('Rate/min (µ)'),       type: 'number', default: 0, help: 'Micro-units per minute (1e-6 CNY)' },
    { key: 'connect_fee_uc',  label: t('Connect fee (µ)'),     type: 'number', default: 0 },
    { key: 'bill_inc_s',      label: t('Billing block (s)'),   type: 'number', default: 60, help: 'Per-call billing granularity' },
    { key: 'min_bill_s',      label: t('Min bill (s)'),       type: 'number', default: 60, help: 'Minimum billing seconds per call' },
    { key: 'enabled',         label: t('Enable'),              type: 'bool',   default: true },
  ],
));

// ===== 账户余额（完整 CRUD：open/close + edit credit_limit/description） =====
app.component('balances-live', crudTable(
  { label: t('Account Balance'), icon: 'fa-solid fa-wallet' },
  '/balances',
  [
    { key: 'account_key',     label: t('Account Key') },
    { key: 'balance_uc',      label: t('Balance (µ)') },
    { key: 'credit_limit_uc', label: t('Credit limit (µ)') },
    { key: 'description',     label: t('Description') },
  ],
  [
    { key: 'account_key',     label: t('Account Key'),     type: 'text',   required: true, help: 'Unique account identifier (e.g. tenant-001)' },
    { key: 'balance_uc',      label: t('Opening balance (µ)'), type: 'number', default: 0, help: 'Initial credit in micro-units (1e-6 CNY)' },
    { key: 'credit_limit_uc', label: t('Credit limit (µ)'),   type: 'number', default: 0 },
    { key: 'description',     label: t('Description'),       type: 'textarea' },
  ],
));

// ===== MediaSession（bridge/transcode） =====
app.component('transcode-live', {
  template: `
    <div>
      <div class="flex items-center justify-between mb-4">
        <h2 class="text-lg font-semibold"><i class="fa-solid fa-microphone"></i>{{ t(' Media Bridge sessions') }}</h2>
        <span class="text-xs text-gray-500">{{ t('Active sessions:') }} {{ total }}</span>
      </div>
      <div v-if="loading" class="text-gray-400">{{ t('Loading...') }}</div>
      <div v-else-if="error" class="text-red-500">⚠ {{ error }}</div>
      <div v-else-if="!rows.length" class="text-center text-gray-400 py-12">{{ t('无活跃Media sessions') }}</div>
      <table v-else class="w-full text-sm border-collapse">
        <thead><tr class="bg-gray-50 border-b">
          <th class="px-3 py-2 text-left font-medium text-gray-600">#</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">Call Key</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">{{ t('Mode') }}</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">A-RTP</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">B-RTP</th>
          <th class="px-3 py-2 text-right font-medium text-gray-600">{{ t('Forwarded pkts') }}</th>
          <th class="px-3 py-2 text-right font-medium text-gray-600">{{ t('Dropped pkts') }}</th>
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

// ===== Codec Registry (read-only, core self-registered) =====
app.component('codecs-live', livePanel(t('Codec Registry'), 'fa-solid fa-code', [
  { key: 'name',         label: t('Codec Name') },
  { key: 'pt',           label: t('PT') },
  { key: 'sample_rate',  label: t('Sample Rate') },
  { key: 'impl_module',  label: t('Impl Module') },
  { key: 'fmtp_default', label: t('FMTP Default') },
  { key: 'media_type',   label: t('Media Type') },
], '/codecs', { refreshMs: 10000 }));

