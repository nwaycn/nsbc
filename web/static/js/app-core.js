// =============================================================================
// NSBC Web — 核心层
// 包含：API wrapper、结构化编辑器、Vue App 创建、CRUD/Live 工厂
// 加载顺序：必须在 vendor/*.js 之后，pages/*.js 之前
// =============================================================================
/* global Vue, ElementPlus, ElementPlusIconsVue */

const { createApp, ref, reactive, computed, onMounted, onBeforeUnmount, watch } = Vue;

// ===== API wrapper =====
const API = {
  base: '/api/v1',
  token: () => localStorage.getItem('nsbc_token'),
  tenantId: () => parseInt(localStorage.getItem('nsbc_tenant_id') || '1', 10) || 1,
  headers(hasBody) {
    const h = { 'x-nsbc-api-version': '1' };
    if (hasBody) h['Content-Type'] = 'application/json';
    if (this.token()) h['Authorization'] = 'Bearer ' + this.token();
    return h;
  },
  async req(method, path, body) {
    const hasBody = body !== undefined;
    const opts = { method, headers: this.headers(hasBody) };
    if (hasBody) opts.body = JSON.stringify(body);
    // GET 请求自动拼 tenant_id（tenant_id=0 或空表示不过滤）
    // 排除 /tenants 本身（租户元数据源，不能被 tenant_id 过滤）
    let finalPath = path;
    if (!hasBody && method === 'GET' && !path.startsWith('/tenants')) {
      const tid = this.tenantId();
      if (tid > 0) {
        const sep = path.includes('?') ? '&' : '?';
        finalPath = path + sep + 'tenant_id=' + tid;
      }
    }
    const res = await fetch(this.base + finalPath, opts);
    if (res.status === 401) {
      // === Session 缓存：自动用 refresh_token 续期 ===
      const refreshToken = localStorage.getItem('nsbc_refresh_token');
      const isRefreshReq = path === '/auth/refresh';
      if (refreshToken && !isRefreshReq && !this._refreshing) {
        try {
          this._refreshing = true;
          const refreshRes = await fetch(this.base + '/auth/refresh', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ refresh_token: refreshToken }),
          });
          if (refreshRes.ok) {
            const rd = await refreshRes.json();
            localStorage.setItem('nsbc_token', rd.access_token);
            if (rd.refresh_token) localStorage.setItem('nsbc_refresh_token', rd.refresh_token);
            try { window.__auth && (window.__auth.token = rd.access_token); } catch(_) {}
            // 重放原请求（token 已更新）
            const retryRes = await fetch(this.base + path, opts);
            if (retryRes.status === 401) {
              // 还是 401 → 真没救了
              localStorage.clear();
              location.hash = '#/login';
              throw new Error('未授权');
            }
            const retryText = await retryRes.text();
            if (!retryRes.ok) throw new Error(retryText || retryRes.statusText);
            return retryText ? JSON.parse(retryText) : null;
          }
        } catch (refreshErr) { /* refresh 失败走下面清 localStorage */ }
        finally { this._refreshing = false; }
      }
      // refresh 也失败 → 清 session 跳登录
      localStorage.clear();
      try { window.__auth && (window.__auth.token = null, window.__auth.user = null, window.__auth.role = null); } catch(_) {}
      location.hash = '#/login';
      throw new Error('未授权');
    }
    const text = await res.text();
    const data = text ? JSON.parse(text) : null;
    if (!res.ok) throw new Error((data && data.detail) || res.statusText);
    return data;
  },
  get:    (p) => API.req('GET', p),
  post:   (p, b) => API.req('POST', p, b),
  put:    (p, b) => API.req('PUT', p, b),
  delete: (p) => API.req('DELETE', p),
};

// ===== 结构化编辑器 =====

// 1) 时间段（time_ranges → JSON 数组）
const TimeRangeEditor = {
  props: ['modelValue'],
  template: `
    <div class="struct-editor">
      <div class="struct-header">
        <span class="font-semibold">时间段规则（{{ rows.length }} 条）</span>
        <span class="text-gray-400 text-[11px]">days: 1-5 / 1,3,5 / 0-6</span>
      </div>
      <div v-if="!rows.length" class="px-3 py-6 text-center text-gray-400 text-sm">暂无时间段</div>
      <div v-for="(row, idx) in rows" :key="idx" class="struct-row">
        <input v-model="row.days" placeholder="1-5" class="w-28">
        <span class="text-gray-400 text-xs">起</span>
        <input v-model="row.from" placeholder="09:00" class="w-24">
        <span class="text-gray-400 text-xs">止</span>
        <input v-model="row.to" placeholder="18:00" class="w-24">
        <i class="fa-solid fa-trash struct-del ml-auto" @click="rows.splice(idx,1)"></i>
      </div>
      <div class="struct-add" @click="rows.push({days:'1-5',from:'09:00',to:'18:00'})"><i class="fa-solid fa-plus mr-1"></i>添加时间段</div>
    </div>
  `,
  setup(props, { emit }) {
    const rows = ref([]);
    let _s = false;
    function syncFrom() { _s = true; rows.value = JSON.parse(props.modelValue || '[]'); if (!rows.value.length) rows.value.push({days:'1-5',from:'09:00',to:'18:00'}); _s = false; }
    function syncTo()  { if (_s) return; emit('update:modelValue', JSON.stringify(rows.value)); }
    watch(() => props.modelValue, syncFrom);
    watch(rows, syncTo, { deep: true });
    syncFrom();
    return { rows };
  },
};

// 2) 队列成员（members_json → JSON 数组）
const QueueMembersEditor = {
  props: ['modelValue'],
  template: `
    <div class="struct-editor">
      <div class="struct-header">
        <span class="font-semibold">队列成员（{{ rows.length }} 人）</span>
        <span class="text-gray-400 text-[11px]">extension / weight(1-100) / enabled</span>
      </div>
      <div v-if="!rows.length" class="px-3 py-6 text-center text-gray-400 text-sm">暂无成员</div>
      <div v-for="(row, idx) in rows" :key="idx" class="struct-row">
        <input v-model="row.extension" placeholder="分机号 2000" class="w-32">
        <input v-model.number="row.weight" type="number" min="1" max="100" placeholder="权重" class="w-20">
        <label class="flex items-center gap-1 text-xs">
          <input type="checkbox" v-model="row.enabled"> 启用
        </label>
        <i class="fa-solid fa-trash struct-del ml-auto" @click="rows.splice(idx,1)"></i>
      </div>
      <div class="struct-add" @click="rows.push({extension:'',weight:10,enabled:true})"><i class="fa-solid fa-plus mr-1"></i>添加成员</div>
    </div>
  `,
  setup(props, { emit }) {
    const rows = ref([]);
    let _s = false;
    function syncFrom() { _s = true; rows.value = JSON.parse(props.modelValue || '[]'); if (!rows.value.length) rows.value.push({extension:'',weight:10,enabled:true}); _s = false; }
    function syncTo()  { if (_s) return; emit('update:modelValue', JSON.stringify(rows.value)); }
    watch(() => props.modelValue, syncFrom);
    watch(rows, syncTo, { deep: true });
    syncFrom();
    return { rows };
  },
};

