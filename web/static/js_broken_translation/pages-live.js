// =============================================================================
// NSBC Web — live monitoring / read-only panels
// =============================================================================

// ---- helpers ----
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

function isCoreOfflineError(msg) {
  if (!msg) return false;
  const m = String(msg).toLowerCase();
  return m.includes('bad gateway') || m.includes('core') || m.includes('unavailable')
      || m.includes('timeout') || m.includes('connect') || m.includes('no such file');
}

// ===== Realtime overview =====
app.component('realtime', {
  template: `
    <div>
      <div class="flex items-center justify-between mb-4">
        <h2 class="text-lg font-semibold"><i class="fa-solid fa-gauge-high"></i> ${ t('rt.title') }</h2>
        <div class="flex items-center gap-2">
          <button @click="showOrigDlg = true" class="px-3 py-1.5 text-sm bg-blue-600 hover:bg-blue-700 text-white rounded shadow-sm">
            <i class="fa-solid fa-phone-plus mr-1"></i> ${ t('rt.originateTest') }
          </button>
          <span class="text-xs text-gray-500">${ t('live.updatedAt') }: {{ lastUpdated }}</span>
        </div>
      </div>
      <div v-if="loading" class="text-gray-400">${ t('generic.loading') }</div>
      <div v-else-if="error" class="text-red-500 mb-4">⚠ {{ error }}</div>
      <div v-else class="grid grid-cols-2 md:grid-cols-4 gap-4">
        <div class="bg-blue-50 border border-blue-200 rounded-lg p-4">
          <div class="text-xs text-blue-600 font-medium">${ t('rt.activeCalls') }</div>
          <div class="text-3xl font-bold text-blue-700 mt-1">{{ calls && calls.active != null ? calls.active : '-' }}</div>
        </div>
        <div class="bg-green-50 border border-green-200 rounded-lg p-4">
          <div class="text-xs text-green-600 font-medium">${ t('rt.totalCalls') }</div>
          <div class="text-3xl font-bold text-green-700 mt-1">{{ calls && calls.total != null ? calls.total : '-' }}</div>
        </div>
        <div class="bg-red-50 border border-red-200 rounded-lg p-4">
          <div class="text-xs text-red-600 font-medium">${ t('rt.failedCalls') }</div>
          <div class="text-3xl font-bold text-red-700 mt-1">{{ calls && calls.failed != null ? calls.failed : '-' }}</div>
        </div>
        <div class="bg-purple-50 border border-purple-200 rounded-lg p-4">
          <div class="text-xs text-purple-600 font-medium">${ t('rt.registrations') }</div>
          <div class="text-3xl font-bold text-purple-700 mt-1">{{ regs && regs.registrations != null ? regs.registrations : '-' }}</div>
        </div>
      </div>

      <!-- Originate dialog -->
      <div v-if="showOrigDlg" class="fixed inset-0 bg-black/40 flex items-center justify-center z-50" @click.self="showOrigDlg = false">
        <div class="bg-white rounded-xl shadow-2xl w-[420px] overflow-hidden">
          <div class="px-5 py-3 bg-blue-600 text-white flex items-center justify-between">
            <h3 class="font-semibold"><i class="fa-solid fa-phone-plus mr-2"></i>${ t('rt.originateTest') }</h3>
            <button @click="showOrigDlg = false" class="hover:bg-white/20 rounded px-2">✕</button>
          </div>
          <div class="p-5 space-y-4">
            <div>
              <label class="field-label">${ t('rt.callUrl') }</label>
              <input v-model="origForm.call_url" class="field-input" placeholder="e.g. 1003 or sip:user@trunk">
            </div>
            <div class="grid grid-cols-2 gap-3">
              <div>
                <label class="field-label">${ t('rt.callerId') }</label>
                <input v-model="origForm.caller_id" class="field-input" placeholder="e.g. 8888">
              </div>
              <div>
                <label class="field-label">${ t('rt.callerName') }</label>
                <input v-model="origForm.caller_name" class="field-input" placeholder="e.g. Switch">
              </div>
            </div>
            <div>
              <label class="field-label">${ t('rt.ringTimeout') }</label>
              <input v-model="origForm.timeout" type="number" min="1" max="3600"
                     class="field-input" placeholder="e.g. 50 (core default if empty)">
            </div>
            <div>
              <label class="field-label">${ t('rt.app') }</label>
              <select v-model="origForm.app" class="field-input">
                <option value="park">park</option>
                <option value="echo">echo</option>
                <option value="hangup">hangup (${ t('live.hangup') })</option>
                <option value="answer">answer</option>
              </select>
            </div>
            <div class="flex items-center gap-2 text-xs text-gray-500 bg-blue-50 border border-blue-200 rounded px-3 py-2">
              <i class="fa-solid fa-info-circle text-blue-600"></i>
              <span>${ t('live.originateHint') }</span>
            </div>
            <div v-if="origMsg" class="text-sm" :class="origOk ? 'text-green-600 bg-green-50' : 'text-red-600 bg-red-50'"
                 class="rounded px-3 py-2 border" :class="origOk ? 'border-green-200' : 'border-red-200'">
              {{ origMsg }}
            </div>
          </div>
          <div class="px-5 py-3 bg-gray-50 flex justify-end gap-2">
            <button @click="showOrigDlg = false" class="btn btn-ghost">${ t('generic.cancel') }</button>
            <button @click="submitOriginate" :disabled="origLoading || !origForm.call_url"
                    class="btn btn-primary disabled:opacity-50">
              <i v-if="origLoading" class="fa-solid fa-spinner fa-spin mr-1"></i>
              ${ origLoading ? t('rt.sending') : t('rt.originate') }
            </button>
          </div>
        </div>
      </div>
    </div>
  `,
  setup() {
    const calls = ref(null), regs = ref(null), loading = ref(false), error = ref(null), lastUpdated = ref('');
    const showOrigDlg = ref(false);
    const origForm = ref({ call_url: '', app: 'park', caller_id: '', caller_name: '', timeout: '' });
    const origLoading = ref(false);
    const origMsg = ref('');
    const origOk = ref(false);

    async function submitOriginate() {
      const f = origForm.value;
      const dest = (f.call_url || '').trim();
      if (!dest) { return; }
      const vars = [];
      if (f.caller_id)   vars.push('origination_caller_id_number=' + String(f.caller_id).trim());
      if (f.caller_name) vars.push('origination_caller_id_name=' + String(f.caller_name).trim());
      if (f.timeout)     vars.push('originate_timeout=' + parseInt(f.timeout, 10));
      let url = (vars.length ? '{' + vars.join(',') + '}' : '') + dest;
      if (f.app && f.app !== 'answer') url += ' &' + f.app;
      origLoading.value = true; origMsg.value = '';
      try {
        const r = await API.post('/originate', { call_url: url });
        origMsg.value = r.status || 'originate-dispatched';
        origOk.value = true;
      } catch (e) {
        origMsg.value = e.message || '';
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
    onMounted(() => document.addEventListener('nsbc-refresh', onSseRefresh));
    onBeforeUnmount(() => { clearInterval(timer); document.removeEventListener('nsbc-refresh', onSseRefresh); });
    return { calls, regs, loading, error, lastUpdated,
             showOrigDlg, origForm, origLoading, origMsg, origOk, submitOriginate };
  },
});

function fmtMos(v) {
  if (v == null || v < 0) return '-';
  return (v / 1000).toFixed(2);
}
function fmtLoss(v) {
  if (v == null || v === 0) return '-';
  return v + '%';
}

function stateBadge(v) {
  if (!v) return '<span class="text-gray-400">-</span>';
  const s = String(v).toLowerCase();
  const map = {
    calling:    ['bg-blue-100 text-blue-700',    'Calling'],
    ringing:    ['bg-yellow-100 text-yellow-700','Ringing'],
    early:      ['bg-amber-100 text-amber-700',  'Early'],
    confirmed:  ['bg-green-100 text-green-700',  'Confirmed'],
    active:     ['bg-green-100 text-green-700',  'Active'],
    held:       ['bg-purple-100 text-purple-700','Held'],
    terminated: ['bg-red-100 text-red-700',      'Terminated'],
    failed:     ['bg-red-100 text-red-700',      'Failed'],
    ended:      ['bg-gray-100 text-gray-600',    'Ended'],
  };
  const [cls, label] = map[s] || ['bg-gray-100 text-gray-600', v];
  return `<span class="inline-block px-2 py-0.5 rounded text-xs font-medium ${cls}">${label}</span>`;
}

// ===== Active calls / channels =====
app.component('channels-live', livePanel('', 'fa-solid fa-phone-volume',
  [
    { key: 'a_call_id',    label: 'A-Leg Call-ID' },
    { key: 'a_state',      label: t('ch.stateA'), fmt: stateBadge },
    { key: 'b_call_id',    label: 'B-Leg Call-ID' },
    { key: 'b_state',      label: t('ch.stateB'), fmt: stateBadge },
    { key: 'caller',       label: t('ch.caller') },
    { key: 'callee',       label: t('ch.callee') },
    { key: 'start_ms',     label: t('ch.start'), fmt: fmtTime },
    { key: 'answer_ms',    label: t('ch.answer'), fmt: fmtClock },
    { key: 'duration_ms',  label: t('ch.duration'), fmt: fmtDuration },
    { key: 'codec_a',      label: t('ch.codecA') },
    { key: 'codec_b',      label: t('ch.codecB') },
    { key: 'from_ip',      label: t('ch.fromIp') },
    { key: 'a_peer_ip',    label: t('ch.arip') },
    { key: 'b_peer_ip',    label: t('ch.brip') },
    { key: 'mos_x1000',    label: t('ch.mos'), fmt: fmtMos },
    { key: 'rtp_loss_pct', label: t('ch.loss'), fmt: fmtLoss },
    { key: 'rtp_jitter_ms',label: t('ch.jitter') },
  ], '/channels', {
    refreshMs: 5000,
    sortBy: 'start_ms', sortDir: 'desc',
    pageSize: 10,
    actionCol: (row) => {
      const callId = row.a_call_id || row.b_call_id || '';
      if (!callId) return '';
      return `<button onclick="nsbcHangup('${callId}')" class="px-2 py-1 text-xs text-red-600 border border-red-300 rounded hover:bg-red-50 hover:text-red-800"><i class="fa-solid fa-phone-slash"></i> ${ t('live.hangup') }</button>`;
    },
  }));

function fmtIso(s) {
  if (!s) return '-';
  const d = new Date(s);
  if (isNaN(d.getTime())) return '-';
  const pad = n => String(n).padStart(2, '0');
  return `${d.getFullYear()}-${pad(d.getMonth()+1)}-${pad(d.getDate())} ${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`;
}
function isoMs(s) {
  if (!s) return 0;
  const d = new Date(s);
  return isNaN(d.getTime()) ? 0 : d.getTime();
}

// ===== CDRs =====
app.component('cdrs-live', {
  template: `
    <div>
      <div class="flex items-center justify-between mb-4">
        <h2 class="text-lg font-semibold"><i class="fa-solid fa-file-lines"></i> ${ t('crud.cdrs.title') }</h2>
        <div class="text-xs text-gray-500">${ t('crud.live.totalFmt', { total }) } · 10s ${ t('crud.live.refresh') }</div>
      </div>
      <div v-if="loading" class="text-gray-400">${ t('generic.loading') }</div>
      <div v-else-if="error" class="text-red-500">⚠ {{ error }}</div>
      <div v-else-if="!rows.length" class="text-center text-gray-400 py-12">${ t('generic.noData') }</div>
      <table v-else class="w-full text-sm border-collapse">
        <thead><tr class="bg-gray-50 border-b">
          <th class="px-3 py-2 text-left font-medium text-gray-600">${ t('cdr.caller') }</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">${ t('cdr.callee') }</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">${ t('crud.cdrs.fromIp') }</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">${ t('crud.cdrs.codecA') }</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">${ t('crud.cdrs.codecB') }</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">${ t('cdr.callId') }</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">${ t('cdr.incoming') }</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">${ t('cdr.answer') }</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">${ t('crud.cdrs.ringTime') }</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">${ t('cdr.end') }</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">${ t('cdr.duration') }</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">${ t('cdr.billSec') }</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">${ t('cdr.cause') }</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">${ t('crud.cdrs.recCol') }</th>
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
                <a :href="getRecUrl(row)" download target="_blank" class="ml-2 text-blue-600 hover:underline" :title="t('cdr.download')">
                  <i class="fa-solid fa-download"></i>
                </a>
              </template>
              <template v-else>-</template>
            </td>
          </tr>
        </tbody>
      </table>
      <div v-if="total > pageSize" class="flex items-center justify-between mt-3 text-xs text-gray-500">
        <div>${ t('crud.live.totalPerPage', { total, pageSize }) }</div>
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

// ===== Metrics =====
app.component('metrics-live', {
  template: `
    <div>
      <h2 class="text-lg font-semibold mb-4"><i class="fa-solid fa-chart-line"></i> Prometheus</h2>
      <div v-if="loading" class="text-gray-400">${ t('generic.loading') }</div>
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

// ===== Registrations =====
app.component('registrations-live', {
  template: `
    <div>
      <div class="flex items-center justify-between mb-4">
        <h2 class="text-lg font-semibold"><i class="fa-solid fa-user-check"></i> ${ t('menu.registrations') }
          <span v-if="shown" class="text-sm text-gray-500">
            ({{ shown }} ${ t('reg.ext') }<span v-if="total > shown"> · {{ total }} endpoints</span>)
          </span>
        </h2>
        <div v-if="truncated" class="text-xs text-yellow-600">⚠ ${ t('crud.live.truncated') }</div>
      </div>
      <div v-if="loading" class="text-gray-400">${ t('generic.loading') }</div>
      <div v-else-if="error" class="text-red-500">⚠ {{ error }}</div>
      <div v-else-if="!rows.length" class="text-center text-gray-400 py-12">${ t('reg.noReg') }</div>
      <table v-else class="w-full text-sm border-collapse">
        <thead><tr class="bg-gray-50 border-b">
          <th class="px-3 py-2 text-left font-medium text-gray-600">${ t('reg.ext') }</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">${ t('reg.aor') }</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">${ t('crud.live.contactRaw') }</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">${ t('reg.nat') }</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">${ t('reg.received') }</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">${ t('reg.reachableContact') }</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">${ t('reg.transport') }</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">${ t('reg.registered') }</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">${ t('reg.remaining') }</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">${ t('reg.status') }</th>
        </tr></thead>
        <tbody>
          <tr v-for="(row, idx) in rows" :key="idx" class="border-b hover:bg-blue-50/50">
            <td class="px-3 py-2 font-mono text-sm font-semibold text-blue-700">{{ row.user || '-' }}</td>
            <td class="px-3 py-2 font-mono text-xs text-gray-600">{{ row.aor || '-' }}</td>
            <td class="px-3 py-2 font-mono text-xs text-gray-500 truncate max-w-[240px]" :title="row.contact">{{ row.contact || '-' }}</td>
            <td class="px-3 py-2 text-xs">
              <span v-if="row.nat_detected" class="inline-block px-1.5 py-0.5 rounded text-white bg-amber-500 font-semibold">NAT</span>
              <span v-else-if="row.received_ip && row.received_ip.length" class="text-green-600 font-medium">${ t('crud.live.noNat') }</span>
              <span v-else class="text-gray-400">-</span>
            </td>
            <td class="px-3 py-2 font-mono text-xs" :class="row.nat_detected ? 'text-amber-600' : 'text-gray-500'">
              <div v-if="row.received_ip || row.received_port">
                <div>{{ row.received_ip || '-' }}<span class="text-gray-400">:</span>{{ row.received_port || '-' }}</div>
              </div>
              <span v-else class="text-gray-400">-</span>
            </td>
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
              {{ row.expires_sec > 0 ? t('reg.online') : t('reg.soonExpire') }}
            </td>
          </tr>
        </tbody>
      </table>
    </div>
  `,
  setup() {
    const data = ref(null), loading = ref(false), error = ref(null);
    const rows = computed(() => {
      const list = (data.value && data.value.bindings) || [];
      const map = new Map();
      for (const b of list) {
        const key = b.user || b.aor || b.contact || '';
        const exist = map.get(key);
        if (!exist || (b.expires_sec ?? 0) > (exist.expires_sec ?? 0)) {
          map.set(key, b);
        }
      }
      return Array.from(map.values());
    });
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

// ===== Rate table =====
app.component('rates-live', {
  template: `
    <div>
      <div class="flex items-center justify-between mb-4">
        <h2 class="text-lg font-semibold"><i class="fa-solid fa-coins"></i> ${ t('live.ratesTitle') }</h2>
        <span class="text-xs text-gray-500">${ t('live.ratesUnit') }</span>
      </div>
      <div v-if="loading" class="text-gray-400">${ t('generic.loading') }</div>
      <div v-else-if="error" class="text-red-500">⚠ {{ error }}</div>
      <div v-else-if="!rows.length" class="text-center text-gray-400 py-12">${ t('generic.noData') }</div>
      <table v-else class="w-full text-sm border-collapse">
        <thead><tr class="bg-gray-50 border-b">
          <th class="px-3 py-2 text-left font-medium text-gray-600">${ t('crud.rates.prefix') }</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">${ t('generic.name') }</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">${ t('crud.rates.price') }</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">${ t('crud.rates.connectFee') }</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">${ t('crud.rates.billInc') }</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">${ t('crud.rates.minBill') }</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">${ t('generic.enabled') }</th>
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

// ===== Balances =====
app.component('balances-live', {
  template: `
    <div>
      <div class="flex items-center justify-between mb-4">
        <h2 class="text-lg font-semibold"><i class="fa-solid fa-wallet"></i> ${ t('live.balancesTitle') }</h2>
        <span class="text-xs text-gray-500">${ t('live.balancesUnit') }</span>
      </div>
      <div v-if="loading" class="text-gray-400">${ t('generic.loading') }</div>
      <div v-else-if="error" class="text-red-500">⚠ {{ error }}</div>
      <div v-else-if="!rows.length" class="text-center text-gray-400 py-12">${ t('generic.noData') }</div>
      <table v-else class="w-full text-sm border-collapse">
        <thead><tr class="bg-gray-50 border-b">
          <th class="px-3 py-2 text-left font-medium text-gray-600">${ t('crud.balances.account') }</th>
          <th class="px-3 py-2 text-right font-medium text-gray-600">${ t('crud.balances.balance') }</th>
          <th class="px-3 py-2 text-right font-medium text-gray-600">${ t('crud.balances.reserved') }</th>
          <th class="px-3 py-2 text-right font-medium text-gray-600">${ t('crud.balances.credit') }</th>
          <th class="px-3 py-2 text-right font-medium text-gray-600">${ t('crud.balances.available') }</th>
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

// ===== Transcode / Media Bridge =====
app.component('transcode-live', {
  template: `
    <div>
      <div class="flex items-center justify-between mb-4">
        <h2 class="text-lg font-semibold"><i class="fa-solid fa-microphone"></i> ${ t('live.transcodeTitle') }</h2>
        <span class="text-xs text-gray-500">${ t('crud.transcode.activeTotal') }: {{ total }}</span>
      </div>
      <div v-if="loading" class="text-gray-400">${ t('generic.loading') }</div>
      <div v-else-if="error" class="text-red-500">⚠ {{ error }}</div>
      <div v-else-if="!rows.length" class="text-center text-gray-400 py-12">${ t('crud.transcode.noSession') }</div>
      <table v-else class="w-full text-sm border-collapse">
        <thead><tr class="bg-gray-50 border-b">
          <th class="px-3 py-2 text-left font-medium text-gray-600">#</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">Call Key</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">${ t('crud.transcode.mode') }</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">A-RTP</th>
          <th class="px-3 py-2 text-left font-medium text-gray-600">B-RTP</th>
          <th class="px-3 py-2 text-right font-medium text-gray-600">${ t('crud.transcode.fwdPkts') }</th>
          <th class="px-3 py-2 text-right font-medium text-gray-600">${ t('crud.transcode.dropPkts') }</th>
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
