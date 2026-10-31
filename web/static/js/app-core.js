// =============================================================================
// NSBC Web — Core层
//
// Copyright 2024-2026 李浩 (Li Hao), 上海宁卫信息技术有限公司
// Contact: 1354608370@qq.com
// Licensed under the Apache License, Version 2.0. See LICENSE in project root.
//
// 包含：API wrapper、结构化Edit器、Vue App Create、CRUD/Live 工厂
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
  // GET 请求 per-path in-flight 去重：同一 URL 正在请求时复用同一个 Promise，
  // 避免 SSE 事件 + setInterval 双路径叠加导致同一 API 瞬间并发 N 个请求打爆后端。
  _inflight: new Map(),
  async req(method, path, body) {
    const hasBody = body !== undefined;
    const opts = { method, headers: this.headers(hasBody) };
    if (hasBody) opts.body = JSON.stringify(body);
    // GET Request自动拼 tenant_id（tenant_id=0 或空表示不过滤）
    // 排除 /tenants 本身（Tenant元数据源，不能被 tenant_id 过滤）
    let finalPath = path;
    if (!hasBody && method === 'GET' && !path.startsWith('/tenants')) {
      const tid = this.tenantId();
      if (tid > 0) {
        const sep = path.includes('?') ? '&' : '?';
        finalPath = path + sep + 'tenant_id=' + tid;
      }
    }
    // GET in-flight 去重（只对 GET 做，POST/PUT/DELETE 必须每次独立发送）
    const dedupKey = method + ' ' + finalPath;
    if (method === 'GET' && this._inflight.has(dedupKey)) {
      return this._inflight.get(dedupKey);
    }
    const promise = this._doReq(method, path, body, finalPath, opts, dedupKey);
    if (method === 'GET') this._inflight.set(dedupKey, promise);
    try {
      return await promise;
    } finally {
      if (method === 'GET') this._inflight.delete(dedupKey);
    }
  },
  async _doReq(method, path, body, finalPath, opts, dedupKey) {
    const MAX_429_RETRY = 2;
    let delay = 500; // 初始退避 500ms
    for (let attempt = 0; attempt <= MAX_429_RETRY; attempt++) {
      if (attempt > 0) {
        await new Promise(r => setTimeout(r, delay));
        delay *= 2; // 指数退避
      }
      const res = await fetch(this.base + finalPath, opts);
      if (res.status === 429) {
        // 读取 Retry-After 头（秒），没有则用指数退避
        const retryAfter = parseInt(res.headers.get('Retry-After'), 10);
        if (!isNaN(retryAfter) && retryAfter > 0) delay = retryAfter * 1000;
        console.warn(`[API] 429 Too Many Requests on ${finalPath}, retry ${attempt+1}/${MAX_429_RETRY+1} after ${delay}ms`);
        // 必须消费 body 否则连接可能泄漏
        try { await res.text(); } catch (_) {}
        continue;
      }
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
              // 重放原Request（token 已Update）
              const retryRes = await fetch(this.base + path, opts);
              if (retryRes.status === 401) {
                localStorage.clear();
                location.hash = '#/login';
                throw new Error('Unauthorized');
              }
              const retryText = await retryRes.text();
              if (!retryRes.ok) throw new Error(retryText || retryRes.statusText);
              return retryText ? JSON.parse(retryText) : null;
            }
          } catch (refreshErr) { /* refresh 失败走下面清 localStorage */ }
          finally { this._refreshing = false; }
        }
        localStorage.clear();
        try { window.__auth && (window.__auth.token = null, window.__auth.user = null, window.__auth.role = null); } catch(_) {}
        location.hash = '#/login';
        throw new Error('Unauthorized');
      }
      // 正常响应：解析并返回
      const text = await res.text();
      const data = text ? JSON.parse(text) : null;
      if (!res.ok) throw new Error((data && data.detail) || res.statusText);
      return data;
    } // end of retry for loop
    // 循环耗尽（全部 429），抛出最后一次错误
    throw new Error('Too Many Requests (429) — please slow down');
  },
  get:    (p) => API.req('GET', p),
  post:   (p, b) => API.req('POST', p, b),
  put:    (p, b) => API.req('PUT', p, b),
  delete: (p) => API.req('DELETE', p),
};

// ===== 结构化Edit器 =====