// 3) IVR 按键选项（options_json → JSON 数组）
const IvrOptionsEditor = {
  props: ['modelValue'],
  template: `
    <div class="struct-editor">
      <div class="struct-header">
        <span class="font-semibold">按键选项（{{ rows.length }} 项）</span>
        <span class="text-gray-400 text-[11px]">key=0-9/*/#  target=extension/queue/ivr/vm</span>
      </div>
      <div v-if="!rows.length" class="px-3 py-6 text-center text-gray-400 text-sm">暂无选项</div>
      <div v-for="(row, idx) in rows" :key="idx" class="struct-row">
        <select v-model="row.key" class="w-20">
          <option v-for="k in ['0','1','2','3','4','5','6','7','8','9','*','#']" :key="k" :value="k">{{ k }}</option>
        </select>
        <span class="text-gray-400">→</span>
        <input v-model="row.target" placeholder="目标" class="flex-1">
        <i class="fa-solid fa-trash struct-del ml-2" @click="rows.splice(idx,1)"></i>
      </div>
      <div class="struct-add" @click="rows.push({key:'1',target:''})"><i class="fa-solid fa-plus mr-1"></i>添加按键选项</div>
    </div>
  `,
  setup(props, { emit }) {
    const rows = ref([]);
    let _s = false;
    function syncFrom() { _s = true; rows.value = JSON.parse(props.modelValue || '[]'); if (!rows.value.length) rows.value.push({key:'1',target:''}); _s = false; }
    function syncTo()  { if (_s) return; emit('update:modelValue', JSON.stringify(rows.value)); }
    watch(() => props.modelValue, syncFrom);
    watch(rows, syncTo, { deep: true });
    syncFrom();
    return { rows };
  },
};

// 4) Trunk Codec Set（逗号分隔字符串 "PCMA,PCMU,G729"）
const TRUNK_CODECS = ['PCMA','PCMU','G711U','G711A','G729','G722','G722.2','ILBC','OPUS','SPEEX','GSM','AMR','AMR-WB'];
const TrunkCodecEditor = {
  props: ['modelValue'],
  template: `
    <div class="struct-editor">
      <div class="struct-header">
        <span class="font-semibold">可用编码（{{ selected.length }} 个）</span>
        <span class="text-gray-400 text-[11px]">勾选自动生成逗号串</span>
      </div>
      <div class="codec-grid">
        <label v-for="c in TRUNK_CODECS" :key="c">
          <input type="checkbox" :value="c" v-model="selected"> {{ c }}
        </label>
      </div>
      <div class="struct-header border-t bg-white">
        <span class="font-mono text-xs text-gray-600 truncate">{{ modelValue || '未选择' }}</span>
        <button class="text-xs text-blue-600 hover:text-blue-800" @click="selectAll(true)">全选</button>
      </div>
    </div>
  `,
  setup(props, { emit }) {
    const selected = ref([]);
    let _s = false;
    function syncFrom() { _s = true; selected.value = (props.modelValue || '').split(',').map(s=>s.trim()).filter(Boolean); _s = false; }
    function syncTo()  { if (_s) return; emit('update:modelValue', selected.value.join(',')); }
    function selectAll(v) { selected.value = v ? [...TRUNK_CODECS] : []; }
    watch(() => props.modelValue, syncFrom);
    watch(selected, syncTo, { deep: true });
    syncFrom();
    return { selected, TRUNK_CODECS, selectAll };
  },
};

// 5) Dialplan Action（action_json → JSON 对象，字段名与 C 侧 sip_hdr_rewrite_t 完全对齐）
const DialplanActionEditor = {
  props: ['modelValue'],
  template: `
    <div class="struct-editor">
      <div class="struct-header">
        <span class="font-semibold">拨号动作（dp_rule 附加字段）</span>
        <span class="text-gray-400 text-[11px]">号码变换 / SIP 头改写 / 录音控制</span>
      </div>
      <div class="px-3 py-3 space-y-2.5 bg-white max-h-[600px] overflow-y-auto">
        <!-- 号码变换 -->
        <div class="text-[11px] text-gray-500 font-semibold pt-0.5">─ 号码变换 ─</div>
        <div class="struct-row">
          <span class="struct-label w-40 shrink-0">clip 强制 CLIP</span>
          <input v-model="action.clip" placeholder="空=不改写，如 442071234567">
        </div>
        <div class="struct-row">
          <label class="inline-flex items-center gap-1.5">
            <input type="checkbox" v-model="action.immediate" class="flex-none">
            <span class="text-gray-600 text-xs">immediate — DigitMap 收齐即匹配</span>
          </label>
        </div>
        <div class="struct-row">
          <span class="struct-label w-40 shrink-0">perm 出局权限</span>
          <input v-model.number="action.perm" type="number" min="0" max="255" class="w-20">
          <span class="hint">(0-255)</span>
        </div>

        <!-- From 头 -->
        <div class="text-[11px] text-gray-500 font-semibold pt-2">─ From 头（从低到高优先级） ─</div>
        <div class="struct-row">
          <span class="struct-label w-40 shrink-0">sip_from_display 显示名</span>
          <input v-model="action.sip_from_display" placeholder='如 "Sales Team"'>
          <span class="hint">组件级</span>
        </div>
        <div class="struct-row">
          <span class="struct-label w-40 shrink-0">sip_from_uri URI 部分</span>
          <input v-model="action.sip_from_uri" placeholder="如 sip:alice@nsbc.local">
          <span class="hint">组件级</span>
        </div>
        <div class="struct-row">
          <span class="struct-label w-40 shrink-0">sip_invite_from 完整From</span>
          <input v-model="action.sip_invite_from" placeholder='完整 From 值（最高优先级）'>
          <span class="hint">覆盖全部</span>
        </div>
        <div class="struct-row">
          <span class="struct-label w-40 shrink-0">sip_invite_from_uri</span>
          <input v-model="action.sip_invite_from_uri" placeholder='纯 URI 覆盖，如 sip:alice@nsbc.local'>
          <span class="hint">FS 对齐</span>
        </div>
        <div class="struct-row">
          <span class="struct-label w-40 shrink-0">sip_invite_from_params</span>
          <input v-model="action.sip_invite_from_params" placeholder='如 transport=tls'>
          <span class="hint">From params</span>
        </div>
        <div class="struct-row">
          <span class="struct-label w-40 shrink-0">orig_caller_id_number</span>
          <input v-model="action.origination_caller_id_number" placeholder='FreeSWITCH 精细化主叫 — 只填 user part'>
          <span class="hint">FS 对齐</span>
        </div>
        <div class="struct-row">
          <span class="struct-label w-40 shrink-0">orig_caller_id_name</span>
          <input v-model="action.origination_caller_id_name" placeholder='Display name（空=同 number）'>
          <span class="hint">FS 对齐</span>
        </div>

        <!-- To / R-URI / Contact -->
        <div class="text-[11px] text-gray-500 font-semibold pt-2">─ To / R-URI / Contact ─</div>
        <div class="struct-row">
          <span class="struct-label w-40 shrink-0">sip_invite_to_uri To覆盖</span>
          <input v-model="action.sip_invite_to_uri" placeholder="空=透传，如 sip:bob@other.com">
        </div>
        <div class="struct-row">
          <span class="struct-label w-40 shrink-0">sip_invite_to_params</span>
          <input v-model="action.sip_invite_to_params" placeholder="如 user=phone">
          <span class="hint">To params</span>
        </div>
        <div class="struct-row">
          <span class="struct-label w-40 shrink-0">sip_invite_req_uri R-URI</span>
          <input v-model="action.sip_invite_req_uri" placeholder="完整 Request-URI 覆盖">
        </div>
        <div class="struct-row">
          <span class="struct-label w-40 shrink-0">sip_invite_tel_params</span>
          <input v-model="action.sip_invite_tel_params" placeholder="tel: URI params">
          <span class="hint">tel: 追加</span>
        </div>
        <div class="struct-row">
          <span class="struct-label w-40 shrink-0">sip_invite_contact_params</span>
          <input v-model="action.sip_invite_contact_params" placeholder="如 transport=tcp">
          <span class="hint">Contact params</span>
        </div>

        <!-- Domain override -->
        <div class="text-[11px] text-gray-500 font-semibold pt-2">─ Domain override ─</div>
        <div class="struct-row">
          <span class="struct-label w-40 shrink-0">sip_invite_domain 强制域</span>
          <input v-model="action.sip_invite_domain" placeholder="如 sip.example.com">
          <span class="hint">全局 host 替换</span>
        </div>

        <!-- Extra headers -->
        <div class="text-[11px] text-gray-500 font-semibold pt-2">─ 额外 SIP 头（注入 extra 块） ─</div>
        <div class="struct-row">
          <span class="struct-label w-40 shrink-0">sip_invite_pid_params</span>
          <input v-model="action.sip_invite_pid_params" placeholder="如 P-Asserted-Identity: <sip:+8613800138000@ims.mnc001.mcc460.3gppnetwork.org>">
          <span class="hint">完整头行或简写</span>
        </div>
        <div class="struct-row flex-wrap items-start">
          <span class="struct-label w-40 shrink-0 self-stretch">sip_invite_params 多块</span>
          <div class="flex-1 min-w-0">
            <textarea v-model="action.sip_invite_params" rows="4"
              placeholder="每行一个完整头（C 侧自动拼接 \r\n），例如：&#10;P-Access-Network-Info: E-UTRAN-TDD; utran-cell-id-3gpp=0001001&#10;P-Called-Party-ID: <sip:+8613900139000@ims.mnc001.mcc460.3gppnetwork.org>&#10;Privacy: id"
              class="w-full text-xs"></textarea>
            <div class="flex items-center gap-2 mt-1">
              <span class="text-[11px] text-gray-400">快速插入：</span>
              <button type="button" class="inline-block text-[11px] px-2 py-0.5 bg-slate-100 border border-slate-300 rounded hover:bg-slate-200" @click="tpl3gpp">3GPP IMS（PAI+PPID+PANI+PCPI）</button>
              <button type="button" class="inline-block text-[11px] px-2 py-0.5 bg-slate-100 border border-slate-300 rounded hover:bg-slate-200" @click="tplClir">运营商 CLIR（Privacy+PAI）</button>
            </div>
          </div>
          <span class="hint">extra 头块</span>
        </div>
      </div>
      <div class="struct-header border-t bg-white">
        <span class="font-mono text-[11px] text-gray-500 truncate">JSON: {{ preview }}</span>
      </div>
    </div>
  `,
  setup(props, { emit }) {
    // 所有 action_json 字段与 C 侧 sip_hdr_rewrite_t 同名
    const action = reactive({
      // 号码变换
      dst_override: '', clip: '', perm: 0, immediate: false,
      // From 头
      sip_invite_from: '', sip_invite_from_uri: '', sip_from_uri: '', sip_from_display: '', sip_invite_from_params: '',
      // FS 精细化主叫（只填 user part，host 自动走 trunk.from_domain）
      origination_caller_id_number: '', origination_caller_id_name: '',
      // To / R-URI / Contact
      sip_invite_to_uri: '', sip_invite_to_params: '',
      sip_invite_req_uri: '', sip_invite_tel_params: '',
      sip_invite_contact_params: '',
      // Domain
      sip_invite_domain: '',
      // Extra
      sip_invite_params: '', sip_invite_pid_params: '',
      // legacy 别名（兼容旧 JSON）
      clip_from: '',
    });
    let _s = false;
    const preview = computed(() => JSON.stringify(action));
    function syncFrom() {
      _s = true;
      try {
        const v = JSON.parse(props.modelValue || '{}');
        // 号码变换
        action.dst_override = v.dst_override ?? '';
        action.clip = v.clip ?? '';
        action.perm = Number(v.perm ?? 0);
        action.immediate = !!v.immediate;
        // From 头
        action.sip_invite_from = v.sip_invite_from ?? '';
        action.sip_invite_from_uri = v.sip_invite_from_uri ?? '';
        action.sip_from_uri = v.sip_from_uri ?? '';
        action.sip_from_display = v.sip_from_display ?? '';
        action.sip_invite_from_params = v.sip_invite_from_params ?? '';
        // FS 精细化主叫
        action.origination_caller_id_number = v.origination_caller_id_number ?? '';
        action.origination_caller_id_name = v.origination_caller_id_name ?? '';
        // To / R-URI / Contact
        action.sip_invite_to_uri = v.sip_invite_to_uri ?? '';
        action.sip_invite_to_params = v.sip_invite_to_params ?? '';
        action.sip_invite_req_uri = v.sip_invite_req_uri ?? '';
        action.sip_invite_tel_params = v.sip_invite_tel_params ?? '';
        action.sip_invite_contact_params = v.sip_invite_contact_params ?? '';
        // Domain
        action.sip_invite_domain = v.sip_invite_domain ?? '';
        // Extra
        action.sip_invite_params = v.sip_invite_params ?? '';
        action.sip_invite_pid_params = v.sip_invite_pid_params ?? '';
        // legacy
        action.clip_from = v.clip_from ?? '';
      } catch {}
      _s = false;
    }
    function syncTo()  { if (_s) return; emit('update:modelValue', JSON.stringify(action)); }
    watch(() => props.modelValue, syncFrom);
    watch(action, syncTo, { deep: true });
    syncFrom();
    // —— 快速模板注入 ——
    // 3GPP IMS 运营商常用头：PAI（主叫身份）、PPID（首选主叫）、
    // PANI（接入网类型）、PCPI（被叫 E.164 标识）、Privacy（CLIR）
    function tpl3gpp() {
      action.sip_invite_pid_params = 'P-Asserted-Identity: <sip:+8613800138000@ims.mnc001.mcc460.3gppnetwork.org>';
      action.sip_invite_params =
`P-Preferred-Identity: <sip:+8613800138000@ims.mnc001.mcc460.3gppnetwork.org>
P-Access-Network-Info: E-UTRAN-TDD; utran-cell-id-3gpp=0001001
P-Called-Party-ID: <sip:+8613900139000@ims.mnc001.mcc460.3gppnetwork.org>
Privacy: id`;
    }
    // 运营商 CLIR：主叫号码隐藏（Privacy: id 触发 B 腿 From anonymize）
    // + 带 P-Asserted-Identity 让对端网络仍能拿到真实主叫用于计费/路由
    function tplClir() {
      action.sip_invite_pid_params = 'P-Asserted-Identity: <sip:+8613800138000@ims.mnc001.mcc460.3gppnetwork.org>';
      action.sip_invite_params =
`Privacy: id`;
    }
    return { action, preview, tpl3gpp, tplClir };
  },
};