// 1) 时间段（time_ranges → JSON 数组）
const TimeRangeEditor = {
  props: ['modelValue'],
  template: `
    <div class="struct-editor">
      <div class="struct-header">
        <span class="font-semibold">{{ t('Time Rules') }} ({{ rows.length }})</span>
        <span class="text-gray-400 text-[11px]">days: 1-5 / 1,3,5 / 0-6</span>
      </div>
      <div v-if="!rows.length" class="px-3 py-6 text-center text-gray-400 text-sm">{{ t('No time rules') }}</div>
      <div v-for="(row, idx) in rows" :key="idx" class="struct-row">
        <input v-model="row.days" placeholder="1-5" class="w-28">
        <span class="text-gray-400 text-xs">{{ t('From') }}</span>
        <input v-model="row.from" placeholder="09:00" class="w-24">
        <span class="text-gray-400 text-xs">{{ t('To') }}</span>
        <input v-model="row.to" placeholder="18:00" class="w-24">
        <i class="fa-solid fa-trash struct-del ml-auto" @click="rows.splice(idx,1)"></i>
      </div>
      <div class="struct-add" @click="rows.push({days:'1-5',from:'09:00',to:'18:00'})"><i class="fa-solid fa-plus mr-1"></i>{{ t('Add time rule') }}</div>
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
        <span class="font-semibold">{{ t('Queue Members') }} ({{ rows.length }})</span>
        <span class="text-gray-400 text-[11px]">extension / weight(1-100) / enabled</span>
      </div>
      <div v-if="!rows.length" class="px-3 py-6 text-center text-gray-400 text-sm">{{ t('No members') }}</div>
      <div v-for="(row, idx) in rows" :key="idx" class="struct-row">
        <input v-model="row.extension" placeholder="Extension" class="w-32">
        <input v-model.number="row.weight" type="number" min="1" max="100" placeholder="Weight" class="w-20">
        <label class="flex items-center gap-1 text-xs">
          <input type="checkbox" v-model="row.enabled"> Enable
        </label>
        <i class="fa-solid fa-trash struct-del ml-auto" @click="rows.splice(idx,1)"></i>
      </div>
      <div class="struct-add" @click="rows.push({extension:'',weight:10,enabled:true})"><i class="fa-solid fa-plus mr-1"></i>{{ t('Add member') }}</div>
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

// 4) IVR 菜单树编辑器（REQ-PBX-IVR-004：与 C 侧 ivr_node_t 1:1 对齐的 JSON 树）
//    每个节点：prompt/timeout/retries + choices + timeout_action + invalid_action
//    choices 的 act 支持 submenu（跨节点跳转）/transfer/play/hangup/voicemail/http
const IvrTreeEditor = {
  props: ['modelValue'],
  template: `
    <div class="struct-editor">
      <div class="struct-header">
        <span class="font-semibold">{{ t('IVR Menu Tree') }} ({{ nodes.length }} {{ t('nodes') }})</span>
        <span class="text-gray-400 text-[11px]">{{ t('entry node:') }} <b>{{ entryNode || 'main' }}</b>
          · {{ t('act:') }} submenu / transfer / play / hangup / vm / http
          · {{ t('choices key 0-9/*/#; act=submenu arg=other_node_name jumps to that node') }}
        </span>
      </div>
      <div v-if="!nodes.length" class="px-3 py-6 text-center text-gray-400 text-sm">{{ t('No nodes — add one to start') }}</div>

      <!-- 节点卡片列表 -->
      <div v-for="(node, ni) in nodes" :key="ni"
           class="border border-slate-200 rounded-lg mb-3 bg-white shadow-sm overflow-hidden">
        <!-- 节点头 -->
        <div class="px-3 py-2 bg-slate-50 flex items-center gap-2 border-b">
          <i class="fa-solid fa-branch text-blue-600"></i>
          <input v-model="node.name" class="w-32 text-sm font-mono" placeholder="node_name">
          <input v-model="node.prompt" class="flex-1 text-sm" placeholder="prompt audio (e.g. welcome.wav)">
          <i v-if="nodes.length > 1" class="fa-solid fa-trash text-red-500 cursor-pointer hover:text-red-700"
             @click="nodes.splice(ni,1)"></i>
        </div>

        <!-- 节点超时配置 -->
        <div class="px-3 py-2 bg-slate-50/50 flex flex-wrap gap-3 border-b text-sm">
          <label class="flex items-center gap-1 text-gray-600">
            {{ t('Initial timeout (s)') }}
            <input type="number" v-model.number="node.initial_timeout_s" min="1" max="120" class="w-14 text-center border rounded">
          </label>
          <label class="flex items-center gap-1 text-gray-600">
            {{ t('Interdigit (s)') }}
            <input type="number" v-model.number="node.interdigit_timeout_s" min="1" max="60" class="w-14 text-center border rounded">
          </label>
          <label class="flex items-center gap-1 text-gray-600">
            {{ t('Max retries') }}
            <input type="number" v-model.number="node.max_retries" min="1" max="10" class="w-14 text-center border rounded">
          </label>
        </div>

        <!-- 按键选项表 -->
        <div class="px-3 py-2 border-b">
          <div class="text-xs text-gray-500 mb-1">{{ t('Digit Choices') }} ({{ node.choices.length }})</div>
          <div v-if="!node.choices.length" class="text-gray-300 text-xs italic mb-2">{{ t('no choices yet') }}</div>
          <div v-for="(ch, ci) in node.choices" :key="ci" class="flex items-center gap-1 mb-1">
            <select v-model="ch.key" class="w-16 text-sm">
              <option v-for="k in ['0','1','2','3','4','5','6','7','8','9','*','#']" :key="k" :value="k">{{ k }}</option>
            </select>
            <select v-model="ch.act" class="w-24 text-sm">
              <option value="submenu">{{ t('submenu') }}</option>
              <option value="transfer">{{ t('transfer') }}</option>
              <option value="play">{{ t('play') }}</option>
              <option value="hangup">{{ t('hangup') }}</option>
              <option value="voicemail">{{ t('voicemail') }}</option>
              <option value="http_query">{{ t('http_query') }}</option>
            </select>
            <input v-model="ch.arg" class="flex-1 text-sm"
                   :placeholder="ch.act==='submenu' ? 'target node name (e.g. sales)' :
                                ch.act==='transfer' ? 'target ext/queue (e.g. 1000)' :
                                ch.act==='play' ? 'audio file path' :
                                ch.act==='http_query' ? 'URL' : ''">
            <i class="fa-solid fa-trash text-gray-300 hover:text-red-500 cursor-pointer text-xs ml-1"
               @click="node.choices.splice(ci,1)"></i>
          </div>
          <div class="text-blue-600 text-xs cursor-pointer hover:text-blue-800"
               @click="node.choices.push({key:'1',act:'transfer',arg:''})">
            <i class="fa-solid fa-plus mr-1"></i>{{ t('Add choice') }}
          </div>
        </div>

        <!-- timeout_action / invalid_action -->
        <div class="px-3 py-2 flex gap-3 text-xs">
          <div class="flex-1 border-r pr-3">
            <div class="text-gray-500 mb-1">{{ t('Timeout Action (after retries exhausted)') }}</div>
            <div class="flex gap-1">
              <select v-model="node.timeout_action.act" class="w-24">
                <option value="hangup">{{ t('hangup') }}</option>
                <option value="transfer">{{ t('transfer') }}</option>
                <option value="play">{{ t('play') }}</option>
              </select>
              <input v-model="node.timeout_action.arg" class="flex-1" placeholder="target or audio">
            </div>
          </div>
          <div class="flex-1">
            <div class="text-gray-500 mb-1">{{ t('Invalid Action (unmatched digit)') }}</div>
            <div class="flex gap-1">
              <select v-model="node.invalid_action.act" class="w-24">
                <option value="hangup">{{ t('hangup') }}</option>
                <option value="transfer">{{ t('transfer') }}</option>
                <option value="play">{{ t('play') }}</option>
              </select>
              <input v-model="node.invalid_action.arg" class="flex-1" placeholder="target or audio">
            </div>
          </div>
        </div>
      </div>

      <!-- 底部：加节点 + 入口节点提示 -->
      <div class="flex items-center gap-2 mt-2">
        <div class="text-blue-600 text-xs cursor-pointer hover:text-blue-800 flex items-center gap-1"
             @click="addNode">
          <i class="fa-solid fa-plus"></i>{{ t('Add node') }}
        </div>
        <span class="text-gray-300">|</span>
        <span class="text-gray-500 text-xs">{{ t('Tip: set entry_node field (above) to match a node.name') }}</span>
      </div>
    </div>
  `,
  setup(props, { emit }) {
    const nodes = ref([]);
    const entryNode = ref('main');
    let _syncing = false;

    function normalizeNode(n) {
      return {
        name: n.name || '',
        prompt: n.prompt || '',
        initial_timeout_s: n.initial_timeout_s || 5,
        interdigit_timeout_s: n.interdigit_timeout_s || 3,
        max_retries: n.max_retries || 3,
        choices: (n.choices || []).map(c => ({
          key: c.key || '1',
          act: c.act || c.action?.act || 'transfer',
          arg: c.arg || c.action?.arg || ''
        })),
        timeout_action: n.timeout_action || { act: 'hangup', arg: '' },
        invalid_action: n.invalid_action || { act: 'hangup', arg: '' },
      };
    }

    function syncFrom() {
      _syncing = true;
      try {
        const raw = props.modelValue || '';
        if (raw.startsWith('[')) {
          const parsed = JSON.parse(raw);
          nodes.value = (Array.isArray(parsed) ? parsed : []).map(normalizeNode);
        } else {
          // 旧前端发的扁平 options_json — 转换成单一 main 节点
          const flat = raw ? JSON.parse(raw) : [];
          const choices = (Array.isArray(flat) ? flat : []).map(o => ({
            key: o.key || '1',
            act: o.act || 'transfer',
            arg: o.arg || o.target || ''
          }));
          nodes.value = [{
            name: 'main', prompt: '',
            initial_timeout_s: 5, interdigit_timeout_s: 3, max_retries: 3,
            choices, timeout_action: { act: 'hangup', arg: '' },
            invalid_action: { act: 'hangup', arg: '' },
          }];
          entryNode.value = 'main';
        }
        if (!nodes.value.length) nodes.value.push(normalizeNode({name:'main'}));
      } catch(e) {
        nodes.value = [normalizeNode({name:'main'})];
      }
      _syncing = false;
    }
    function syncTo() {
      if (_syncing) return;
      emit('update:modelValue', JSON.stringify(nodes.value));
    }

    function addNode() {
      nodes.value.push(normalizeNode({ name: 'node_' + (nodes.value.length + 1) }));
    }

    watch(() => props.modelValue, syncFrom);
    watch(nodes, syncTo, { deep: true });
    syncFrom();
    return { nodes, entryNode, addNode };
  },
};

// 旧 IvrOptionsEditor 保留做兼容（后端 options_json 字段仍可读写），
// 新菜单统一用 IvrTreeEditor + nodes_json。
const IvrOptionsEditor = IvrTreeEditor;

// 4) Trunk Codec Set（逗号分隔字符串 "PCMA,PCMU,G729"）
const TRUNK_CODECS = ['PCMA','PCMU','G711U','G711A','G729','G722','G722.2','ILBC','OPUS','SPEEX','GSM','AMR','AMR-WB'];
const TrunkCodecEditor = {
  props: ['modelValue'],
  template: `
    <div class="struct-editor">
      <div class="struct-header">
        <span class="font-semibold">{{ t('Available Codecs') }} ({{ selected.length }})</span>
        <span class="text-gray-400 text-[11px]">{{ t('check to build comma list') }}</span>
      </div>
      <div class="codec-grid">
        <label v-for="c in TRUNK_CODECS" :key="c">
          <input type="checkbox" :value="c" v-model="selected"> {{ c }}
        </label>
      </div>
      <div class="struct-header border-t bg-white">
        <span class="font-mono text-xs text-gray-600 truncate">{{ modelValue || t('not selected') }}</span>
        <button class="text-xs text-blue-600 hover:text-blue-800" @click="selectAll(true)">{{ t('All') }}</button>
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
        <span class="font-semibold">{{ t('Dial Action (dp_rule extra fields)') }}</span>
        <span class="text-gray-400 text-[11px]">{{ t('number rewrite / SIP header override / recording control') }}</span>
      </div>
      <div class="px-3 py-3 space-y-2.5 bg-white max-h-[600px] overflow-y-auto">
        <!-- 号码变换 -->
        <div class="text-[11px] text-gray-500 font-semibold pt-0.5">{{ t('─ Number Rewrite ─') }}</div>
        <div class="struct-row">
          <span class="struct-label w-40 shrink-0">{{ t('clip forced CLIP') }}</span>
          <input v-model="action.clip" placeholder="e.g. 1234567890">
        </div>
        <div class="struct-row">
          <label class="inline-flex items-center gap-1.5">
            <input type="checkbox" v-model="action.immediate" class="flex-none">
            <span class="text-gray-600 text-xs">{{ t('immediate — match when digitmap complete') }}</span>
          </label>
        </div>
        <div class="struct-row">
          <span class="struct-label w-40 shrink-0">{{ t('perm outbound permission') }}</span>
          <input v-model.number="action.perm" type="number" min="0" max="255" class="w-20">
          <span class="hint">(0-255)</span>
        </div>

        <!-- From 头 -->
        <div class="text-[11px] text-gray-500 font-semibold pt-2">{{ t('─ From Header (low → high priority) ─') }}</div>
        <div class="struct-row">
          <span class="struct-label w-40 shrink-0">{{ t('sip_from_display Display') }}</span>
          <input v-model="action.sip_from_display" :placeholder="t('e.g. &quot;Sales Team&quot;')">
          <span class="hint">{{ t('component') }}</span>
        </div>
        <div class="struct-row">
          <span class="struct-label w-40 shrink-0">{{ t('sip_from_uri URI part') }}</span>
          <input v-model="action.sip_from_uri" placeholder="e.g. sip:user@domain">
          <span class="hint">{{ t('component') }}</span>
        </div>
        <div class="struct-row">
          <span class="struct-label w-40 shrink-0">{{ t('sip_invite_from Full From') }}</span>
          <input v-model="action.sip_invite_from" :placeholder="t('Full From value (highest priority)')">
          <span class="hint">{{ t('overrides all') }}</span>
        </div>
        <div class="struct-row">
          <span class="struct-label w-40 shrink-0">sip_invite_from_uri</span>
          <input v-model="action.sip_invite_from_uri" :placeholder="t('URI override only, e.g. sip:alice@nsbc.local')">
          <span class="hint">{{ t('FS-aligned') }}</span>
        </div>
        <div class="struct-row">
          <span class="struct-label w-40 shrink-0">sip_invite_from_params</span>
          <input v-model="action.sip_invite_from_params" :placeholder="t('e.g. transport=tls')">
          <span class="hint">From params</span>
        </div>
        <div class="struct-row">
          <span class="struct-label w-40 shrink-0">orig_caller_id_number</span>
          <input v-model="action.origination_caller_id_number" :placeholder="t('FreeSWITCH granular caller — user part only')">
          <span class="hint">{{ t('FS-aligned') }}</span>
        </div>
        <div class="struct-row">
          <span class="struct-label w-40 shrink-0">orig_caller_id_name</span>
          <input v-model="action.origination_caller_id_name" :placeholder="t('Display name (empty = same as number)')">
          <span class="hint">{{ t('FS-aligned') }}</span>
        </div>

        <!-- To / R-URI / Contact -->
        <div class="text-[11px] text-gray-500 font-semibold pt-2">─ To / R-URI / Contact ─</div>
        <div class="struct-row">
          <span class="struct-label w-40 shrink-0">{{ t('sip_invite_to_uri To override') }}</span>
          <input v-model="action.sip_invite_to_uri" placeholder="e.g. sip:user@domain">
        </div>
        <div class="struct-row">
          <span class="struct-label w-40 shrink-0">sip_invite_to_params</span>
          <input v-model="action.sip_invite_to_params" placeholder="e.g. transport=ws">
          <span class="hint">To params</span>
        </div>
        <div class="struct-row">
          <span class="struct-label w-40 shrink-0">sip_invite_req_uri R-URI</span>
          <input v-model="action.sip_invite_req_uri" placeholder="e.g. sip:user@domain">
        </div>
        <div class="struct-row">
          <span class="struct-label w-40 shrink-0">sip_invite_tel_params</span>
          <input v-model="action.sip_invite_tel_params" placeholder="tel: URI params">
          <span class="hint">{{ t('tel: append') }}</span>
        </div>
        <div class="struct-row">
          <span class="struct-label w-40 shrink-0">sip_invite_contact_params</span>
          <input v-model="action.sip_invite_contact_params" placeholder="e.g. transport=ws;ob">
          <span class="hint">Contact params</span>
        </div>

        <!-- Domain override -->
        <div class="text-[11px] text-gray-500 font-semibold pt-2">─ Domain override ─</div>
        <div class="struct-row">
          <span class="struct-label w-40 shrink-0">{{ t('sip_invite_domain Forced domain') }}</span>
          <input v-model="action.sip_invite_domain" placeholder="e.g. nsbc.local">
          <span class="hint">{{ t('global host replace') }}</span>
        </div>

        <!-- Extra headers -->
        <div class="text-[11px] text-gray-500 font-semibold pt-2">{{ t('─ Extra SIP Headers (injected into extra block) ─') }}</div>
        <div class="struct-row">
          <span class="struct-label w-40 shrink-0">sip_invite_pid_params</span>
          <input v-model="action.sip_invite_pid_params" placeholder="e.g. P-Asserted-Identity: <sip:...>
          <span class="hint">{{ t('full header line or shorthand') }}</span>
        </div>
        <div class="struct-row flex-wrap items-start">
          <span class="struct-label w-40 shrink-0 self-stretch">{{ t('sip_invite_params multi-block') }}</span>
          <div class="flex-1 min-w-0">
            <textarea v-model="action.sip_invite_params" rows="4"
              placeholder="One full header per line (core joins with \r\n), e.g.:&#10;P-Access-Network-Info: E-UTRAN-TDD; utran-cell-id-3gpp=0001001&#10;P-Called-Party-ID: <sip:+8613900139000@ims.mnc001.mcc460.3gppnetwork.org>&#10;Privacy: id"
              class="w-full text-xs"></textarea>
            <div class="flex items-center gap-2 mt-1">
              <span class="text-[11px] text-gray-400">{{ t('Quick insert:') }}</span>
              <button type="button" class="inline-block text-[11px] px-2 py-0.5 bg-slate-100 border border-slate-300 rounded hover:bg-slate-200" @click="tpl3gpp">3GPP IMS（PAI+PPID+PANI+PCPI）</button>
              <button type="button" class="inline-block text-[11px] px-2 py-0.5 bg-slate-100 border border-slate-300 rounded hover:bg-slate-200" @click="tplClir">运营商 CLIR（Privacy+PAI）</button>
            </div>
          </div>
          <span class="hint">{{ t('extra header block') }}</span>
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
      // FS 精细化Caller（只填 user part，host 自动走 trunk.from_domain）
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
        // FS 精细化Caller
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
    // 3GPP IMS 运营商常用头：PAI（Caller身份）、PPID（首选Caller）、
    // PANI（接入网Type）、PCPI（Callee E.164 标识）、Privacy（CLIR）
    function tpl3gpp() {
      action.sip_invite_pid_params = 'P-Asserted-Identity: <sip:+8613800138000@ims.mnc001.mcc460.3gppnetwork.org>';
      action.sip_invite_params =
`P-Preferred-Identity: <sip:+8613800138000@ims.mnc001.mcc460.3gppnetwork.org>
P-Access-Network-Info: E-UTRAN-TDD; utran-cell-id-3gpp=0001001
P-Called-Party-ID: <sip:+8613900139000@ims.mnc001.mcc460.3gppnetwork.org>
Privacy: id`;
    }
    // 运营商 CLIR：Caller ID隐藏（Privacy: id 触发 B 腿 From anonymize）
    // + 带 P-Asserted-Identity 让对端网络仍能拿到真实Caller用于计费/路由
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
        <span class="font-semibold">{{ t('Dial Application Chain') }}</span>
        <span class="text-gray-400 text-[11px]">{{ t('Run in order: pre_answer → playback → sleep → answer …') }}</span>
      </div>
      <div class="px-3 py-3 bg-white space-y-2">
        <!-- 添加Action -->
        <div class="flex items-center gap-2">
          <el-select v-model="nextApp" placeholder="Select app to add..." style="flex:1" size="small" filterable>
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

        <!-- 空Status -->
        <div v-if="!chain.length" class="text-center py-4 text-gray-400 text-xs">
          暂无拨号Action。上方选一个 app 添加进来，按顺序执行。
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
          JSON: {{ chain.length ? preview : '[] (empty chain, skipped by core)' }}
        </span>
      </div>
    </div>
  `,
  setup(props, { emit }) {
    const { ref, computed, watch } = Vue;
    const { Plus, ArrowUp, ArrowDown, Delete } = ElementPlusIconsVue || {};

    /* C 侧 app_registry.c builtin_apps[] Register的 17 个内建 apps（必须对齐） */
    const APP_DESCS = {
      answer:              'Answer inbound (send 200 OK)',
      pre_answer:          'Early media pre-answer (183, no 200 OK)',
      bridge:              'Bridge to sofia/gateway/<gw>/<ext>',
      playback:            'Play a single audio file',
      post_playback:       'Play after answer (phase marker)',
      record:              'Record current call audio',
      transfer:            'Transfer current call (REFER or 302)',
      set:                 'Set channel variable var=value',
      conference:          'Join conference <room>@<profile>',
      play_and_get_digits: 'Play prompt and collect DTMF',
      sleep:               'Pause execution (ms)',
      park:                'Hold call for pickup (hold + MoH)',
      att_xfer:            'Attended/consultative transfer',
      gentones:            'Generate tone sequence',
      endless_playback:    'Endless loop playback (uuid_break to interrupt)',
      uuid_break:          'Interrupt playback/sleep',
      read:                'Collect DTMF digits',
    };

    /* 按执行阶段Group（引导用户构建合理顺序） */
    const appGroups = computed(() => [
      { label: t('— Early Media / Answer Phase —'), apps: ['pre_answer', 'playback', 'post_playback', 'sleep', 'answer', 'endless_playback', 'read', 'play_and_get_digits'] },
      { label: t('— Routing / Bridge Phase —'),        apps: ['bridge', 'transfer', 'att_xfer', 'conference', 'park'] },
      { label: t('— Variables / Control —'),            apps: ['set', 'uuid_break', 'gentones'] },
      { label: t('— Media / Recording —'),            apps: ['record'] },
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
        playback:             'welcome.wav or /path/to/file.wav',
        post_playback:        'announce.wav',
        endless_playback:     'hold_music.wav',
        sleep:                '1500 (ms)',
        bridge:               'sofia/gateway/my_trunk/8613800138000',
        transfer:             'sip:support@other.com',
        conference:           'my_room@default',
        set:                  'var_name=value',
        gentones:             '1004,1500,800',
        record:               '/var/recording/callid.wav',
        play_and_get_digits:  'prompt.wav 3 5000',
        read:                 '5 10000',
      };
      return H[name] || '(no data)';
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
        <span class="font-semibold">{{ t('Cluster Nodes') }} ({{ rows.length }})</span>
        <span class="text-gray-400 text-[11px]">node_id/host/port/role(master|slave|witness)</span>
      </div>
      <div v-if="!rows.length" class="px-3 py-6 text-center text-gray-400 text-sm">{{ t('No nodes') }}</div>
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
      <div class="struct-add" @click="rows.push({node_id:'',host:'',port:8080,role:'slave'})"><i class="fa-solid fa-plus mr-1"></i>{{ t('Add node') }}</div>
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

    // ===== 多Tenant全局Status =====
    const tenantId = ref(parseInt(localStorage.getItem('nsbc_tenant_id') || '1', 10) || 1);
    const tenantOptions = ref([]);  // [{value, label}]
    const tenantLoading = ref(false);
    function changeTenant(id) {
      tenantId.value = id;
      localStorage.setItem('nsbc_tenant_id', id.toString());
      // 同步写入 SIP realm（WebRTC 面和独立 webrtc.html 都会读这个）
      const t = tenantOptions.value.find(x => x.value === id);
      if (t && t.label) {
        const tn = t.label.replace(/\s*\(Disable\)\s*$/, '').trim();
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
          label: t.tenant_name + (t.enabled ? '' : t(' (Disable)')),
        }));
      } catch (e) { tenantOptions.value = []; }
      finally { tenantLoading.value = false; }
    }
    // 暴露给外部（pages-crud.js crudTable 工厂需要）
    window.__tenantId = tenantId;

    const menuItems = [
      // 实时监控（置于顶部，运维高频）
      { path: '/realtime',      label: t('Realtime Overview'), icon: 'fa-solid fa-gauge-high' },
      { path: '/channels',     label: t('Live Calls'), icon: 'fa-solid fa-phone-volume' },
      { path: '/cdrs',          label: t('Call Record'), icon: 'fa-solid fa-file-lines' },
      { path: '/metrics',       label: t('Metrics'), icon: 'fa-solid fa-chart-line' },
      { path: '/registrations', label: t('Reg Status'), icon: 'fa-solid fa-user-check' },
      { path: '/security-center', label: t('Security Center'), icon: 'fa-solid fa-shield-halved' },
      // 业务数据
      { path: '/tenants',      label: t('Tenant (SIP Realm)'), icon: 'fa-solid fa-building' },
      { path: '/extensions',   label: t('Extension'),     icon: 'fa-solid fa-phone' },
      { path: '/sip-profiles', label: 'SIP Profile', icon: 'fa-solid fa-network-wired' },
      { path: '/config',       label: t('Config Hot-Editor'), icon: 'fa-solid fa-sliders' },
      { path: '/gateways',     label: t('Gateway'),     icon: 'fa-solid fa-tower-broadcast' },
      { path: '/trunks',       label: t('Trunk'),     icon: 'fa-solid fa-signal' },
      // 路由组已移到后端Core侧（nsbcd route_group_t），Web 侧暂不暴露
      { path: '/dialplan-rules', label: t('Dialplan Rules'), icon: 'fa-solid fa-code-branch' },
      { path: '/time-groups',  label: t('Time Groups'), icon: 'fa-solid fa-clock' },
      { path: '/queues',       label: t('Extension Queues'), icon: 'fa-solid fa-people-group' },
      { path: '/ivr-menus',    label: t('IVR Menus'), icon: 'fa-solid fa-circle-question' },
      // 计费与Media
      { path: '/rates',        label: t('Rates'), icon: 'fa-solid fa-coins' },
      { path: '/balances',     label: t('Balances'), icon: 'fa-solid fa-wallet' },
      { path: '/transcode',    label: t('MediaSession'), icon: 'fa-solid fa-microphone' },
      { path: '/codecs',       label: t('Codec Registry'), icon: 'fa-solid fa-code' },
      // 系统
      { path: '/ha-groups',    label: t('HA Groups'), icon: 'fa-solid fa-server' },
      // 工具
      { path: '/webrtc',       label: t('WebRTC Test'), icon: 'fa-solid fa-video' },
      // REQ-EXT-API-001: 外部 API 平台
      { path: '/ext/keys',     label: '🔑 API Key 管理', icon: 'fa-solid fa-key' },
      { path: '/ext/webhooks', label: '🔔 Webhook 回调', icon: 'fa-solid fa-plug' },
      { path: '/ext/guide',    label: '📡 外部 API 使用指南', icon: 'fa-solid fa-book-open' },
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
        // 登录Success后拉取Tenant列表
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

    // ===== Change Password =====
    const showPwdDialog = ref(false);
    const pwdLoading = ref(false);
    const pwdFormRef = ref(null);
    const pwdForm = reactive({
      old_password: '',
      new_password: '',
      confirm_password: '',
    });
    const pwdRules = {
      old_password: [{ required: true, message: () => t('Please enter current password'), trigger: 'blur' }],
      new_password: [
        { required: true, message: () => t('Please enter new password'), trigger: 'blur' },
        { min: 12, message: () => t('Password must be at least 12 characters'), trigger: 'blur' },
        { validator: (_, v, cb) => {
          if (!v) return cb();
          let c = 0;
          if (/[a-z]/.test(v)) c++;
          if (/[A-Z]/.test(v)) c++;
          if (/\d/.test(v)) c++;
          if (/[^a-zA-Z0-9]/.test(v)) c++;
          c >= 3 ? cb() : cb(new Error(t('Password must contain at least 3 of: uppercase, lowercase, digit, symbol')));
        }, trigger: 'blur' },
      ],
      confirm_password: [
        { required: true, message: () => t('Please confirm new password'), trigger: 'blur' },
        { validator: (_, v, cb) => v === pwdForm.new_password ? cb() : cb(new Error(t('Passwords do not match'))), trigger: 'blur' },
      ],
    };
    async function submitChangePassword() {
      if (!pwdFormRef.value) return;
      try {
        await pwdFormRef.value.validate();
      } catch { return; }
      pwdLoading.value = true;
      try {
        const res = await fetch('/api/v1/auth/change-password', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json', 'Authorization': 'Bearer ' + (localStorage.getItem('nsbc_token') || '') },
          body: JSON.stringify({ old_password: pwdForm.old_password, new_password: pwdForm.new_password }),
        });
        const data = await res.json().catch(() => ({}));
        if (res.ok) {
          ElementPlus.ElMessage.success(t('Password changed. Please log in again.'));
          showPwdDialog.value = false;
          pwdForm.old_password = ''; pwdForm.new_password = ''; pwdForm.confirm_password = '';
          setTimeout(logout, 800);  // 安全：密码改了后强制重新登录
        } else {
          ElementPlus.ElMessage.error(data.detail || t('Password change failed'));
        }
      } catch (e) {
        ElementPlus.ElMessage.error(t('Network error') + ': ' + e.message);
      } finally {
        pwdLoading.value = false;
      }
    }

    refreshCaptcha();
    window.__auth = auth;  // 暴露给 API 401 handler 使用

    // ===== 登录态自动恢复：面Refresh后也拉Tenant列表 =====
    // fetchTenants 原来只在 login() Success回调里调 — 面Refresh（localStorage 有 token）
    // 不走 login()，导致 tenantOptions 永远为空 → 所有 tenant_id 下拉无选项
    if (auth.token) fetchTenants();

    return { auth, loginForm, captchaSvg, clientIp, route, menuItems, login, logout, refreshCaptcha,
             tenantId, tenantOptions, tenantLoading, changeTenant, fetchTenants,
             showPwdDialog, pwdLoading, pwdFormRef, pwdForm, pwdRules, submitChangePassword };
  },
});

// Register结构化Edit器
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
            <el-select v-if="hasBoolColumn" v-model="filterEnabled" placeholder="All" size="small" style="width:110px" clearable>
              <el-option label="All" value=""></el-option>
              <el-option label="Enabled" value="true"></el-option>
              <el-option label="Disabled" value="false"></el-option>
            </el-select>
            <el-button type="primary" size="small" @click="openCreate">
              <el-icon><Plus /></el-icon>New
            </el-button>
          </div>
        </div>

        <el-table :data="items" border stripe size="default" style="width:100%">
          <el-table-column type="index" label="#" width="55" align="center"></el-table-column>
          <el-table-column v-for="c in columns" :key="c.key" :prop="c.key" :label="c.label" min-width="120">
            <template #default="{ row }">
              <el-tag v-if="c.type === 'bool'" :type="row[c.key] ? 'success' : 'info'" effect="light" size="small" round>
                {{ row[c.key] ? t('Enable') : t('Disable') }}
              </el-tag>
              <span v-else-if="c.type === 'json'" class="font-mono text-xs text-slate-500" :title="row[c.key]" style="max-width:200px;display:inline-block;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;">{{ row[c.key] }}</span>
              <span v-else class="text-slate-700">{{ row[c.key] != null && row[c.key] !== '' ? row[c.key] : '-' }}</span>
            </template>
          </el-table-column>
          <el-table-column label="Actions" width="180" align="center" fixed="right">
            <template #default="{ row }">
              <div style="display:flex; gap:4px; justify-content:center; align-items:center; flex-wrap:nowrap;">
                <el-button link type="primary" size="small" @click="openEdit(row)">
                  <el-icon><Edit /></el-icon>Edit
                </el-button>
                <el-popconfirm title="Delete this record?" @confirm="del(row.id)">
                  <template #reference>
                    <el-button link type="danger" size="small">
                      <el-icon><Delete /></el-icon>Delete
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

        <el-dialog v-model="showModal" :title="editingId ? 'Edit' : 'Create New'" width="720px" destroy-on-close>
          <el-form :model="form" label-position="top">
            <el-form-item v-for="f in formSchema" :key="f.key" :label="f.label">
              <template v-if="f.type === 'select'">
                <el-select v-model="form[f.key]" placeholder="Please select" style="width:100%" clearable>
                  <el-option v-for="o in f.options" :key="o.value" :label="o.label" :value="o.value"></el-option>
                </el-select>
              </template>
              <template v-else-if="f.type === 'async-select'">
                <el-select v-model="form[f.key]" placeholder="Loading options..." style="width:100%" clearable :loading="asyncLoading[f.key]">
                  <el-option v-for="o in asyncOptions[f.key]" :key="o.value" :label="o.label" :value="o.value"></el-option>
                </el-select>
              </template>
              <el-input-number v-else-if="f.type === 'number'" v-model="form[f.key]" :controls="false" style="width:100%"></el-input-number>
              <el-switch v-else-if="f.type === 'bool'" v-model="form[f.key]" active-text="ON" inactive-text="OFF"></el-switch>
              <template v-else-if="f.type === 'json'">
                <div class="w-full">
                  <div class="flex justify-between items-center mb-1">
                    <span class="text-xs text-slate-500">{{ t('Structured data (click to toggle Raw JSON)') }}</span>
                    <el-button link size="small" type="primary" @click="rawMode[f.key] = !rawMode[f.key]">
                      {{ rawMode[f.key] ? t('Structured') : 'Raw JSON' }}
                    </el-button>
                  </div>
                  <el-input v-if="rawMode[f.key]" v-model="form[f.key]" type="textarea" :rows="6"
                    :placeholder="t('Valid JSON, e.g. {&quot;clip&quot;:&quot;44&quot;}')" resize="none"></el-input>
                  <trunk-codec-editor v-else-if="f.editor === 'trunk-codec'" v-model="form[f.key]"></trunk-codec-editor>
                  <dp-action-editor   v-else-if="f.editor === 'dp-action'"   v-model="form[f.key]"></dp-action-editor>
                  <time-range-editor  v-else-if="f.editor === 'time-ranges'"  v-model="form[f.key]"></time-range-editor>
                  <queue-members-editor v-else-if="f.editor === 'queue-members'" v-model="form[f.key]"></queue-members-editor>
                  <ivr-options-editor v-else-if="f.editor === 'ivr-options'"  v-model="form[f.key]"></ivr-options-editor>
                  <ha-node-list-editor v-else-if="f.editor === 'ha-node-list'"  v-model="form[f.key]"></ha-node-list-editor>
                  <el-input v-else v-model="form[f.key]" type="textarea" :rows="6" resize="none"
                    placeholder='Enter JSON string'></el-input>
                </div>
              </template>
              <el-input v-else-if="f.type === 'textarea'" v-model="form[f.key]" type="textarea" :rows="3" resize="vertical"></el-input>
              <el-input v-else-if="f.type === 'password'" v-model="form[f.key]" type="password" show-password placeholder="••••••••"></el-input>
              <el-input v-else v-model="form[f.key]" placeholder="••••••••"></el-input>
              <div v-if="f.help" class="text-xs text-slate-400 mt-1">{{ f.help }}</div>
            </el-form-item>
          </el-form>
          <template #footer>
            <el-button @click="showModal = false">{{ t('Cancel') }}</el-button>
            <el-button type="primary" :loading="saving" @click="save">{{ t('Save') }}</el-button>
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
      const errorMsg = ref('');  // 显示在列表上方的Error提示
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
          errorMsg.value = t('Load failed: ') + (e.message || e) + t(' (check backend build / DB availability / login expiry)');
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
        // 自动填入Current全局Tenant ID（覆盖 default）
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
              catch { alert(f.label + ' is not valid JSON'); saving.value = false; return; }
            }
          }
          const body = {};
          formSchema.forEach(f => {
            const v = form[f.key];
            if (v === undefined) return;
            // password 字段：空字符串也不传（Edit时留空 = 不改Password）
            if (f.type === 'password' && v === '') return;
            body[f.key] = v;
          });
          if (editingId.value) await API.put(path + '/' + editingId.value, body);
          else await API.post(path, body);
          showModal.value = false; await load();
        } catch (e) { alert('Save failed: ' + e.message); }
        finally { saving.value = false; }
      }
      async function del(id) {
        if (!confirm(t('Confirm delete ID=') + id + '?')) return;
        try { await API.delete(path + '/' + id); await load(); }
        catch (e) { alert('Delete failed: ' + e.message); }
      }

      onMounted(() => {
        load(); loadAsyncOptions();
        // 监听Tenant切换，自动重新加载Current实体列表
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
            <span v-if="loading"><i class="fa-solid fa-spinner fa-spin"></i>{{ t(' Loading...') }}</span>
            <span v-else-if="error" class="text-red-500">⚠ {{ error }}</span>
            <span v-else>✓ Refreshed {{ lastUpdated }}</span>
            <button @click="load" class="px-2 py-1 border rounded hover:bg-gray-50">{{ t('Refresh') }}</button>
          </div>
        </div>
        <div v-if="!allRows.length && !loading" class="text-center text-gray-400 py-12">
          No data
        </div>
        <div v-else class="overflow-x-auto border rounded">
        <table class="w-full text-sm border-collapse min-w-[1400px]">
          <thead>
            <tr class="bg-gray-50 border-b">
              <th v-for="c in columns" :key="c.key" class="px-3 py-2 text-left font-medium text-gray-600 whitespace-nowrap">{{ c.label }}</th>
              <th v-if="actionCol" class="px-3 py-2 text-left font-medium text-gray-600 sticky right-0 bg-gray-50 border-l">{{ t('Action') }}</th>
            </tr>
          </thead>
          <tbody>
            <tr v-for="(row, idx) in pagedRows" :key="idx" class="border-b hover:bg-blue-50/50">
              <td v-for="c in columns" :key="c.key" class="px-3 py-2 font-mono text-xs text-gray-700 whitespace-nowrap">
                <template v-if="row[c.key] !== undefined && row[c.key] !== null && row[c.key] !== ''">
                  <template v-if="c.fmt">
                    <span v-html="c.fmt(row[c.key])"></span>
                  </template>
                  <template v-else>{{ row[c.key] }}</template>
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
        <!-- 分控件 -->
        <div v-if="allRows.length > pageSize" class="flex items-center justify-between mt-3 text-xs text-gray-500">
          <div>Total {{ allRows.length }}  · {{ pageSize }} per page </div>
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
      let inflight = false;          // in-flight 互斥：上一个 load 没回来不再发新请求
      let refreshTimer = null;       // SSE 去抖 timer

      const totalPages = computed(() => Math.max(1, Math.ceil(allRows.value.length / pageSize)));
      const pagedRows = computed(() => {
        const start = (currentPage.value - 1) * pageSize;
        return allRows.value.slice(start, start + pageSize);
      });

      async function load(silent) {
        if (inflight) return;        // 上一个还没回来，跳过
        inflight = true;
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
        finally {
          inflight = false;
          if (silent !== true) loading.value = false;
        }
      }

      function extractRows(data) {
        if (!data) return [];
        return data.rates || data.accounts || data.sessions || data.calls || data.cdrs
            || data.items  // 后端分结构
            || (data.metrics && Object.entries(data.metrics).map(([k,v]) => ({metric: k, value: v})))
            || (Array.isArray(data) ? data : []);
      }

      function scheduleRefresh() {
        if (refreshTimer) return;
        refreshTimer = setTimeout(() => {
          refreshTimer = null;
          load(true);
        }, 300);
      }

      onMounted(() => { load(); if (refreshMs > 0) timer = setInterval(() => load(true), refreshMs); });
      // nsbc-esl-web §8#1：SSE 推送触发静默Refresh（不闪 loading），与 refreshMs
      // 轮询构成 push+pull 双路径。300ms 去抖对齐全局 scheduleRefresh，
      // 避免 session 事件密集时各面板同时重拉打爆后端。
      onMounted(() => document.addEventListener('nsbc-refresh', scheduleRefresh));
      onBeforeUnmount(() => {
        if (timer) clearInterval(timer);
        clearTimeout(refreshTimer);
        document.removeEventListener('nsbc-refresh', scheduleRefresh);
      });
      return { allRows, loading, error, lastUpdated, load, columns, icon, currentPage, totalPages, pagedRows, actionCol };
    },
  };
}

// 所有 const / function 在经典 script 标签间自动Total享全局词法作用域，
// 后续 pages-crud.js / pages-live.js 可直接使用 app, crudTable, livePanel, API 等。

// ===== 全局Hang up函数（livePanel actionCol 用 v-html + onclick 调） =====
window.nsbcHangup = async function(callId) {
  if (!callId) return;
  if (!confirm(t('Confirm hang up call ') + callId + '?')) return;
  try {
    await API.post('/calls/' + encodeURIComponent(callId) + '/hangup', {});
    // 触发所有 livePanel Refresh
    document.dispatchEvent(new CustomEvent('nsbc-refresh'));
    alert('Hang up command sent');
  } catch (e) {
    alert('Hang up failed: ' + (e.message || e));
  }
};

// ===== SESSION 实时推送消费者（SSE，nsbc-esl-web §8#1）=====
// 双路径设计（符合项目「订阅推送 + 拉取」约束）：本消费者把服务端 /calls/stream
// 的 SESSION 事件转成本地 nsbc-refresh 事件即时Refresh livePanel；若 SSE 不可用，
// 各面板的 refreshMs 轮询自动兜底，无感降级。
// 说明：EventSource 无法携带 Authorization 头，故用 fetch + ReadableStream 手解析
// SSE 帧（event:/data: 行，空行分隔）；Connection lost按指数退避（1s→30s）重连。
(function () {
  const MAX_BACKOFF = 30000;
  let refreshTimer = null;
  let backoff = 1000;

  // 合并突发事件：一次Call会有 create/answer/hold/... 多个事件，300ms 去抖后
  // 只触发一次面板Refresh，避免高频重拉。
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
      console.warn(t('[live] SSE connect failed, retrying in ') + backoff + t('ms: '), e.message || e);
      setTimeout(streamLoop, backoff);
      backoff = Math.min(backoff * 2, MAX_BACKOFF);
      return;
    }
    backoff = 1000;  // Connected，Reset退避
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
      console.warn('[live] SSE read interrupted: ', e.message || e);
    }
    setTimeout(streamLoop, backoff);        // 断线重连
    backoff = Math.min(backoff * 2, MAX_BACKOFF);
  }

  // 延迟启动，等首帧/登录态就绪
  setTimeout(streamLoop, 1500);
})();