// 6) AppChainEditor —— Application 链（REQ-DP-APP-001）
// C 侧 dp_json_parse_one 解析 top-level "app_chain": [{"app":"playback","data":"welcome.wav"}, ...]
const AppChainEditor = {
  props: ['modelValue'],
  template: `
    <div class="app-chain-editor">
      <div class="struct-header">
        <span class="font-semibold">拨号动作链（Application Chain）</span>
        <span class="text-gray-400 text-[11px]">按顺序执行：pre_answer → playback → sleep → answer …</span>
      </div>
      <div class="px-3 py-3 bg-white space-y-2">
        <!-- 添加动作 -->
        <div class="flex items-center gap-2">
          <el-select v-model="nextApp" placeholder="选择要添加的应用…" style="flex:1" size="small" filterable>
            <el-option-group v-for="g in appGroups" :key="g.label" :label="g.label">
              <el-option v-for="a in g.apps" :key="a.name" :value="a.name" :label="a.name + ' — ' + a.desc">
                <div class="flex justify-between items-center w-full">
                  <span class="font-mono text-xs">{{ a.name }}</span>
                  <span class="text-gray-400 text-[11px]">{{ a.desc }}</span>
                </div>
              </el-option>
            </el-option-group>
          </el-select>
          <el-button size="small" type="primary" :disabled="!nextApp" @click="addApp">
            <el-icon><Plus /></el-icon>添加
          </el-button>
        </div>

        <!-- 空状态 -->
        <div v-if="!chain.length" class="text-center py-4 text-gray-400 text-xs">
          暂无拨号动作。上方选一个 app 添加进来，按顺序执行。
        </div>

        <!-- 链列表 -->
        <div v-for="(item, idx) in chain" :key="idx"
             class="flex items-start gap-2 p-2 bg-slate-50 border border-slate-200 rounded">
          <span class="flex-none w-6 h-6 flex items-center justify-center bg-blue-500 text-white text-xs rounded-full font-mono mt-0.5">{{ idx + 1 }}</span>
          <div class="flex-1 min-w-0 space-y-1">
            <div class="flex items-center gap-2">
              <el-tag size="small" type="info" effect="plain" class="font-mono">{{ item.app }}</el-tag>
              <span class="text-[11px] text-slate-500">{{ appHint(item.app) }}</span>
            </div>
            <el-input v-if="needsData(item.app)" v-model="item.data" size="small"
                      :placeholder="dataPlaceholder(item.app)" clearable></el-input>
          </div>
          <div class="flex-none flex flex-col gap-0.5">
            <el-button link size="small" :disabled="idx === 0" @click="moveUp(idx)">
              <el-icon><ArrowUp /></el-icon>
            </el-button>
            <el-button link size="small" :disabled="idx === chain.length - 1" @click="moveDown(idx)">
              <el-icon><ArrowDown /></el-icon>
            </el-button>
            <el-button link size="small" type="danger" @click="removeAt(idx)">
              <el-icon><Delete /></el-icon>
            </el-button>
          </div>
        </div>
      </div>
      <div class="struct-header border-t bg-white">
        <span class="font-mono text-[11px] text-gray-500 truncate">
          JSON: {{ chain.length ? preview : '[] (空链，C 侧跳过)' }}
        </span>
      </div>
    </div>
  `,
  setup(props, { emit }) {
    const { ref, computed, watch } = Vue;
    const { Plus, ArrowUp, ArrowDown, Delete } = ElementPlusIconsVue || {};

    /* C 侧 app_registry.c builtin_apps[] 注册的 17 个内建 apps（必须对齐） */
    const APP_DESCS = {
      answer:              '应答来电（发送 200 OK）',
      pre_answer:          'Early Media 预应答（发 183，不发 200 OK）',
      bridge:              '桥接到 sofia/gateway/<gw>/<ext>',
      playback:            '播放单个音频文件',
      post_playback:       '接通后放音（阶段区分用）',
      record:              '录制当前通话音频',
      transfer:            '转移当前通话（REFER 或 302）',
      set:                 '设置通道变量 var=value',
      conference:          '加入会议 <room>@<profile>',
      play_and_get_digits: '播放提示音并收集 DTMF',
      sleep:               '暂停执行（毫秒）',
      park:                '保持通话等待取回（hold + MoH）',
      att_xfer:            '出席/咨询转移',
      gentones:            '生成特定音调序列',
      endless_playback:    '循环播放（uuid_break 可中断）',
      uuid_break:          '打断 playback/sleep',
      read:                'DTMF 收号',
    };

    /* 按执行阶段分组（引导用户构建合理顺序） */
    const appGroups = computed(() => [
      { label: '— Early Media / 应答阶段 —', apps: ['pre_answer', 'playback', 'post_playback', 'sleep', 'answer', 'endless_playback', 'read', 'play_and_get_digits'] },
      { label: '— 路由/桥接阶段 —',        apps: ['bridge', 'transfer', 'att_xfer', 'conference', 'park'] },
      { label: '— 变量/控制 —',            apps: ['set', 'uuid_break', 'gentones'] },
      { label: '— 媒体/录音 —',            apps: ['record'] },
    ].map(g => ({
      label: g.label,
      apps: g.apps.map(n => ({ name: n, desc: APP_DESCS[n] || '' })),
    })));

    const nextApp = ref('');
    const chain = ref([]);

    function appHint(name)  { return APP_DESCS[name] || ''; }
    function needsData(name){ return ['playback','post_playback','endless_playback','sleep','bridge','transfer','conference','set','gentones','record','play_and_get_digits','read'].includes(name); }
    function dataPlaceholder(name) {
      const H = {
        playback:             'welcome.wav 或 /path/to/file.wav',
        post_playback:        'announce.wav',
        endless_playback:     'hold_music.wav',
        sleep:                '1500 (毫秒)',
        bridge:               'sofia/gateway/my_trunk/8613800138000',
        transfer:             'sip:support@other.com',
        conference:           'my_room@default',
        set:                  'var_name=value',
        gentones:             '1004,1500,800',
        record:               '/var/recording/callid.wav',
        play_and_get_digits:  'prompt.wav 3 5000',
        read:                 '5 10000',
      };
      return H[name] || '(无 data)';
    }

    function addApp() {
      if (!nextApp.value) return;
      chain.value.push({ app: nextApp.value, data: '' });
      nextApp.value = '';
    }
    function removeAt(i)  { chain.value.splice(i, 1); }
    function moveUp(i)   { if (i > 0) { const c = chain.value; [c[i-1], c[i]] = [c[i], c[i-1]]; } }
    function moveDown(i) { const c = chain.value; if (i < c.length - 1) { [c[i+1], c[i]] = [c[i], c[i+1]]; } }

    const preview = computed(() => JSON.stringify(chain.value));

    let _s = false;
    function syncFrom() {
      _s = true;
      try {
        const v = JSON.parse(props.modelValue || '[]');
        if (Array.isArray(v))
          chain.value = v
            .filter(x => x && typeof x.app === 'string' && x.app.length > 0)
            .map(x => ({ app: x.app, data: x.data || '' }));
        else
          chain.value = [];
      } catch { chain.value = []; }
      _s = false;
    }
    function syncTo() {
      if (_s) return;
      const clean = chain.value
        .filter(x => x && x.app)
        .map(x => x.data ? { app: x.app, data: x.data } : { app: x.app });
      emit('update:modelValue', JSON.stringify(clean));
    }

    watch(() => props.modelValue, syncFrom);
    watch(chain, syncTo, { deep: true });
    syncFrom();

    return { chain, nextApp, appGroups, addApp, removeAt, moveUp, moveDown,
             appHint, needsData, dataPlaceholder, preview };
  },
};

// 7) HA 节点列表（node_list_json → JSON 数组）
const HaNodeListEditor = {
  props: ['modelValue'],
  template: `
    <div class="struct-editor">
      <div class="struct-header">
        <span class="font-semibold">集群节点（{{ rows.length }} 个）</span>
        <span class="text-gray-400 text-[11px]">node_id/host/port/role(master|slave|witness)</span>
      </div>
      <div v-if="!rows.length" class="px-3 py-6 text-center text-gray-400 text-sm">暂无节点</div>
      <div v-for="(row, idx) in rows" :key="idx" class="struct-row">
        <input v-model="row.node_id" placeholder="node-1" class="w-28">
        <input v-model="row.host" placeholder="10.0.0.25" class="w-36">
        <input v-model.number="row.port" type="number" min="1" max="65535" placeholder="8080" class="w-20">
        <select v-model="row.role" class="w-24">
          <option value="master">master</option>
          <option value="slave">slave</option>
          <option value="witness">witness</option>
        </select>
        <i class="fa-solid fa-trash struct-del ml-auto" @click="rows.splice(idx,1)"></i>
      </div>
      <div class="struct-add" @click="rows.push({node_id:'',host:'',port:8080,role:'slave'})"><i class="fa-solid fa-plus mr-1"></i>添加节点</div>
    </div>
  `,
  setup(props, { emit }) {
    const rows = ref([]);
    let _s = false;
    function syncFrom() {
      _s = true;
      const v = JSON.parse(props.modelValue || '[]');
      rows.value = Array.isArray(v) ? v : [];
      if (!rows.value.length) rows.value.push({node_id:'',host:'',port:8080,role:'slave'});
      _s = false;
    }
    function syncTo() { if (_s) return; emit('update:modelValue', JSON.stringify(rows.value)); }
    watch(() => props.modelValue, syncFrom);
    watch(rows, syncTo, { deep: true });
    syncFrom();
    return { rows };
  },
};

// ===== App =====
const app = createApp({
  setup() {
    const auth = reactive({
      token: localStorage.getItem('nsbc_token') || null,
      user: localStorage.getItem('nsbc_user') || '',
      role: localStorage.getItem('nsbc_role') || '',
      loading: false, error: null,
    });
    const loginForm = reactive({ username: 'admin', password: 'admin', captcha_code: '' });
    const captchaSvg = ref(null);
    const captchaId = ref(null);
    const clientIp = ref('---');
    const route = ref(location.hash.replace('#', '') || '/realtime');

    // ===== 多租户全局状态 =====
    const tenantId = ref(parseInt(localStorage.getItem('nsbc_tenant_id') || '1', 10) || 1);
    const tenantOptions = ref([]);  // [{value, label}]
    const tenantLoading = ref(false);
    function changeTenant(id) {
      tenantId.value = id;
      localStorage.setItem('nsbc_tenant_id', id.toString());
      // 同步写入 SIP realm（WebRTC 页面和独立 webrtc.html 都会读这个）
      const t = tenantOptions.value.find(x => x.value === id);
      if (t && t.label) {
        const tn = t.label.replace(/\s*\(禁用\)\s*$/, '').trim();
        try { localStorage.setItem('nsbc_sip_domain', tn); } catch(_) {}
      }
      // 广播给所有 crudTable 重新加载
      window.dispatchEvent(new CustomEvent('nsbc-tenant-changed', { detail: id }));
    }
    async function fetchTenants() {
      tenantLoading.value = true;
      try {
        const r = await API.get('/tenants?limit=500&offset=0');
        tenantOptions.value = (r.items || []).map(t => ({
          value: t.id,
          label: t.tenant_name + (t.enabled ? '' : ' (禁用)'),
        }));
      } catch (e) { tenantOptions.value = []; }
      finally { tenantLoading.value = false; }
    }
    // 暴露给外部（pages-crud.js crudTable 工厂需要）
    window.__tenantId = tenantId;

    const menuItems = [
      // 实时监控（置于顶部，运维高频）
      { path: '/realtime',      label: '实时总览', icon: 'fa-solid fa-gauge-high' },
      { path: '/channels',     label: '在途呼叫', icon: 'fa-solid fa-phone-volume' },
      { path: '/cdrs',          label: '通话记录', icon: 'fa-solid fa-file-lines' },
      { path: '/metrics',       label: '性能指标', icon: 'fa-solid fa-chart-line' },
      { path: '/registrations', label: '注册状态', icon: 'fa-solid fa-user-check' },
      { path: '/security-center', label: '安全中心', icon: 'fa-solid fa-shield-halved' },
      // 业务数据
      { path: '/tenants',      label: '租户（SIP Realm）', icon: 'fa-solid fa-building' },
      { path: '/extensions',   label: '分机',     icon: 'fa-solid fa-phone' },
      { path: '/sip-profiles', label: 'SIP Profile', icon: 'fa-solid fa-network-wired' },
      { path: '/gateways',     label: '网关',     icon: 'fa-solid fa-tower-broadcast' },
      { path: '/trunks',       label: '中继',     icon: 'fa-solid fa-signal' },
      // 路由组已移到后端核心侧（nsbcd route_group_t），Web 侧暂不暴露
      { path: '/dialplan-rules', label: '拨号规则', icon: 'fa-solid fa-code-branch' },
      { path: '/time-groups',  label: '时间策略', icon: 'fa-solid fa-clock' },
      { path: '/queues',       label: '分机队列', icon: 'fa-solid fa-people-group' },
      { path: '/ivr-menus',    label: 'IVR 菜单', icon: 'fa-solid fa-circle-question' },
      // 计费与媒体
      { path: '/rates',        label: '费率管理', icon: 'fa-solid fa-coins' },
      { path: '/balances',     label: '账户余额', icon: 'fa-solid fa-wallet' },
      { path: '/transcode',    label: '媒体会话', icon: 'fa-solid fa-microphone' },
      // 系统
      { path: '/ha-groups',    label: '高可用组', icon: 'fa-solid fa-server' },
      // 工具
      { path: '/webrtc',       label: 'WebRTC 测试', icon: 'fa-solid fa-video' },
    ];

    window.addEventListener('hashchange', () => { route.value = location.hash.replace('#', '') || '/extensions'; });

    async function refreshCaptcha() {
      try {
        const r = await API.get('/auth/captcha');
        captchaId.value = r.captcha_id;
        captchaSvg.value = r.svg;
      } catch (e) { /* 验证码加载失败不阻塞登录 */ }
    }

    async function login() {
      auth.loading = true; auth.error = null;
      try {
        const body = { username: loginForm.username, password: loginForm.password };
        if (captchaId.value && loginForm.captcha_code) {
          body.captcha_id = captchaId.value;
          body.captcha_code = loginForm.captcha_code;
        }
        const r = await API.post('/auth/login', body);
        auth.token = r.access_token;
        auth.user  = loginForm.username;
        auth.role  = r.role || 'admin';
        localStorage.setItem('nsbc_token', r.access_token);
        localStorage.setItem('nsbc_user',  auth.user);
        localStorage.setItem('nsbc_role',  auth.role);
        if (r.refresh_token) localStorage.setItem('nsbc_refresh_token', r.refresh_token);
        // 登录成功后拉取租户列表
        fetchTenants();
        location.hash = '#/realtime';
      } catch (e) {
        auth.error = e.message;
        loginForm.captcha_code = '';
        refreshCaptcha();
      }
      finally { auth.loading = false; }
    }
    function logout() { localStorage.clear(); auth.token = null; location.hash = '#/login'; }

    refreshCaptcha();
    window.__auth = auth;  // 暴露给 API 401 handler 使用

    // ===== 登录态自动恢复：页面刷新后也拉租户列表 =====
    // fetchTenants 原来只在 login() 成功回调里调 — 页面刷新（localStorage 有 token）
    // 不走 login()，导致 tenantOptions 永远为空 → 所有 tenant_id 下拉无选项
    if (auth.token) fetchTenants();

    return { auth, loginForm, captchaSvg, clientIp, route, menuItems, login, logout, refreshCaptcha,
             tenantId, tenantOptions, tenantLoading, changeTenant, fetchTenants };
  },
});

// 注册结构化编辑器
app.component('time-range-editor',    TimeRangeEditor);
app.component('queue-members-editor',  QueueMembersEditor);
app.component('ivr-options-editor',    IvrOptionsEditor);
app.component('trunk-codec-editor',    TrunkCodecEditor);
app.component('dp-action-editor',      DialplanActionEditor);
app.component('dp-app-chain-editor',    AppChainEditor);
app.component('ha-node-list-editor',   HaNodeListEditor);

// ===== Shared CRUD factory =====
// asyncLoaders: { [formKey]: async () => [{value, label}] }  动态下拉选项，onMounted 时并发拉取
function crudTable(entity, path, columns, formSchema, asyncLoaders = null) {
  return {
    template: `
      <div class="space-y-4">
        <div v-if="errorMsg" class="mb-2 p-3 bg-red-50 border border-red-200 rounded text-red-700 text-sm flex items-center gap-2">
          <i class="fa-solid fa-triangle-exclamation"></i>{{ errorMsg }}
        </div>
        <div class="flex items-center justify-between">
          <h2 class="text-lg font-semibold text-slate-800"><i class="${entity.icon} mr-2 text-blue-600"></i>${entity.label}</h2>
          <div class="flex items-center gap-2">
            <el-select v-if="hasBoolColumn" v-model="filterEnabled" placeholder="状态" size="small" style="width:110px" clearable>
              <el-option label="全部" value=""></el-option>
              <el-option label="启用" value="true"></el-option>
              <el-option label="禁用" value="false"></el-option>
            </el-select>
            <el-button type="primary" size="small" @click="openCreate">
              <el-icon><Plus /></el-icon>新建
            </el-button>
          </div>
        </div>

        <el-table :data="items" border stripe size="default" style="width:100%">
          <el-table-column type="index" label="#" width="55" align="center"></el-table-column>
          <el-table-column v-for="c in columns" :key="c.key" :prop="c.key" :label="c.label" min-width="120">
            <template #default="{ row }">
              <el-tag v-if="c.type === 'bool'" :type="row[c.key] ? 'success' : 'info'" effect="light" size="small" round>
                {{ row[c.key] ? '启用' : '禁用' }}
              </el-tag>
              <span v-else-if="c.type === 'json'" class="font-mono text-xs text-slate-500" :title="row[c.key]" style="max-width:200px;display:inline-block;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;">{{ row[c.key] }}</span>
              <span v-else class="text-slate-700">{{ row[c.key] != null && row[c.key] !== '' ? row[c.key] : '-' }}</span>
            </template>
          </el-table-column>
          <el-table-column label="操作" width="180" align="center" fixed="right">
            <template #default="{ row }">
              <div style="display:flex; gap:4px; justify-content:center; align-items:center; flex-wrap:nowrap;">
                <el-button link type="primary" size="small" @click="openEdit(row)">
                  <el-icon><Edit /></el-icon>编辑
                </el-button>
                <el-popconfirm title="确认删除？" @confirm="del(row.id)">
                  <template #reference>
                    <el-button link type="danger" size="small">
                      <el-icon><Delete /></el-icon>删除
                    </el-button>
                  </template>
                </el-popconfirm>
              </div>
            </template>
          </el-table-column>
        </el-table>

        <div class="flex justify-end pt-2">
          <el-pagination
            v-model:current-page="page"
            :page-size="limit"
            :total="total"
            layout="total, prev, pager, next"
            small
            background
          ></el-pagination>
        </div>

        <el-dialog v-model="showModal" :title="(editingId ? '编辑' : '新建') + ' ${entity.label}'" width="720px" destroy-on-close>
          <el-form :model="form" label-position="top">
            <el-form-item v-for="f in formSchema" :key="f.key" :label="f.label">
              <template v-if="f.type === 'select'">
                <el-select v-model="form[f.key]" placeholder="请选择" style="width:100%" clearable>
                  <el-option v-for="o in f.options" :key="o.value" :label="o.label" :value="o.value"></el-option>
                </el-select>
              </template>
              <template v-else-if="f.type === 'async-select'">
                <el-select v-model="form[f.key]" placeholder="加载中..." style="width:100%" clearable :loading="asyncLoading[f.key]">
                  <el-option v-for="o in asyncOptions[f.key]" :key="o.value" :label="o.label" :value="o.value"></el-option>
                </el-select>
              </template>
              <el-input-number v-else-if="f.type === 'number'" v-model="form[f.key]" :controls="false" style="width:100%"></el-input-number>
              <el-switch v-else-if="f.type === 'bool'" v-model="form[f.key]" active-text="启用" inactive-text="禁用"></el-switch>
              <template v-else-if="f.type === 'json'">
                <div class="w-full">
                  <div class="flex justify-between items-center mb-1">
                    <span class="text-xs text-slate-500">结构化数据（点击切换 Raw JSON）</span>
                    <el-button link size="small" type="primary" @click="rawMode[f.key] = !rawMode[f.key]">
                      {{ rawMode[f.key] ? '结构化编辑' : 'Raw JSON' }}
                    </el-button>
                  </div>
                  <el-input v-if="rawMode[f.key]" v-model="form[f.key]" type="textarea" :rows="6"
                    placeholder='有效 JSON，例如 {"clip":"44"}' resize="none"></el-input>
                  <trunk-codec-editor v-else-if="f.editor === 'trunk-codec'" v-model="form[f.key]"></trunk-codec-editor>
                  <dp-action-editor   v-else-if="f.editor === 'dp-action'"   v-model="form[f.key]"></dp-action-editor>
                  <time-range-editor  v-else-if="f.editor === 'time-ranges'"  v-model="form[f.key]"></time-range-editor>
                  <queue-members-editor v-else-if="f.editor === 'queue-members'" v-model="form[f.key]"></queue-members-editor>
                  <ivr-options-editor v-else-if="f.editor === 'ivr-options'"  v-model="form[f.key]"></ivr-options-editor>
                  <ha-node-list-editor v-else-if="f.editor === 'ha-node-list'"  v-model="form[f.key]"></ha-node-list-editor>
                  <el-input v-else v-model="form[f.key]" type="textarea" :rows="6" resize="none"
                    placeholder='输入 JSON 字符串'></el-input>
                </div>
              </template>
              <el-input v-else-if="f.type === 'textarea'" v-model="form[f.key]" type="textarea" :rows="3" resize="vertical"></el-input>
              <el-input v-else-if="f.type === 'password'" v-model="form[f.key]" type="password" show-password :placeholder="f.help || '请输入密码'"></el-input>
              <el-input v-else v-model="form[f.key]" :placeholder="'请输入 ' + f.label"></el-input>
              <div v-if="f.help" class="text-xs text-slate-400 mt-1">{{ f.help }}</div>
            </el-form-item>
          </el-form>
          <template #footer>
            <el-button @click="showModal = false">取消</el-button>
            <el-button type="primary" :loading="saving" @click="save">保 存</el-button>
          </template>
        </el-dialog>
      </div>
    `,
    props: [],
    setup() {
      const items = ref([]), total = ref(0), page = ref(1), limit = ref(50);
      const filterEnabled = ref('');
      const showModal = ref(false), editingId = ref(null);
      const form = reactive({}), saving = ref(false);
      const rawMode = reactive({});
      const errorMsg = ref('');  // 显示在列表上方的错误提示
      // async-select 动态选项
      const asyncOptions = reactive({});
      const asyncLoading = reactive({});

      const hasBoolColumn = columns.some(c => c.type === 'bool');
      const hasAsync = !!asyncLoaders;

      async function load() {
        const q = new URLSearchParams({ limit: limit.value, offset: (page.value - 1) * limit.value });
        if (filterEnabled.value) q.set('enabled', filterEnabled.value);
        errorMsg.value = '';
        try {
          const r = await API.get(path + '?' + q.toString());
          items.value = r.items || []; total.value = r.total || items.value.length;
        } catch (e) {
          items.value = []; total.value = 0;
          errorMsg.value = '加载失败: ' + (e.message || e) + '（检查后端是否已编译 / DB 是否可用 / 登录是否过期）';
        }
      }
      async function loadAsyncOptions() {
        if (!asyncLoaders) return;
        const tasks = Object.entries(asyncLoaders).map(async ([key, fn]) => {
          asyncLoading[key] = true;
          try { asyncOptions[key] = await fn(); }
          catch (e) { asyncOptions[key] = []; }
          finally { asyncLoading[key] = false; }
        });
        await Promise.all(tasks);
      }
      function openCreate() {
        Object.keys(form).forEach(k => delete form[k]);
        Object.keys(rawMode).forEach(k => delete rawMode[k]);
        formSchema.forEach(f => {
          form[f.key] = f.type === 'bool' ? true : (f.default ?? '');
          if (f.editor) rawMode[f.key] = false;
        });
        // 自动填入当前全局租户 ID（覆盖 default）
        if (formSchema.some(f => f.key === 'tenant_id')) {
          form.tenant_id = API.tenantId();
        }
        editingId.value = null; showModal.value = true;
      }
      function openEdit(row) {
        Object.keys(form).forEach(k => delete form[k]);
        Object.keys(rawMode).forEach(k => delete rawMode[k]);
        Object.assign(form, row);
        formSchema.forEach(f => { if (f.editor) rawMode[f.key] = false; });
        editingId.value = row.id; showModal.value = true;
      }
      function toggleRaw(k) { rawMode[k] = !rawMode[k]; }

      async function save() {
        saving.value = true;
        try {
          for (const f of formSchema) {
            if (f.type === 'json') {
              try { const v = form[f.key] || 'null'; if (v !== 'null') JSON.parse(v); }
              catch { alert(f.label + ' 不是有效 JSON'); saving.value = false; return; }
            }
          }
          const body = {};
          formSchema.forEach(f => {
            const v = form[f.key];
            if (v === undefined) return;
            // password 字段：空字符串也不传（编辑时留空 = 不改密码）
            if (f.type === 'password' && v === '') return;
            body[f.key] = v;
          });
          if (editingId.value) await API.put(path + '/' + editingId.value, body);
          else await API.post(path, body);
          showModal.value = false; await load();
        } catch (e) { alert('保存失败: ' + e.message); }
        finally { saving.value = false; }
      }
      async function del(id) {
        if (!confirm('确认删除 ID=' + id + '？')) return;
        try { await API.delete(path + '/' + id); await load(); }
        catch (e) { alert('删除失败: ' + e.message); }
      }

      onMounted(() => {
        load(); loadAsyncOptions();
        // 监听租户切换，自动重新加载当前实体列表
        const onTenantChanged = () => { load(); };
        window.addEventListener('nsbc-tenant-changed', onTenantChanged);
        onBeforeUnmount(() => {
          window.removeEventListener('nsbc-tenant-changed', onTenantChanged);
        });
      });
      watch([page, limit, filterEnabled], load);

      return { items, total, page, limit, filterEnabled, showModal, editingId, form, saving, rawMode,
               errorMsg, hasBoolColumn, hasAsync, columns, formSchema, asyncOptions, asyncLoading,
               openCreate, openEdit, toggleRaw, save, del };
    },
  };
}

// ===== 只读面板工厂 =====
function livePanel(title, icon, columns, apiPath, opts = {}) {
  const { refreshMs = 5000, actionCol = null, pageSize = 20, sortBy = null, sortDir = 'desc' } = opts;
  return {
    template: `
      <div>
        <div class="flex items-center justify-between mb-4">
          <h2 class="text-lg font-semibold"><i :class="icon"></i> ${title}</h2>
          <div class="flex items-center gap-2 text-xs text-gray-500">
            <span v-if="loading"><i class="fa-solid fa-spinner fa-spin"></i> 加载中</span>
            <span v-else-if="error" class="text-red-500">⚠ {{ error }}</span>
            <span v-else>✓ 已刷新 {{ lastUpdated }}</span>
            <button @click="load" class="px-2 py-1 border rounded hover:bg-gray-50">刷新</button>
          </div>
        </div>
        <div v-if="!allRows.length && !loading" class="text-center text-gray-400 py-12">
          暂无数据
        </div>
        <div v-else class="overflow-x-auto border rounded">
        <table class="w-full text-sm border-collapse min-w-[1400px]">
          <thead>
            <tr class="bg-gray-50 border-b">
              <th v-for="c in columns" :key="c.key" class="px-3 py-2 text-left font-medium text-gray-600 whitespace-nowrap">{{ c.label }}</th>
              <th v-if="actionCol" class="px-3 py-2 text-left font-medium text-gray-600 sticky right-0 bg-gray-50 border-l">操作</th>
            </tr>
          </thead>
          <tbody>
            <tr v-for="(row, idx) in pagedRows" :key="idx" class="border-b hover:bg-blue-50/50">
              <td v-for="c in columns" :key="c.key" class="px-3 py-2 font-mono text-xs text-gray-700 whitespace-nowrap">
                <template v-if="row[c.key] !== undefined && row[c.key] !== null && row[c.key] !== ''">
                  {{ c.fmt ? c.fmt(row[c.key]) : row[c.key] }}
                </template>
                <template v-else>-</template>
              </td>
              <td v-if="actionCol" class="px-3 py-2 sticky right-0 bg-white border-l">
                <span v-html="actionCol(row)"></span>
              </td>
            </tr>
          </tbody>
        </table>
        </div>
        <!-- 分页控件 -->
        <div v-if="allRows.length > pageSize" class="flex items-center justify-between mt-3 text-xs text-gray-500">
          <div>共 {{ allRows.length }} 条 · 每页 {{ pageSize }} 条</div>
          <div class="flex items-center gap-1">
            <button @click="currentPage = 1" :disabled="currentPage===1" class="px-2 py-1 border rounded disabled:opacity-30">«</button>
            <button @click="currentPage--" :disabled="currentPage===1" class="px-2 py-1 border rounded disabled:opacity-30">‹</button>
            <span class="px-2">{{ currentPage }} / {{ totalPages }}</span>
            <button @click="currentPage++" :disabled="currentPage===totalPages" class="px-2 py-1 border rounded disabled:opacity-30">›</button>
            <button @click="currentPage = totalPages" :disabled="currentPage===totalPages" class="px-2 py-1 border rounded disabled:opacity-30">»</button>
          </div>
        </div>
      </div>
    `,
    setup() {
      const allRows = ref([]);
      const loading = ref(false);
      const error = ref(null);
      const lastUpdated = ref('');
      const currentPage = ref(1);
      let timer = null;

      const totalPages = computed(() => Math.max(1, Math.ceil(allRows.value.length / pageSize)));
      const pagedRows = computed(() => {
        const start = (currentPage.value - 1) * pageSize;
        return allRows.value.slice(start, start + pageSize);
      });

      async function load(silent) {
        if (silent !== true) loading.value = true;
        error.value = null;
        try {
          const data = await API.get(apiPath);
          let rows = extractRows(data);
          // 排序
          if (sortBy) {
            rows = rows.slice().sort((a, b) => {
              let va = a[sortBy], vb = b[sortBy];
              if (va == null) va = 0; if (vb == null) vb = 0;
              if (typeof va === 'string') return sortDir === 'asc' ? va.localeCompare(vb) : vb.localeCompare(va);
              return sortDir === 'asc' ? va - vb : vb - va;
            });
          }
          allRows.value = rows;
          if (currentPage.value > totalPages.value) currentPage.value = totalPages.value;
          lastUpdated.value = new Date().toLocaleTimeString();
        } catch (e) { error.value = e.message; }
        finally { if (silent !== true) loading.value = false; }
      }

      function extractRows(data) {
        if (!data) return [];
        return data.rates || data.accounts || data.sessions || data.calls || data.cdrs
            || data.items  // 后端分页结构
            || (data.metrics && Object.entries(data.metrics).map(([k,v]) => ({metric: k, value: v})))
            || (Array.isArray(data) ? data : []);
      }

      onMounted(() => { load(); if (refreshMs > 0) timer = setInterval(load, refreshMs); });
      // nsbc-esl-web §8#1：SSE 推送触发静默刷新（不闪 loading），与 refreshMs
      // 轮询构成 push+pull 双路径。修复：清定时器须在卸载时（原误用 onMounted，
      // 导致 setInterval 刚建即被清除，轮询实际不生效）。
      const onSseRefresh = () => load(true);
      onMounted(() => document.addEventListener('nsbc-refresh', onSseRefresh));
      onBeforeUnmount(() => { if (timer) clearInterval(timer); document.removeEventListener('nsbc-refresh', onSseRefresh); });
      return { allRows, loading, error, lastUpdated, load, columns, icon, currentPage, totalPages, pagedRows, actionCol };
    },
  };
}

// 所有 const / function 在经典 script 标签间自动共享全局词法作用域，
// 后续 pages-crud.js / pages-live.js 可直接使用 app, crudTable, livePanel, API 等。

// ===== 全局挂断函数（livePanel actionCol 用 v-html + onclick 调） =====
window.nsbcHangup = async function(callId) {
  if (!callId) return;
  if (!confirm('确认挂断通话 ' + callId + ' ？')) return;
  try {
    await API.post('/calls/' + encodeURIComponent(callId) + '/hangup', {});
    // 触发所有 livePanel 刷新
    document.dispatchEvent(new CustomEvent('nsbc-refresh'));
    alert('挂断指令已发送');
  } catch (e) {
    alert('挂断失败: ' + (e.message || e));
  }
};

// ===== SESSION 实时推送消费者（SSE，nsbc-esl-web §8#1）=====
// 双路径设计（符合项目「订阅推送 + 拉取」约束）：本消费者把服务端 /calls/stream
// 的 SESSION 事件转成本地 nsbc-refresh 事件即时刷新 livePanel；若 SSE 不可用，
// 各面板的 refreshMs 轮询自动兜底，无感降级。
// 说明：EventSource 无法携带 Authorization 头，故用 fetch + ReadableStream 手解析
// SSE 帧（event:/data: 行，空行分隔）；连接断开按指数退避（1s→30s）重连。
(function () {
  const MAX_BACKOFF = 30000;
  let refreshTimer = null;
  let backoff = 1000;

  // 合并突发事件：一次呼叫会有 create/answer/hold/... 多个事件，300ms 去抖后
  // 只触发一次面板刷新，避免高频重拉。
  function scheduleRefresh() {
    if (refreshTimer) return;
    refreshTimer = setTimeout(() => {
      refreshTimer = null;
      document.dispatchEvent(new CustomEvent('nsbc-refresh'));
    }, 300);
  }

  async function streamLoop() {
    const token = API.token();
    if (!token) { setTimeout(streamLoop, 2000); return; }  // 未登录：稍后重试
    let res;
    try {
      const h = API.headers(false);
      h['Accept'] = 'text/event-stream';
      res = await fetch(API.base + '/calls/stream', { headers: h });
      if (!res.ok || !res.body) throw new Error('HTTP ' + res.status);
    } catch (e) {
      console.warn('[live] SSE 连接失败，' + backoff + 'ms 后重连：', e.message || e);
      setTimeout(streamLoop, backoff);
      backoff = Math.min(backoff * 2, MAX_BACKOFF);
      return;
    }
    backoff = 1000;  // 连接成功，重置退避
    const reader = res.body.getReader();
    const dec = new TextDecoder();
    let buf = '';
    try {
      for (;;) {
        const { value, done } = await reader.read();
        if (done) break;
        buf += dec.decode(value, { stream: true });
        let idx;
        while ((idx = buf.indexOf('\n\n')) >= 0) {
          const frame = buf.slice(0, idx);
          buf = buf.slice(idx + 2);
          const m = /(^|\n)event:\s*([^\n]+)/.exec(frame);
          const ev = m ? m[2].trim() : 'message';
          if (ev === 'session') scheduleRefresh();
        }
      }
    } catch (e) {
      console.warn('[live] SSE 读取中断：', e.message || e);
    }
    setTimeout(streamLoop, backoff);        // 断线重连
    backoff = Math.min(backoff * 2, MAX_BACKOFF);
  }

  // 延迟启动，等首帧/登录态就绪
  setTimeout(streamLoop, 1500);
})();
