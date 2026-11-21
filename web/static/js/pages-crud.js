// =============================================================================
// NSBC Web — CRUD 实体面组件
// 加载顺序：必须在 app-core.js 之后
// 依赖：app-core.js 中声明的全局 const app, crudTable
// =============================================================================

// --- Tenant ---
app.component('tenants', crudTable(
  { label: t('Tenant（SIP Realm）'), icon: 'fa-solid fa-building' },
  '/tenants',
  [
    { key: 'id', label: 'ID', type: 'text' },
    { key: 'tenant_name', label: 'SIP Realm', type: 'text' },
    { key: 'enabled', label: t('Status'), type: 'bool' },
  ],
  [
    { key: 'tenant_name', label: t('SIP Realm (Tenant name)'), type: 'text',
      help: 'SIP Auth Realm / Tenant unique id, default nsbc.local。Incoming call INVITE 的 From 域或ExtensionRegister realm 必须与此Value一致。如 "nsbc.local"、"acme.com"' },
    { key: 'enabled', label: t('Enable'), type: 'bool', default: true },
    { key: 'description', label: t('Notes'), type: 'textarea' },
    { key: 'match_domain', label: t('Incoming call From domain match'), type: 'text',
      placeholder: 'acme.com', help: t('Incoming call From domain matches → tenant (optional; empty = match by tenant_name/realm only)') },
    { key: 'match_did_prefix', label: t('Incoming call DID Number Prefix'), type: 'text',
      placeholder: '+86101234', help: t('Incoming callNumber starts with this prefix → tenant (optional)') },
    { key: 'match_trunk', label: t('Incoming callTrunk Name match'), type: 'text',
      placeholder: t('Carrier A'), help: t('Incoming callCalls using this trunk → tenant (optional)') },
  ],
));

// --- Extension ---
app.component('extensions', crudTable(
  { label: t('Extension'), icon: 'fa-solid fa-phone' },
  '/extensions',
  [
    { key: 'tenant_id', label: t('Tenant ID'), type: 'text' },
    { key: 'extension', label: t('Extension'), type: 'text' },
    { key: 'ext_type', label: t('Type'), type: 'text' },
    { key: 'full_name', label: t('Full Name'), type: 'text' },
    { key: 'enabled', label: t('Status'), type: 'bool' },
  ],
  [
    { key: 'tenant_id', label: t('Tenant'), type: 'async-select',
      help: 'Owner tenant (list auto-filtered by current tenant)' },
    { key: 'extension', label: t('Extension'), type: 'text' },
    { key: 'ext_type', label: t('Type'), type: 'select',
      options: [{value:'internal', label:t('Internal Extension (SIP Register)')},
                {value:'external', label:t('External Extension (outbound via trunk)')}],
      default: 'internal', help: t('Required for external') },
    { key: 'password', label: t('Password'), type: 'password', help: t('Leave empty if external extension does not register') },
    { key: 'full_name', label: t('Full Name'), type: 'text' },
    { key: 'enabled', label: t('Enable'), type: 'bool', default: true },
    { key: 'description', label: t('Notes'), type: 'textarea' },
    // 外部ExtensionConfig（ext_type=external 时有效）
    { key: 'external_number', label: t('External Number (E.164, e.g. +8613800138000)'), type: 'text',
      placeholder: '+8613800138000', help: t('Real outbound number') },
    { key: 'trunk_name', label: t('Outbound Trunk'), type: 'text',
      placeholder: '运营商IMS', help: t('走哪个 trunk 外呼（Web Trunk管理里的名字）') },
    { key: 'display_number', label: t('Caller显示号码（Caller ID）'), type: 'text',
      placeholder: '+861012345678', help: t('对端看到的Incoming号码（空=用 trunk Default）') },
  ],
  // asyncLoaders：动态下拉数据源
  {
    tenant_id: async () => {
      try {
        const r = await API.get('/tenants?limit=500&offset=0');
        const opts = (r.items || []).map(t => ({ value: t.id, label: t.tenant_name + (t.enabled ? '' : t(' (Disable)')) }));
        if (!opts.length) console.warn('[tenant_id async-loader] /tenants 返回空列表——检查 DB 是否有DefaultTenant');
        return opts;
      } catch (e) {
        console.error('[tenant_id async-loader] /tenants 加载失败:', e.message || e);
        return [];
      }
    },
  },
));

// --- Gateway ---
app.component('gateways', crudTable(
  { label: t('Gateway'), icon: 'fa-solid fa-tower-broadcast' },
  '/gateways',
  [
    { key: 'name', label: t('Name'), type: 'text' },
    { key: 'enabled', label: t('Status'), type: 'bool' },
    { key: 'description', label: t('Notes'), type: 'text' },
  ],
  [
    { key: 'name', label: t('Name'), type: 'text' },
    { key: 'enabled', label: t('Enable'), type: 'bool', default: true },
    { key: 'description', label: t('Notes'), type: 'textarea' },
  ],
));

// --- Trunk ---
app.component('trunks', crudTable(
  { label: t('Trunk'), icon: 'fa-solid fa-signal' },
  '/trunks',
  [
    { key: 'tenant_id', label: t('Tenant ID'), type: 'text' },
    { key: 'name', label: t('Name'), type: 'text' },
    { key: 'gateway_name', label: t('Gateway'), type: 'text' },
    { key: 'kind', label: t('Type'), type: 'text' },
    { key: 'host', label: '主机', type: 'text' },
    { key: 'port', label: t('Port'), type: 'text' },
    { key: 'enabled', label: t('Status'), type: 'bool' },
  ],
  [
    { key: 'tenant_id', label: t('Tenant'), type: 'async-select',
      help: 'Owner tenant (list auto-filtered by current tenant)' },
    { key: 'name', label: t('Name'), type: 'text' },
    { key: 'gateway_name', label: t('所属Gateway'), type: 'async-select',
      help: '从「Gateway」面Config的组（可留空，表示该Trunk不属任何组）' },
    { key: 'profile_name', label: 'SIP Profile', type: 'async-select', default: 'external',
      help: '从「SIP Profile」面Config（internal=Default, external=公网, local=回环）' },
    { key: 'kind', label: t('TrunkType'), type: 'select', options: [
      { value: 'static_peer', label: 'static_peer (静态对端)' },
      { value: 'register',    label: t('register (Register型)') },
      { value: 'auth_out',    label: 'auth_out (出局鉴权)' },
    ]},
    { key: 'proto', label: '协议', type: 'select', options: [
      { value: 0, label: 'UDP (0)' }, { value: 1, label: 'TCP (1)' }, { value: 2, label: 'TLS (2)' },
    ]},
    { key: 'host', label: '主机地址', type: 'text' },
    { key: 'port', label: t('Port'), type: 'number', default: 5060 },
    { key: 'auth_user', label: '鉴权用户名', type: 'text' },
    { key: 'password', label: t('鉴权Password'), type: 'text', editor: 'password' },
    { key: 'realm', label: 'Realm', type: 'text' },
    { key: 'codec_set', label: '可用编码（逗号分隔）', type: 'text',
      default: 'PCMA,PCMU' },
    { key: 'weight', label: t('Weight'), type: 'number', default: 10 },
    { key: 'priority', label: t('Priority'), type: 'number', default: 1 },
    { key: 'max_channels', label: t('最大并发（硬性d花板）'), type: 'number', default: 0 },
    { key: 'per_number_max_channels', label: '单号码最大并发', type: 'number', default: 0 },
    { key: 'caller_id_pool', label: '号码池（逗号分隔）', type: 'textarea',
      placeholder: '8613800000000,8613800000001,8613800000002' },
    { key: 'enabled', label: t('Enable'), type: 'bool', default: true },
    { key: 'description', label: t('Notes'), type: 'textarea' },
    { key: 'from_user', label: '[FS] From User（空=auth_user）', type: 'text', default: '' },
    { key: 'from_domain', label: 'From Domain（空=走回退链 realm→proxy→host）', type: 'text', default: '' },
    { key: 'proxy', label: '出局代理（空=同 host）', type: 'text', default: '' },
    { key: 'register_proxy', label: t('Register专用代理（空=同 proxy）'), type: 'text', default: '' },
    { key: 'expire_seconds', label: t('RegisterExpires（s）'), type: 'number', default: 3600 },
    { key: 'retry_seconds', label: t('退避基数（s，0=FS 语义不重试）'), type: 'number', default: 5 },
    { key: 'caller_id_in_from', label: 'caller_id_in_from（=1 只替换 From user part）', type: 'bool', default: false },
    { key: 'contact_params', label: t('Contact 额外Parameters（如 transport=udp）'), type: 'text', default: '' },
    { key: 'extension_in_contact', label: 'extension_in_contact（=1 把 inbound_extension 放入 Contact）', type: 'bool', default: false },
    { key: 'ping_seconds', label: 'OPTIONS 健康探测间隔（0=关闭）', type: 'number', default: 0 },
    { key: 'inbound_extension', label: t('入站 DID 映射Target（如 2001）'), type: 'text', default: '' },
    { key: 'cid_type', label: t('CID Type（预留 rpid 等）'), type: 'text', default: '' },
    { key: 'rfc5626_enabled', label: 'RFC 5626 Outbound（预留）', type: 'bool', default: false },
    { key: 'reg_id', label: 'reg-id（RFC 5626 预留）', type: 'text', default: '' },
    { key: 'peer_ip', label: 'peer_ip 白名单（CIDR 逗号分隔，auth_out 用）', type: 'textarea', default: '' },
    { key: 'ims_enabled', label: t('运营商 IMS Trunk（自动注入 PAI/PPID/PCPI 头）'), type: 'bool', default: false },
    { key: 'ims_domain', label: 'IMS 根域（如 ims.mnc001.mcc460.3gppnetwork.org）', type: 'text', default: '',
      placeholder: 'ims.mnc001.mcc460.3gppnetwork.org',
      help: '必填：开启 IMS 后，PAI/PPID/PCPI 会用 {auth_user}@{ims_domain} 拼 SIP URI' },
    { key: 'ims_auto_pani', label: t('自动注入 P-Access-Network-Info（Default E-UTRAN-TDD）'), type: 'bool', default: false },
    { key: 'ims_pani_value', label: t('自定义 PANI Value（空=E-UTRAN-TDD）'), type: 'text', default: '',
      placeholder: 'E-UTRAN-TDD; utran-cell-id-3gpp=0001001' },
    { key: 'ims_auto_privacy', label: '自动注入 Privacy: id（CLIR，对端 From 匿名）', type: 'bool', default: false },
    { key: 'custom_headers', label: '自定义 SIP 头（出站 INVITE）', type: 'textarea', default: '',
      placeholder: 'MyID=${callid}\r\nX-Biz-Caller=${caller}\r\nX-Biz-Callee=${callee}',
      help: '每行一，必须带 \\r\\n 结尾。可用变量：${callid} ${caller} ${callee} ${from_user} ${to_user} ${from_ip} ${to_ip}' },
  ],
  // asyncLoaders：动态下拉数据源
  {
    tenant_id: async () => {
      try {
        const r = await API.get('/tenants?limit=500&offset=0');
        const opts = (r.items || []).map(t => ({ value: t.id, label: t.tenant_name + (t.enabled ? '' : t(' (Disable)')) }));
        if (!opts.length) console.warn('[trunks tenant_id] /tenants 返回空列表');
        return opts;
      } catch (e) {
        console.error('[trunks tenant_id] /tenants 加载失败:', e.message || e);
        return [];
      }
    },
    gateway_name: async () => {
      try {
        const r = await API.get('/gateways?limit=500&offset=0');
        return (r.items || []).map(g => ({ value: g.name, label: `${g.name}${g.enabled ? '' : t(' (Disable)')}` }));
      } catch (e) { return []; }
    },
    profile_name: async () => {
      try {
        const r = await API.get('/sip-profiles?limit=500&offset=0');
        return (r.items || []).map(p => ({ value: p.name, label: `${p.name}${p.enabled ? '' : t(' (Disable)')}` }));
      } catch (e) { return []; }
    },
  },
));

// --- 路由组 ---
app.component('route-groups', crudTable(
  { label: '路由组', icon: 'fa-solid fa-route' },
  '/routes',
  [
    { key: 'name', label: t('Name'), type: 'text' },
    { key: 'strategy', label: '策略', type: 'text' },
    { key: 'enabled', label: t('Status'), type: 'bool' },
  ],
  [
    { key: 'name', label: t('Name'), type: 'text' },
    { key: 'strategy', label: '选路策略', type: 'select', options: [
      { value: 'priority', label: 'priority (主备优先)' },
      { value: 'weight',   label: 'weight (加权散列)' },
      { value: 'least_busy', label: 'least_busy (最闲优先)' },
      { value: 'ring',     label: 'ring (轮询)' },
    ], default: 'priority' },
    { key: 'enabled', label: t('Enable'), type: 'bool', default: true },
    { key: 'description', label: t('Notes'), type: 'textarea' },
  ],
));

/* §15 Dialplan Rules：带 internal/external Profile Tab 分区（= FreeSWITCH default.xml vs public.xml）
 * CIDR DefaultValue：internal/external 都默认 0.0.0.0/0 放行（安全边界在 auth/profile 层，CIDR 是额外闸门） */
app.component('dialplan-rules', {
  template: `
    <div class="space-y-4">
      <div class="flex items-center justify-between">
        <h2 class="text-lg font-semibold text-slate-800"><i class="fa-solid fa-code-branch mr-2 text-blue-600"></i>{{ t('Dialplan Rules') }}</h2>
        <div class="flex items-center gap-2">
          <el-button size="small" @click="reloadRules">
            <el-icon><Refresh /></el-icon>Reload 到 C 侧
          </el-button>
          <el-button type="primary" size="small" @click="openCreate">
            <el-icon><Plus /></el-icon>新建
          </el-button>
        </div>
      </div>

      <!-- Profile Tab 分区（= FreeSWITCH context） -->
      <el-tabs v-model="activeProfile" type="border-card">
        <el-tab-pane label="📥 Internal (SIP → internal)" name="internal">
          <el-table :data="pagedItems" border stripe size="default" style="width:100%">
            <el-table-column type="index" label="#" width="55" align="center"></el-table-column>
            <el-table-column prop="tenant_id" label="Tenant" width="80" align="center">
              <template #default="{ row }"><el-tag size="small" effect="plain">T{{ row.tenant_id || 1 }}</el-tag></template>
            </el-table-column>
            <el-table-column prop="name" label="Name" min-width="140"></el-table-column>
            <el-table-column prop="match_expr" label="Match Expression" min-width="140">
              <template #default="{ row }"><code class="text-xs bg-slate-100 px-1 rounded">{{ row.match_expr }}</code></template>
            </el-table-column>
            <el-table-column prop="target" label="Target" min-width="160"></el-table-column>
            <el-table-column prop="rec_action" label="录音" width="90" align="center">
              <template #default="{ row }">
                <el-tag v-if="row.rec_action === 1" type="danger" effect="light" size="small" round>Start</el-tag>
                <el-tag v-else-if="row.rec_action === 2" type="warning" effect="light" size="small" round>Stop</el-tag>
                <span v-else class="text-slate-400 text-xs">—</span>
              </template>
            </el-table-column>
            <el-table-column prop="rec_path_template" label="录音路径模板" min-width="200">
              <template #default="{ row }">
                <code v-if="row.rec_path_template" class="text-xs text-emerald-600">{{ row.rec_path_template }}</code>
                <span v-else class="text-slate-400 text-xs">{{ t('Default') }}</span>
              </template>
            </el-table-column>
            <el-table-column prop="rec_fmt" label="录音格式" width="110" align="center">
              <template #default="{ row }">
                <el-tag v-if="row.rec_fmt" :type="row.rec_fmt === 'mp4' ? 'danger' : 'info'" effect="plain" size="small" round>{{ row.rec_fmt }}</el-tag>
                <span v-else class="text-slate-400 text-xs">全局</span>
              </template>
            </el-table-column>
            <el-table-column prop="prio" label="Priority" width="80" align="center"></el-table-column>
            <el-table-column prop="src_ip_cidr" label="IP 准入 CIDR" min-width="180">
              <template #default="{ row }">
                <code class="text-xs" :class="row.src_ip_cidr === '0.0.0.0/0' ? 'text-amber-600' : 'text-slate-500'">{{ row.src_ip_cidr }}</code>
              </template>
            </el-table-column>
            <el-table-column prop="enabled" label="Enabled" width="80" align="center">
              <template #default="{ row }">
                <el-tag :type="row.enabled ? 'success' : 'info'" effect="light" size="small" round>
                  {{ row.enabled ? t('Enable') : t('Disable') }}
                </el-tag>
              </template>
            </el-table-column>
            <el-table-column label="Actions" width="150" align="center" fixed="right">
              <template #default="{ row }">
                <el-button link type="primary" size="small" @click="openEdit(row)">{{ t('Edit') }}</el-button>
                <el-popconfirm title="Delete this rule?" @confirm="del(row.id)">
                  <template #reference><el-button link type="danger" size="small">{{ t('Delete') }}</el-button></template>
                </el-popconfirm>
              </template>
            </el-table-column>
          </el-table>
          <div class="flex justify-end pt-2">
            <el-pagination v-model:current-page="page" :page-size="limit" :total="filteredTotal"
              layout="total, prev, pager, next" small background></el-pagination>
          </div>
        </el-tab-pane>

        <el-tab-pane label="📤 External (Gateway/Trunk → internal)" name="external">
          <el-table :data="pagedItems" border stripe size="default" style="width:100%">
            <el-table-column type="index" label="#" width="55" align="center"></el-table-column>
            <el-table-column prop="tenant_id" label="Tenant" width="80" align="center">
              <template #default="{ row }"><el-tag size="small" effect="plain">T{{ row.tenant_id || 1 }}</el-tag></template>
            </el-table-column>
            <el-table-column prop="name" label="Name" min-width="140"></el-table-column>
            <el-table-column prop="match_expr" label="Match Expression" min-width="140">
              <template #default="{ row }"><code class="text-xs bg-slate-100 px-1 rounded">{{ row.match_expr }}</code></template>
            </el-table-column>
            <el-table-column prop="target" label="Target" min-width="160"></el-table-column>
            <el-table-column prop="rec_action" label="录音" width="90" align="center">
              <template #default="{ row }">
                <el-tag v-if="row.rec_action === 1" type="danger" effect="light" size="small" round>Start</el-tag>
                <el-tag v-else-if="row.rec_action === 2" type="warning" effect="light" size="small" round>Stop</el-tag>
                <span v-else class="text-slate-400 text-xs">—</span>
              </template>
            </el-table-column>
            <el-table-column prop="rec_path_template" label="录音路径模板" min-width="200">
              <template #default="{ row }">
                <code v-if="row.rec_path_template" class="text-xs text-emerald-600">{{ row.rec_path_template }}</code>
                <span v-else class="text-slate-400 text-xs">{{ t('Default') }}</span>
              </template>
            </el-table-column>
            <el-table-column prop="rec_fmt" label="录音格式" width="110" align="center">
              <template #default="{ row }">
                <el-tag v-if="row.rec_fmt" :type="row.rec_fmt === 'mp4' ? 'danger' : 'info'" effect="plain" size="small" round>{{ row.rec_fmt }}</el-tag>
                <span v-else class="text-slate-400 text-xs">全局</span>
              </template>
            </el-table-column>
            <el-table-column prop="prio" label="Priority" width="80" align="center"></el-table-column>
            <el-table-column prop="src_ip_cidr" label="IP 准入 CIDR" min-width="180">
              <template #default="{ row }">
                <code class="text-xs" :class="row.src_ip_cidr === '0.0.0.0/0' ? 'text-amber-600' : 'text-slate-500'">{{ row.src_ip_cidr }}</code>
              </template>
            </el-table-column>
            <el-table-column prop="enabled" label="Enabled" width="80" align="center">
              <template #default="{ row }">
                <el-tag :type="row.enabled ? 'success' : 'info'" effect="light" size="small" round>
                  {{ row.enabled ? t('Enable') : t('Disable') }}
                </el-tag>
              </template>
            </el-table-column>
            <el-table-column label="Actions" width="150" align="center" fixed="right">
              <template #default="{ row }">
                <el-button link type="primary" size="small" @click="openEdit(row)">{{ t('Edit') }}</el-button>
                <el-popconfirm title="Delete this rule?" @confirm="del(row.id)">
                  <template #reference><el-button link type="danger" size="small">{{ t('Delete') }}</el-button></template>
                </el-popconfirm>
              </template>
            </el-table-column>
          </el-table>
          <div class="flex justify-end pt-2">
            <el-pagination v-model:current-page="page" :page-size="limit" :total="filteredTotal"
              layout="total, prev, pager, next" small background></el-pagination>
          </div>
        </el-tab-pane>
      </el-tabs>

      <!-- 新建/Edit弹窗 -->
      <el-dialog v-model="showModal" :title="editingId ? 'Edit Dialplan Rule' : 'New Dialplan Rule'" width="900px" destroy-on-close>
        <el-form tag="div" :model="form" label-position="top">
          <el-form-item label="所属Tenant">
            <el-select v-model="form.tenant_id" style="width:100%" filterable>
              <el-option v-for="t in tenantOptions" :key="t.id" :value="t.id"
                :label="'T' + t.id + ' — ' + t.tenant_name + (t.enabled ? '' : ' (停用)')">
              </el-option>
            </el-select>
            <div class="text-xs text-slate-400 mt-1">{{ t('跨Tenant规则互相隔离，Default选 T1 (default)') }}</div>
          </el-form-item>
          <el-form-item label="Rule Name" required><el-input v-model="form.name" placeholder="例：DID 51350820 → 内部 alice"></el-input></el-form-item>
          <el-form-item label="所属 Profile（= FreeSWITCH context）">
            <el-select v-model="form.profile_name" style="width:100%" @change="onProfileChange">
              <el-option value="internal" label="internal (SIP → internal)"></el-option>
              <el-option value="external" label="external (Gateway/Trunk → internal)"></el-option>
            </el-select>
            <div class="text-xs text-slate-400 mt-1">internal = default.xml；external = public.xml，两个 context 规则完全隔离</div>
          </el-form-item>
          <el-form-item :label="'源 IP 准入 CIDR（多网段逗号分隔）' + (form.profile_name === 'external' ? ' — 外部Gateway请填运营商 IP' : ' — 默认 0.0.0.0/0 放行，生产建议按规则收紧')">
            <el-input v-model="form.src_ip_cidr" placeholder="0.0.0.0/0 或 10.0.0.0/8,192.168.0.0/16 等"></el-input>
          </el-form-item>
          <el-form-item label="CalleeMatch Expression">
            <div class="flex gap-2 items-start w-full">
              <el-input v-model="form.match_expr" placeholder="正则 / 通配符：^1[01]\d{2}$ / 100[0-9] / 51350820" style="flex:1"></el-input>
              <el-button @click="validateMatchExpr" :type="matchValid === null ? 'info' : (matchValid ? 'success' : 'danger')">{{ t('Validate Expression') }}</el-button>
            </div>
            <div v-if="matchValid !== null" class="mt-1 text-xs" :class="matchValid ? 'text-green-600' : 'text-red-500'">
              <span v-if="matchValid">{{ t('✓ Match Expression合法（可用正则语法）') }}</span>
              <span v-else>✗ Match Expression有问题：{{ matchError }}</span>
            </div>
            <div v-if="matchValid" class="mt-1 text-xs text-slate-500">
              <div class="flex gap-2 items-center mt-1">
                <span>Test Number：</span>
                <el-input v-model="matchTestNum" size="small" style="width:160px" placeholder="如 11001"></el-input>
                <el-tag v-if="matchTestResult !== null" :type="matchTestResult ? 'success' : 'info'" size="small">
                  {{ matchTestResult ? t('✓ 能Match') : t('✗ 不Match') }}
                </el-tag>
              </div>
            </div>
          </el-form-item>
          <el-form-item label="Caller Match (optional)"><el-input v-model="form.caller_match" placeholder="e.g. caller_id_number=^6202 ; empty = match any"></el-input></el-form-item>
          <div class="grid grid-cols-2 gap-4">
            <el-form-item label="同 Profile 内Priority（小Value优先）">
              <el-input-number v-model="form.prio" :controls="false" style="width:100%"></el-input-number>
            </el-form-item>
            <el-form-item label="去前导位数（strip）">
              <el-input-number v-model="form.strip" :controls="false" style="width:100%"></el-input-number>
            </el-form-item>
          </div>
          <el-form-item label="Callee覆盖（dst_override）">
            <el-input v-model="form.dst_override"
                      placeholder="空=strip 结果；可用 $1 $2 或 \${1} \${2}；如 888$2"
                      clearable>
            </el-input>
            <div class="text-xs text-slate-400 mt-1">
              当Callee变换不是简单"去前导"时用这个；正则捕获组两种语法都支持：
              <code class="bg-slate-100 px-1 rounded">$1</code>
              <code class="bg-slate-100 px-1 rounded mx-1">$2</code>
              <span class="mx-1">或</span>
              <code class="bg-slate-100 px-1 rounded">\${1}</code>
              <code class="bg-slate-100 px-1 rounded mx-1">\${2}</code>。
              Priority：<b>dst_override</b> > strip > add_prefix。
            </div>
          </el-form-item>
          <el-form-item label="Target资源（只管路由）">
            <el-select v-model="form.target" filterable allow-create default-first-option
              placeholder="Select target..." style="width:100%" clearable>
              <el-option-group v-if="gatewayOptions.length" label="— GATEWAY —">
                <el-option v-for="o in gatewayOptions" :key="o.value" :value="o.value" :label="o.label"></el-option>
              </el-option-group>
              <el-option-group v-if="trunkOptions.length" label="— TRUNK —">
                <el-option v-for="o in trunkOptions" :key="o.value" :value="o.value" :label="o.label"></el-option>
              </el-option-group>
              <el-option-group v-if="queueOptions.length" label="— QUEUE 分机队列 —">
                <el-option v-for="o in queueOptions" :key="o.value" :value="o.value" :label="o.label"></el-option>
              </el-option-group>
              <el-option-group v-if="trunkApiOk && !trunkOptions.length && !gatewayOptions.length" label="— OUT —">
                <el-option disabled value="" label="(none available)"></el-option>
              </el-option-group>
              <el-option-group label="— EXT Extension / BIZ 业务 —">
                <el-option value="extension:" label="extension:<extension> → BIZ 内部分机"></el-option>
                <el-option value="queue:"     label="queue:<队列> → BIZ 队列"></el-option>
                <el-option value="ivr:"       label="ivr:<IVR> → BIZ 语音菜单"></el-option>
                <el-option value="hangup"     label="hangup → BIZ 挂机"></el-option>
              </el-option-group>
              <el-option-group label="— LOCAL 本地 —">
                <el-option value="local:ai"   label="local:ai → 🤖 AI 对话 (走 ai_backends 表)"></el-option>
              </el-option-group>
            </el-select>
            <div class="text-xs text-slate-400 mt-1">
              <b>★ 设计解耦</b>：<b>{{ t('Target资源只管路由') }}</b>{{ t('，Callee号码变换（strip/\${N}/dst_override）在下面「拨号Action」里独立Config。避免笛卡尔积爆炸。') }}<br>
              <b>{{ t('e.g.') }}</b>：match=<code>^(\\d{3,6})(\\d{11})$</code>，target=gateway:all_trunks，<b>dst_override=\${2}</b> → 外呼 捕获组2@all_trunks ✅<br>
              <b>队列</b>：target=<code>queue:队列名</code>，按队列策略选成员振铃；strip/dst_override 不参与队列选路。
            </div>
          </el-form-item>
          <div class="grid grid-cols-2 gap-4">
            <el-form-item label="录音控制（本规则命中时触发）">
              <el-select v-model="form.rec_action" style="width:100%">
                <el-option :value="0" label="Never"></el-option>
                <el-option :value="1" label="Start — 命中后开始录音"></el-option>
                <el-option :value="2" label="Stop — 命中后停止录音"></el-option>
              </el-select>
              <div class="text-xs text-slate-400 mt-1">{{ t('每规则独立Config，下一规则可不同；0=none 1=start 2=stop') }}</div>
            </el-form-item>
            <el-form-item label="Media模式（本规则路由时）">
              <el-select v-model="form.media_mode" style="width:100%">
                <el-option :value="0" label="Auto — 自动决策"></el-option>
                <el-option :value="1" label="Bypass — 媒体直连绕过 nsbc"></el-option>
                <el-option :value="2" label="Relay — nsbc 锚定媒体（H.264 视频录制必需）"></el-option>
              </el-select>
              <div class="text-xs text-slate-400 mt-1">
                WebRTC 通话自动走 SRTP_RELAY；BYPASS 模式媒体不经 nsbc 锚定，<b>不支持视频录制</b>。
              </div>
            </el-form-item>
          </div>
          <el-form-item label="录音格式（本规则独立配置）">
            <el-select v-model="form.rec_fmt" style="width:100%" clearable placeholder="跟随全局 recording.format">
              <el-option label="（空）跟随全局默认" value=""></el-option>
              <el-option-group label="音频格式">
                <el-option value="wav"  label="wav · 原生 PCM 无依赖，体积最大"></el-option>
                <el-option value="mp3"  label="mp3 · 通用播放器兼容 128kbps"></el-option>
                <el-option value="opus" label="opus · 省空间（约 wav 1/10）"></el-option>
              </el-option-group>
              <el-option-group label="音视频合成">
                <el-option value="mp4"  label="mp4 · AAC 音频 + H.264 视频浏览器直放"></el-option>
              </el-option-group>
            </el-select>
            <div class="text-xs text-slate-400 mt-1">
              优先级：本规则配置 <b>></b> 系统 <code>recording.format</code> <b>></b> wav 兜底。
              mp4 格式自动将 A/B 腿语音 + 视频合成到同一个 mp4 文件。
            </div>
          </el-form-item>
          <el-form-item label="录音路径模板（空=系统Default）">
            <el-input v-model="form.rec_path_template"
                      placeholder="如 YYYY-MM-DD/{caller}-{callee}-{callid}.wav"
                      clearable>
            </el-input>
            <div class="text-xs text-slate-400 mt-1">
              支持变量：<code class="bg-slate-100 px-1 rounded">{caller}</code>
              <code class="bg-slate-100 px-1 rounded mx-1">{callee}</code>
              <code class="bg-slate-100 px-1 rounded">{callid}</code>
              <code class="bg-slate-100 px-1 rounded mx-1">{date}</code>
              <code class="bg-slate-100 px-1 rounded">{time}</code>
              。空模板时系统用 <code class="bg-slate-100 px-1 rounded">yyyymmdd/callid_mix_seq.wav</code>
            </div>
          </el-form-item>
          <!-- 3GPP/IMS 强制覆盖（三态）：0=自动继承 profile 1=强制开启 2=强制关闭 -->
          <el-divider content-position="left">
            <i class="fas fa-signal mr-1 text-purple-500"></i>
            {{ t('IMS Capability Override (3-state)') }}
          </el-divider>
          <div class="grid grid-cols-2 gap-4">
            <el-form-item :label="t('100rel Force')">
              <el-select v-model="form.force_prack" style="width:100%">
                <el-option :value="0" :label="t('Auto (inherit profile)')"></el-option>
                <el-option :value="1" :label="t('Force ON')"></el-option>
                <el-option :value="2" :label="t('Force OFF')"></el-option>
              </el-select>
            </el-form-item>
            <el-form-item :label="t('Precondition Force')">
              <el-select v-model="form.force_precondition" style="width:100%">
                <el-option :value="0" :label="t('Auto (inherit profile)')"></el-option>
                <el-option :value="1" :label="t('Force ON')"></el-option>
                <el-option :value="2" :label="t('Force OFF')"></el-option>
              </el-select>
            </el-form-item>
            <el-form-item :label="t('Session Timer Force')">
              <el-select v-model="form.force_timer" style="width:100%">
                <el-option :value="0" :label="t('Auto (inherit profile)')"></el-option>
                <el-option :value="1" :label="t('Force ON')"></el-option>
                <el-option :value="2" :label="t('Force OFF')"></el-option>
              </el-select>
            </el-form-item>
            <el-form-item :label="t('GRUU Force')">
              <el-select v-model="form.force_gruu" style="width:100%">
                <el-option :value="0" :label="t('Auto (inherit profile)')"></el-option>
                <el-option :value="1" :label="t('Force ON')"></el-option>
                <el-option :value="2" :label="t('Force OFF')"></el-option>
              </el-select>
            </el-form-item>
          </el-divider>
          <div class="grid grid-cols-2 gap-4">
            <el-form-item label="接通后最大通话时长 (秒)">
              <el-input-number v-model="form.sched_hangup_s" :min="0" :max="86400" :step="60"
                controls-position="right" style="width:100%"
                :placeholder="t('0 = 不限 / 走全局')"></el-input-number>
              <div class="form-hint">REQ-DP-TIMEOUT-001：双腿 confirm 后强制挂断（0=Session Timer 继续生效）</div>
            </el-form-item>
            <el-form-item label="B-leg 等待接通超时 (秒)">
              <el-input-number v-model="form.originate_call_timeout_s" :min="0" :max="3600" :step="5"
                controls-position="right" style="width:100%"
                :placeholder="t('0 = 走全局默认 50s')"></el-input-number>
              <div class="form-hint">REQ-DP-TIMEOUT-001：originate 创建的 call 发起 B-leg INVITE 后等待 answer 的超时</div>
            </el-form-item>
          </div>
          <el-form-item label="Enable">
            <el-switch v-model="form.enabled" active-text="ON" inactive-text="OFF"></el-switch>
          </el-form-item>
          <el-form-item label="Description"><el-input v-model="form.description" type="textarea" :rows="2"></el-input></el-form-item>
          <el-form-item label="拨号Action（SIP 头改写 / 号码变换）">
            <div class="w-full">
              <div class="flex justify-between items-center mb-1">
                <span class="text-xs text-slate-500">结构化数据（点击切换 Raw JSON）</span>
                <el-button link size="small" type="primary" @click="rawMode.action_json = !rawMode.action_json">
                  {{ rawMode.action_json ? t('结构化Edit') : 'Raw JSON' }}
                </el-button>
              </div>
              <el-input v-if="rawMode.action_json" v-model="form.action_json" type="textarea" :rows="6"
                placeholder='有效 JSON，例如 {"clip":"44","sip_invite_from_uri":"sip:alice@nsbc.local"}' resize="none"></el-input>
              <dp-action-editor v-else v-model="form.action_json"></dp-action-editor>
            </div>
            <div class="text-xs text-slate-400 mt-1">
              Settings SIP INVITE 出站头改写规则（对齐 FreeSWITCH sofia_glue.c 原生变量）。Save后点"Reload 到 C 侧"生效。
            </div>
          </el-form-item>
          <!-- REQ-DP-APP-001: Application 链Edit器 -->
          <el-form-item label="拨号Application Chain（Application Chain）">
            <div class="w-full">
              <div class="flex justify-between items-center mb-1">
                <span class="text-xs text-slate-500">顺序执行：pre_answer → playback → sleep → answer …（空链则跳过）</span>
                <el-button link size="small" type="primary" @click="rawMode.app_chain = !rawMode.app_chain">
                  {{ rawMode.app_chain ? t('结构化Edit') : 'Raw JSON' }}
                </el-button>
              </div>
              <el-input v-if="rawMode.app_chain" v-model="form.app_chain" type="textarea" :rows="6"
                placeholder='JSON 数组，例如 [{"app":"pre_answer"},{"app":"playback","data":"welcome.wav"},{"app":"answer"}]' resize="none"></el-input>
              <dp-app-chain-editor v-else v-model="form.app_chain"></dp-app-chain-editor>
            </div>
            <div class="text-xs text-slate-400 mt-1">
              选中命中本规则后依次执行的 Application。C 侧 dp_execute 命中规则 → dp_execute_app_chain 逐调 app_registry 中Register的 17 个内建 apps。
              <code class="bg-slate-100 px-1 rounded ml-1">sleep</code> 返回 PENDING 时 timer_wheel 挂起，到点后自动继续。
            </div>
          </el-form-item>
          <!-- ★ 完整 Rule JSON 双向同步预览/编辑框 -->
          <el-divider content-position="left">
            <i class="fas fa-code mr-1 text-indigo-500"></i>
            Full Rule JSON（双向同步 · 直接改这里也能更新上方条目）
          </el-divider>
          <el-form-item label="">
            <div class="w-full">
              <div class="flex justify-between items-center mb-1">
                <span class="text-xs text-slate-500">
                  结构化条目 ↔ 完整 JSON 实时同步 · 保存时也会校验 JSON 合法性
                </span>
                <div class="flex gap-1">
                  <el-button link size="small" @click="ruleJsonSyncFromForm" type="primary" v-if="!ruleJsonUserEdited">
                    ↻ 从条目同步
                  </el-button>
                  <el-button link size="small" @click="ruleJsonFormat">格式化</el-button>
                  <el-button link size="small" @click="ruleJsonCopy">复制</el-button>
                </div>
              </div>
              <el-input v-model="ruleJson" type="textarea" :rows="12"
                placeholder='{"name":"...","profile_name":"internal","match_expr":"^1000$",...}' resize="none"
                @input="ruleJsonOnInput" @blur="ruleJsonOnBlur">
              </el-input>
              <div class="text-xs mt-1" :class="ruleJsonValid ? 'text-green-600' : 'text-red-500'">
                <span v-if="ruleJsonValid">✓ JSON 合法</span>
                <span v-else>✗ JSON 不合法：{{ ruleJsonError }}</span>
              </div>
            </div>
          </el-form-item>
        </el-form>
        <template #footer>
          <el-button @click="showModal = false">{{ t('Cancel') }}</el-button>
          <el-button type="primary" :loading="saving" @click="save">{{ t('Save') }}</el-button>
        </template>
      </el-dialog>
    </div>
  `,
  setup() {
    const API_PATH = '/dialplan-rules';
    const RFC1918 = '0.0.0.0/0';
    const { ref, onMounted } = Vue;

    const items = ref([]);
    const page = ref(1);
    const limit = ref(50);
    const activeProfile = ref('internal');
    const showModal = ref(false);
    const editingId = ref(null);
    const form = ref({});
    const saving = ref(false);
    const rawMode = Vue.reactive({ action_json: false, app_chain: false });
    // ★ 完整 Rule JSON 双向同步
    const ruleJson = ref('');
    const ruleJsonValid = ref(true);
    const ruleJsonError = ref('');
    const ruleJsonUserEdited = ref(false); // 用户手动改过 JSON（blur 时反向解析回条目）
    let ruleJsonTimer = null;
    const ruleJsonFromForm = () => {
      const f = form.value;
      const obj = {
        name: f.name || '',
        profile_name: f.profile_name || 'internal',
        src_ip_cidr: f.src_ip_cidr || '0.0.0.0/0',
        match_expr: f.match_expr || '',
        caller_match: f.caller_match || '',
        prio: f.prio ?? 1,
        strip: f.strip ?? 0,
        target: f.target || '',
        rec_action: f.rec_action ?? 0,
        rec_path_template: f.rec_path_template || '',
        rec_fmt: f.rec_fmt || '',
        force_prack: f.force_prack ?? 0,
        force_precondition: f.force_precondition ?? 0,
        force_timer: f.force_timer ?? 0,
        force_gruu: f.force_gruu ?? 0,
        sched_hangup_s: f.sched_hangup_s ?? 0,
        originate_call_timeout_s: f.originate_call_timeout_s ?? 0,
        action_json: toJsonObj(f.action_json || '{}'),
        app_chain: (() => {
          try { return typeof f.app_chain === 'string' ? JSON.parse(f.app_chain || '[]') : f.app_chain; }
          catch { return []; }
        })(),
        enabled: f.enabled !== false,
        description: f.description || '',
      };
      try {
        ruleJson.value = JSON.stringify(obj, null, 2);
        ruleJsonValid.value = true;
        ruleJsonError.value = '';
      } catch (e) { /* ignore */ }
    };
    function ruleJsonOnInput() {
      ruleJsonUserEdited.value = true;
      // 延迟 400ms 自动校验
      if (ruleJsonTimer) clearTimeout(ruleJsonTimer);
      ruleJsonTimer = setTimeout(() => {
        try {
          JSON.parse(ruleJson.value);
          ruleJsonValid.value = true;
          ruleJsonError.value = '';
        } catch (e) {
          ruleJsonValid.value = false;
          ruleJsonError.value = e.message;
        }
      }, 400);
    }
    function ruleJsonOnBlur() {
      if (!ruleJsonUserEdited.value) return;
      // 用户停止编辑后：JSON 合法 → 反向解析回 form 条目
      let obj;
      try { obj = JSON.parse(ruleJson.value); }
      catch { ruleJsonValid.value = false; return; }
      ruleJsonValid.value = true;
      ruleJsonError.value = '';
      // 反向 merge 回 form（只覆盖 JSON 里有的字段）
      const f = form.value;
      if (obj.name !== undefined) f.name = obj.name;
      if (obj.profile_name !== undefined) f.profile_name = obj.profile_name;
      if (obj.src_ip_cidr !== undefined) f.src_ip_cidr = obj.src_ip_cidr;
      if (obj.match_expr !== undefined) f.match_expr = obj.match_expr;
      if (obj.caller_match !== undefined) f.caller_match = obj.caller_match;
      if (obj.prio !== undefined) f.prio = Number(obj.prio);
      if (obj.strip !== undefined) f.strip = Number(obj.strip);
      if (obj.target !== undefined) f.target = obj.target;
      if (obj.rec_action !== undefined) f.rec_action = Number(obj.rec_action);
      if (obj.rec_path_template !== undefined) f.rec_path_template = obj.rec_path_template;
      if (obj.rec_fmt !== undefined) f.rec_fmt = obj.rec_fmt;
      if (obj.force_prack !== undefined) f.force_prack = Number(obj.force_prack);
      if (obj.force_precondition !== undefined) f.force_precondition = Number(obj.force_precondition);
      if (obj.force_timer !== undefined) f.force_timer = Number(obj.force_timer);
      if (obj.force_gruu !== undefined) f.force_gruu = Number(obj.force_gruu);
      if (obj.sched_hangup_s !== undefined) f.sched_hangup_s = Number(obj.sched_hangup_s);
      if (obj.originate_call_timeout_s !== undefined) f.originate_call_timeout_s = Number(obj.originate_call_timeout_s);
      if (obj.action_json !== undefined) f.action_json = typeof obj.action_json === 'string'
        ? obj.action_json : JSON.stringify(obj.action_json);
      if (obj.app_chain !== undefined) f.app_chain = typeof obj.app_chain === 'string'
        ? obj.app_chain : JSON.stringify(obj.app_chain);
      if (obj.enabled !== undefined) f.enabled = !!obj.enabled;
      if (obj.description !== undefined) f.description = obj.description;
      // 同步一下 form → JSON 避免循环不一致
      ruleJsonUserEdited.value = false;
      ruleJsonFromForm();
      ruleJsonUserEdited.value = true; // 保持"用户正在编辑"的UI状态，直到用户点保存或切换
    }
    function ruleJsonSyncFromForm() {
      ruleJsonUserEdited.value = false;
      ruleJsonFromForm();
    }
    function ruleJsonFormat() {
      let obj; try { obj = JSON.parse(ruleJson.value); }
      catch { ElementPlus.ElMessage({ message: 'JSON 不合法，无法格式化', type: 'error' }); return; }
      ruleJson.value = JSON.stringify(obj, null, 2);
    }
    function ruleJsonCopy() {
      navigator.clipboard?.writeText(ruleJson.value).then(() =>
        ElementPlus.ElMessage({ message: '已复制到剪贴板', type: 'success' })
      ).catch(() =>
        ElementPlus.ElMessage({ message: '复制失败（请手动 Ctrl+C）', type: 'warning' })
      );
    }
    // ★ 表单字段变化 → 自动同步 JSON（但用户手动编辑 JSON 期间暂停，避免覆盖）
    const syncFields = [
      'name','profile_name','src_ip_cidr','match_expr','caller_match',
      'prio','strip','target','rec_action','rec_path_template','rec_fmt',
      'force_prack','force_precondition','force_timer','force_gruu',
      'sched_hangup_s','originate_call_timeout_s',
      'action_json','app_chain','enabled','description',
    ];
    Vue.watch(
      () => syncFields.map(k => form.value[k]),
      () => {
        if (!showModal.value) return;          // 弹窗关闭时不触发
        if (ruleJsonUserEdited.value) return;  // 用户在手动编辑 JSON → 不覆盖
        ruleJsonFromForm();
      },
      { deep: true }
    );
    // 正则验证Status
    const matchValid = ref(null);    // null=未验证, true=合法, false=非法
    const matchError = ref('');
    const matchTestNum = ref('');
    const matchTestResult = ref(null); // null=未测试, true=能Match, false=不Match

    // 外呼下拉选项（纯路由，不再自动拼 |${N} 组合——与 dst_override 解耦）
    const trunkOptions = ref([]);     // [{ value: 'trunk:xxx', label }]
    const gatewayOptions = ref([]);    // [{ value: 'gateway:xxx', label }]
    const queueOptions = ref([]);      // [{ value: 'queue:xxx', label }]
    const trunkApiOk = ref(false);
    // Tenant下拉（tenant_id 外Key）
    const tenantOptions = ref([]);    // [{ id, tenant_name, enabled }]

    async function fetchTenants() {
      try {
        const r = await API.get('/tenants');
        tenantOptions.value = r.items || [];
      } catch (e) { /* 静默——tenants 接口 503 时后端还没配 DB */ }
    }

    async function fetchTrunkOptions() {
      try {
        const r = await API.get('/trunks?limit=500&offset=0');
        const enabled = (r.items || []).filter(t => t.enabled);
        trunkOptions.value = enabled.map(t => ({
          value: 'trunk:' + t.name, label: `${t.name} (${t.host}:${t.port})`,
        }));
        trunkApiOk.value = true;
        ElementPlus.ElMessage({
          message: `[诊断] 拉到 ${r.items?.length || 0}  trunk，其中 enabled=${enabled.length}`,
          type: enabled.length ? 'success' : 'warning',
          duration: 4000, showClose: true,
        });
      } catch (e) {
        trunkApiOk.value = false;
        ElementPlus.ElMessage({
          message: `[诊断] fetchTrunkOptions 失败: ${e.message}`,
          type: 'error', duration: 6000, showClose: true,
        });
      }
    }

    async function fetchGatewayOptions() {
      try {
        const r = await API.get('/gateways?limit=500&offset=0');
        const enabled = (r.items || []).filter(g => g.enabled);
        gatewayOptions.value = enabled.map(g => ({
          value: 'gateway:' + g.name, label: `组: ${g.name}`,
        }));
      } catch (e) { /* 静默 */ }
    }

    /* 分机队列下拉（BIZ 路由：queue:<name>）。
     * 队列成员存在 members_json 字符串里，这里解析成数量仅用于展示。 */
    async function fetchQueueOptions() {
      try {
        const r = await API.get('/queues?limit=500&offset=0');
        const enabled = (r.items || []).filter(q => q.enabled !== 0 && q.enabled !== false);
        queueOptions.value = enabled.map(q => {
          let n = 0;
          try { n = (JSON.parse(q.members_json || '[]') || []).length; } catch (e) { n = 0; }
          return { value: 'queue:' + q.name, label: `队列 ${q.name}（${n} 成员）` };
        });
      } catch (e) { /* 静默——/queues 503 时后端还没配 DB */ }
    }

    /* 本地按 profile 过滤 + 分 */
    const filteredItems = computed(() =>
      items.value.filter(r => r.profile_name === activeProfile.value)
    );
    const filteredTotal = computed(() => filteredItems.value.length);
    const pagedItems = computed(() => {
      const start = (page.value - 1) * limit.value;
      return filteredItems.value.slice(start, start + limit.value);
    });

    async function load() {
      const q = new URLSearchParams({ limit: '500', offset: '0' });
      const r = await API.get(API_PATH + '?' + q.toString());
      items.value = r.items;
      page.value = 1;
    }
    async function reloadRules() {
      try {
        const r = await API.post(API_PATH + '/reload');
        ElementPlus.ElMessage({
          message: `已加载到 C 侧：${r.loaded} 规则`,
          type: 'success',
          duration: 3000,
          showClose: true,
        });
        await load();
      } catch (e) {
        ElementPlus.ElMessage({
          message: 'Reload 失败：' + e.message,
          type: 'error',
          duration: 4000,
          showClose: true,
        });
      }
    }
    // 统一 action_json 为 object —— 后端 json_text_as_value 把 DB 里的
    // JSON 文本序列化成真正的 object 返回；但 form 内部 v-model 传的是
    // JSON 字符串。兼容两种形态避免字段回退默认值（media_mode 变 auto、
    // ringback 丢失等）。
    function toJsonObj(v) {
      if (v == null || v === '') return {};
      if (typeof v === 'object') return v;       // 后端返回的是 object
      if (typeof v === 'string') {
        try { return JSON.parse(v); } catch { return {}; }
      }
      return {};
    }
    // 从 action_json 里安全地取出顶层字段，不存在返回DefaultValue
    function actGet(action_json, key, def) {
      const o = toJsonObj(action_json);
      return o[key] ?? def;
    }
    // 把顶层字段安全地 merge 进 action_json（保留未知字段如 ringback）
    function actSet(action_json, patches) {
      const o = toJsonObj(action_json);
      Object.assign(o, patches);
      return JSON.stringify(o);
    }
    // 把 value 统一成 JSON 字符串（保存 payload 用）
    function toJsonStr(v) {
      if (v == null) return '';
      if (typeof v === 'string') return v;
      if (typeof v === 'object') return JSON.stringify(v);
      return String(v);
    }

    function openCreate() {
      form.value = {
        tenant_id: 1,
        name: '',
        profile_name: activeProfile.value,
        src_ip_cidr: activeProfile.value === 'internal' ? RFC1918 : '0.0.0.0/0',
        match_expr: '',
        caller_match: '',
        prio: 1,
        strip: 0,
        dst_override: '',
        target: '',
        rec_action: 0,
        media_mode: 0,
        rec_path_template: '',
        rec_fmt: '',
        /* IMS 三态强制覆盖：0=inherit 1=force on 2=force off */
        force_prack: 0,
        force_precondition: 0,
        force_timer: 0,
        force_gruu: 0,
        /* REQ-DP-TIMEOUT-001：dialplan 规则级 per-call timeout override */
        sched_hangup_s: 0,
        originate_call_timeout_s: 0,
        action_json: '{}',
        /* REQ-DP-APP-001: Application 链 JSON 数组 —— C 侧 dp_json_parse_one 解析 */
        app_chain: '[]',
        enabled: true,
        description: '',
      };
      rawMode.action_json = false;
      rawMode.app_chain = false;
      ruleJsonUserEdited.value = false;
      editingId.value = null;
      showModal.value = true;
      // 初始化 ruleJson（等 Vue tick 完 form 已赋值）
      Vue.nextTick(() => ruleJsonFromForm());
    }
    function openEdit(row) {
      const f = { ...row };
      // 从 action_json 里把顶层快捷字段剥出来
      f.dst_override = actGet(row.action_json, 'dst_override', '');
      f.media_mode = Number(actGet(row.action_json, 'media_mode', 0));
      // app_chain 回显：后端 json_text_as_value 序列化成真正数组，
      // form 内部 v-model 期望字符串，统一成 JSON string
      f.app_chain = toJsonStr(row.app_chain || '[]');
      // action_json 也统一成字符串 —— 供 DialplanActionEditor v-model 使用
      f.action_json = toJsonStr(row.action_json || '{}');
      form.value = f;
      rawMode.action_json = false;
      rawMode.app_chain = false;
      ruleJsonUserEdited.value = false;
      editingId.value = row.id;
      showModal.value = true;
      Vue.nextTick(() => ruleJsonFromForm());
    }
    /* profile 切换时智能填Default CIDR（internal→RFC1918 防盗打，external→0.0.0.0/0 外部Gateway） */
    function onProfileChange() {
      if (!editingId.value) {
        form.value.src_ip_cidr = form.value.profile_name === 'internal' ? RFC1918 : '0.0.0.0/0';
      }
    }
    /* 正则Match Expression验证：JS RegExp 编译测试 + 可选号码Match测试。
     * 注：NSBC C 侧用 POSIX ERE，JS 语法与 POSIX ERE 基本兼容，
     * 但反向引用（\1 \2）等高级特性两端支持不同。这里做基础校验，
     * 避免明显拼写Error（未闭合括号/方括号等）。 */
    function validateMatchExpr() {
      const expr = form.value.match_expr || '';
      matchTestResult.value = null;
      if (!expr.trim()) {
        matchValid.value = false;
        matchError.value = 'Match Expression为空';
        return;
      }
      try {
        new RegExp(expr);
        matchValid.value = true;
        matchError.value = '';
        // 如果有Test Number，同时跑一下Match
        const tn = matchTestNum.value.trim();
        if (tn) {
          try {
            const re = new RegExp('^(?:' + expr + ')$');
            matchTestResult.value = re.test(tn);
          } catch {
            // 有些合法 POSIX ERE 在 JS 里 ^(?:...)$ 拼接会有问题，
            // 退而求其次直接 test（不加锚）
            try {
              const re2 = new RegExp(expr);
              matchTestResult.value = re2.test(tn);
            } catch {
              matchTestResult.value = null;
            }
          }
        }
      } catch (e) {
        matchValid.value = false;
        matchError.value = e.message || '语法Error';
      }
    }
    async function save() {
      saving.value = true;
      try {
        // ★ 前端必填校验 — name trim 后为空直接拦截，避免打到后端 400
        if (!form.value.name || !form.value.name.trim()) {
          ElMessage.error('Rule Name 不能为空');
          return;
        }
        form.value.name = form.value.name.trim();

        // ★ 如果用户直接编辑了 JSON 并且合法 → 优先用 JSON 作为 payload
        let payload;
        if (ruleJsonUserEdited.value && ruleJsonValid.value) {
          let obj; try { obj = JSON.parse(ruleJson.value); }
          catch { obj = null; }
          if (obj && typeof obj === 'object') {
            // 确保 action_json / app_chain 是字符串（后端 DB 列是 TEXT）
            if (obj.action_json !== undefined && typeof obj.action_json !== 'string')
              obj.action_json = JSON.stringify(obj.action_json);
            if (obj.app_chain !== undefined && typeof obj.app_chain !== 'string')
              obj.app_chain = JSON.stringify(obj.app_chain);
            payload = obj;
          }
        }
        if (!payload) {
          // 回退：用 form 结构化字段（走原来的 merge 逻辑）
          payload = { ...form.value };
          payload.action_json = actSet(payload.action_json, {
            dst_override: payload.dst_override || '',
            media_mode: payload.media_mode ?? 0,
          });
          delete payload.dst_override;
          delete payload.media_mode;
        }
        if (editingId.value) await API.put(API_PATH + '/' + editingId.value, payload);
        else await API.post(API_PATH, payload);
        showModal.value = false;
        await load();
      } catch (e) { ElMessage.error('Save失败: ' + e.message); }
      finally { saving.value = false; }
    }
    async function del(id) {
      try {
        await API.delete(API_PATH + '/' + id);
        await load();
      } catch (e) { ElMessage.error('Delete失败: ' + e.message); }
    }

    onMounted(() => { load(); fetchTrunkOptions(); fetchGatewayOptions(); fetchQueueOptions(); fetchTenants(); });
    return { items, page, limit, activeProfile, showModal, editingId, form, saving, rawMode,
             trunkOptions, gatewayOptions, queueOptions, trunkApiOk, tenantOptions,
             filteredItems, filteredTotal, pagedItems,
             matchValid, matchError, matchTestNum, matchTestResult, validateMatchExpr,
             ruleJson, ruleJsonValid, ruleJsonError, ruleJsonUserEdited,
             ruleJsonOnInput, ruleJsonOnBlur, ruleJsonSyncFromForm, ruleJsonFormat, ruleJsonCopy,
             load, reloadRules, openCreate, openEdit, onProfileChange, save, del };
  },
});

// =============================================================================
// AI Backends — AI 对话后端配置 CRUD（2026-11-19 NEW）
// REST API: /ai-backends
// =============================================================================
app.component('ai-backends', {
  template: `
    <div class="space-y-4">
      <div class="flex items-center justify-between">
        <h2 class="text-lg font-semibold text-slate-800"><i class="fa-solid fa-robot mr-2 text-purple-600"></i>{{ t('AI Backends') }}</h2>
        <div class="flex items-center gap-2">
          <el-input v-model="keyword" size="small" placeholder="搜索 name / provider..." clearable style="width:220px"
            @keyup.enter="load"></el-input>
          <el-button type="primary" size="small" @click="openCreate">
            <el-icon><Plus /></el-icon>{{ t('New') }}
          </el-button>
        </div>
      </div>

      <el-table :data="items" border stripe size="default" style="width:100%">
        <el-table-column type="index" label="#" width="55" align="center"></el-table-column>
        <el-table-column prop="name" label="Name" min-width="140"></el-table-column>
        <el-table-column prop="nsb_web_url" label="nsb-web URL" min-width="180">
          <template #default="{ row }"><code class="text-xs">{{ row.nsb_web_url }}</code></template>
        </el-table-column>
        <el-table-column prop="llm_provider" label="LLM" width="100" align="center">
          <template #default="{ row }"><el-tag size="small" effect="plain">{{ row.llm_provider }}</el-tag></template>
        </el-table-column>
        <el-table-column prop="llm_model" label="Model" width="140" align="center">
          <template #default="{ row }"><span class="text-xs text-slate-500">{{ row.llm_model || '—' }}</span></template>
        </el-table-column>
        <el-table-column prop="asr_provider" label="ASR" width="100" align="center">
          <template #default="{ row }"><el-tag size="small" type="success" effect="plain">{{ row.asr_provider }}</el-tag></template>
        </el-table-column>
        <el-table-column prop="tts_provider" label="TTS" width="100" align="center">
          <template #default="{ row }"><el-tag size="small" type="warning" effect="plain">{{ row.tts_provider }}</el-tag></template>
        </el-table-column>
        <el-table-column prop="transfer_dest" label="转人工" width="100" align="center">
          <template #default="{ row }">
            <span v-if="row.transfer_dest" class="text-sm font-mono text-amber-600">{{ row.transfer_dest }}</span>
            <span v-else class="text-slate-400 text-xs">—</span>
          </template>
        </el-table-column>
        <el-table-column prop="barge_in" label="打断" width="70" align="center">
          <template #default="{ row }">
            <span v-if="row.barge_in" class="text-green-600 font-bold">✓</span>
            <span v-else class="text-slate-300">—</span>
          </template>
        </el-table-column>
        <el-table-column prop="enabled" label="Enabled" width="80" align="center">
          <template #default="{ row }">
            <el-tag :type="row.enabled ? 'success' : 'info'" effect="light" size="small" round>
              {{ row.enabled ? 'ON' : 'OFF' }}
            </el-tag>
          </template>
        </el-table-column>
        <el-table-column prop="updated_at" label="Updated" width="180">
          <template #default="{ row }"><span class="text-xs text-slate-400">{{ row.updated_at }}</span></template>
        </el-table-column>
        <el-table-column label="Actions" width="150" align="center" fixed="right">
          <template #default="{ row }">
            <el-button link type="primary" size="small" @click="openEdit(row)">{{ t('Edit') }}</el-button>
            <el-popconfirm title="Delete this AI backend?" @confirm="del(row.id)">
              <template #reference><el-button link type="danger" size="small">{{ t('Delete') }}</el-button></template>
            </el-popconfirm>
          </template>
        </el-table-column>
      </el-table>

      <!-- 新建/编辑弹窗 -->
      <el-dialog v-model="showModal" :title="editingId ? 'Edit AI Backend' : 'New AI Backend'" width="820px" destroy-on-close>
        <el-form tag="div" :model="form" label-position="top">
          <el-tabs v-model="activeTab">
            <!-- Tab 1: 基本 -->
            <el-tab-pane label="基本信息" name="basic">
              <div class="grid grid-cols-2 gap-4">
                <el-form-item label="Name" required>
                  <el-input v-model="form.name" placeholder="例：default / doubao-cn / mock-test"></el-input>
                </el-form-item>
                <el-form-item label="Enable">
                  <el-switch v-model="form.enabled" active-text="ON" inactive-text="OFF"></el-switch>
                </el-form-item>
              </div>
              <el-form-item label="Description">
                <el-input v-model="form.description" type="textarea" :rows="2" placeholder="可选"></el-input>
              </el-form-item>
            </el-tab-pane>

            <!-- Tab 2: 控制通道 -->
            <el-tab-pane label="nsb-web 控制通道" name="control">
              <el-form-item label="nsb-web URL">
                <el-input v-model="form.nsb_web_url" placeholder="https://127.0.0.1:443"></el-input>
                <div class="text-xs text-slate-400 mt-1">AI agent 通过此 URL 调用 nsb-web HTTP API 发起 SIP 呼叫</div>
              </el-form-item>
              <div class="grid grid-cols-2 gap-4">
                <el-form-item label="Transport">
                  <el-select v-model="form.transport" style="width:100%">
                    <el-option value="https" label="HTTPS (推荐)"></el-option>
                    <el-option value="http" label="HTTP (内网开发)"></el-option>
                    <el-option value="wss" label="WSS (WebSocket Secure)"></el-option>
                    <el-option value="ws" label="WS (WebSocket)"></el-option>
                  </el-select>
                </el-form-item>
                <el-form-item label="TLS 证书校验">
                  <el-switch v-model="form.insecure"
                    active-text="跳过校验" inactive-text="严格校验"></el-switch>
                  <div class="text-xs text-slate-400 mt-1">内网自签证书建议打开跳过</div>
                </el-form-item>
              </div>
              <el-form-item label="API Token">
                <el-input v-model="form.nsb_web_token" type="password" form="nsbc-f" show-password placeholder="nsb-web /api/v1/login 登录获取"></el-input>
              </el-form-item>
            </el-tab-pane>

            <!-- Tab 3: LLM -->
            <el-tab-pane label="LLM 大模型" name="llm">
              <div class="grid grid-cols-2 gap-4">
                <el-form-item label="LLM Provider">
                  <el-select v-model="form.llm_provider" style="width:100%">
                    <el-option value="mock" label="mock（内置回音，开发测试用）"></el-option>
                    <el-option value="openai" label="OpenAI / 兼容 (gpt-4o-mini 等)"></el-option>
                    <el-option value="doubao" label="豆包 / 火山方舟"></el-option>
                    <el-option value="ollama" label="Ollama (本地部署)"></el-option>
                    <el-option value="qwen" label="通义千问"></el-option>
                  </el-select>
                </el-form-item>
                <el-form-item label="Model">
                  <el-input v-model="form.llm_model" placeholder="gpt-4o-mini"></el-input>
                </el-form-item>
              </div>
              <el-form-item label="API Key">
                <el-input v-model="form.llm_api_key" type="password" form="nsbc-f" show-password placeholder="sk-..."></el-input>
              </el-form-item>
              <el-form-item label="Endpoint (可选，自定义 URL)">
                <el-input v-model="form.llm_endpoint" placeholder="https://api.openai.com/v1"></el-input>
                <div class="text-xs text-slate-400 mt-1">留空 = Provider 默认地址；私有化部署或中转代理时填写</div>
              </el-form-item>
            </el-tab-pane>

            <!-- Tab 4: ASR -->
            <el-tab-pane label="ASR 语音识别" name="asr">
              <div class="grid grid-cols-2 gap-4">
                <el-form-item label="ASR Provider">
                  <el-select v-model="form.asr_provider" style="width:100%">
                    <el-option value="nway" label="nway (NSBC 内建，走 Event Client)"></el-option>
                    <el-option value="whisper" label="Whisper / faster-whisper"></el-option>
                    <el-option value="aliyun" label="阿里云智能语音"></el-option>
                    <el-option value="tencent" label="腾讯云 ASR"></el-option>
                  </el-select>
                </el-form-item>
                <el-form-item label="ASR 地址">
                  <el-input v-model="form.asr_addr" placeholder="127.0.0.1:8089"></el-input>
                </el-form-item>
              </div>
            </el-tab-pane>

            <!-- Tab 5: TTS -->
            <el-tab-pane label="TTS 语音合成" name="tts">
              <div class="grid grid-cols-2 gap-4">
                <el-form-item label="TTS Provider">
                  <el-select v-model="form.tts_provider" style="width:100%">
                    <el-option value="nway" label="nway (NSBC 内建)"></el-option>
                    <el-option value="edge" label="Edge TTS (免费, 离线)"></el-option>
                    <el-option value="aliyun" label="阿里云智能语音"></el-option>
                    <el-option value="tencent" label="腾讯云 TTS"></el-option>
                  </el-select>
                </el-form-item>
                <el-form-item label="TTS 地址">
                  <el-input v-model="form.tts_addr" placeholder="127.0.0.1:8090"></el-input>
                </el-form-item>
              </div>
            </el-tab-pane>

            <!-- Tab 6: 呼叫控制 -->
            <el-tab-pane label="呼叫控制" name="call">
              <div class="grid grid-cols-2 gap-4">
                <el-form-item label="转人工目标">
                  <div class="flex gap-2 w-full">
                    <el-select v-model="transferKind" style="width:130px" @change="onTransferKindChange">
                      <el-option value="ext" label="分机号"></el-option>
                      <el-option value="queue" label="分机队列"></el-option>
                      <el-option value="uri" label="SIP URI"></el-option>
                    </el-select>
                    <el-select v-if="transferKind === 'queue'" v-model="form.transfer_dest"
                      filterable allow-create default-first-option style="flex:1"
                      placeholder="选择队列（queue:<name>）">
                      <el-option v-for="o in queueOptions" :key="o.value" :value="o.value" :label="o.label"></el-option>
                      <template #empty>
                        <div class="p-3 text-xs text-slate-400">
                          暂无启用的分机队列，请先在「分机队列」页面创建并启用。
                        </div>
                      </template>
                    </el-select>
                    <el-input v-else v-model="form.transfer_dest" style="flex:1"
                      :placeholder="transferKind === 'uri' ? 'sip:1007@example.com' : '例：1007 或 extension:1007'"></el-input>
                  </div>
                  <div class="text-xs text-slate-400 mt-1">
                    <b>分机号</b>：转给单个分机（1007 / <code>extension:1007</code>）；<br>
                    <b>分机队列</b>：按队列策略选一个当前可振铃成员后转接（直接转给该座席，不排队等待）；<br>
                    <b>SIP URI</b>：完整 URI 原样透传。<code>trunk:</code> / <code>gateway:</code> / <code>ivr:</code> 暂不支持。
                  </div>
                </el-form-item>
                <el-form-item label="VAD 静音阈值">
                  <el-select v-model="form.vad_threshold" style="width:100%" clearable>
                    <el-option value="low" label="low (静音判定宽松，适合噪声大环境)"></el-option>
                    <el-option value="default" label="default (平衡)"></el-option>
                    <el-option value="high" label="high (静音判定严格，安静环境)"></el-option>
                  </el-select>
                </el-form-item>
              </div>
              <div class="grid grid-cols-2 gap-4">
                <el-form-item label="Barge-in（打断说话）">
                  <el-switch v-model="form.barge_in" active-text="允许" inactive-text="禁止"></el-switch>
                  <div class="text-xs text-slate-400 mt-1">用户开始说话时立即停止 LLM 正在播的 TTS</div>
                </el-form-item>
                <el-form-item label="录音保存目录">
                  <el-input v-model="form.record_dir" placeholder="/var/lib/nsbc/ai-records"></el-input>
                </el-form-item>
              </div>
            </el-tab-pane>
          </el-tabs>
        </el-form>
        <template #footer>
          <el-button @click="showModal = false">{{ t('Cancel') }}</el-button>
          <el-button type="primary" :loading="saving" @click="save">{{ t('Save') }}</el-button>
        </template>
      </el-dialog>
    </div>
  `,
  setup() {
    const API_PATH = '/ai-backends';
    const { ref, onMounted } = Vue;

    const items = ref([]);
    const keyword = ref('');
    const showModal = ref(false);
    const editingId = ref(null);
    const activeTab = ref('basic');
    const form = ref({});
    const saving = ref(false);
    const queueOptions = ref([]);   // [{ value: 'queue:xxx', label }]

    /* 转人工目标类型：普通 ref + syncTransferKind() 初始化。
     * 不用 computed getter/setter——setter 清空旧值后 getter 立刻推回 ext，
     * 会造成 el-select 看起来"切换不过去"的死循环。 */
    const transferKind = ref('ext');
    function syncTransferKind() {
      const v = (form.value.transfer_dest || '').trim();
      if (v.startsWith('queue:')) transferKind.value = 'queue';
      else if (v.startsWith('sip:') || v.startsWith('sips:') || v.includes('@')) transferKind.value = 'uri';
      else transferKind.value = 'ext';
    }
    function onTransferKindChange(kind) {
      if (kind === 'queue') {
        if (!(form.value.transfer_dest || '').startsWith('queue:')) form.value.transfer_dest = '';
      } else if (kind === 'uri') {
        const v = form.value.transfer_dest || '';
        if (!v.startsWith('sip:') && !v.startsWith('sips:')) form.value.transfer_dest = '';
      } else {
        const v = form.value.transfer_dest || '';
        if (v.startsWith('queue:') || v.startsWith('sip:') || v.startsWith('sips:')) form.value.transfer_dest = '';
      }
    }

    async function fetchQueueOptions() {
      try {
        const r = await API.get('/queues?limit=500&offset=0');
        const enabled = (r.items || []).filter(q => q.enabled !== 0 && q.enabled !== false);
        queueOptions.value = enabled.map(q => {
          let n = 0;
          try { n = (JSON.parse(q.members_json || '[]') || []).length; } catch (e) { n = 0; }
          return { value: 'queue:' + q.name, label: `队列 ${q.name}（${n} 成员）` };
        });
      } catch (e) { /* 静默——/queues 503 时后端还没配 DB */ }
    }

    function defaultForm() {
      return {
        name: '',
        description: '',
        nsb_web_url: 'https://127.0.0.1:443',
        nsb_web_token: '',
        transport: 'https',
        insecure: true,
        llm_provider: 'mock',
        llm_api_key: '',
        llm_model: 'gpt-4o-mini',
        llm_endpoint: '',
        asr_provider: 'nway',
        asr_addr: '127.0.0.1:8089',
        tts_provider: 'nway',
        tts_addr: '127.0.0.1:8090',
        transfer_dest: '',
        barge_in: true,
        vad_threshold: 'default',
        record_dir: '',
        enabled: true,
      };
    }

    async function load() {
      const params = new URLSearchParams({ limit: '500', offset: '0' });
      if (keyword.value) params.set('keyword', keyword.value.trim());
      const r = await API.get(API_PATH + '?' + params.toString());
      items.value = r.items || [];
    }

    function openCreate() {
      form.value = defaultForm();
      activeTab.value = 'basic';
      editingId.value = null;
      syncTransferKind();
      showModal.value = true;
      fetchQueueOptions();
    }

    function openEdit(row) {
      // 后端 AiBackend bool 字段返回 i64 (0/1)，前端直接映射
      const f = { ...row };
      // Option 字段：null → ''（输入框友好）
      for (const k of ['description','nsb_web_token','llm_api_key','llm_endpoint','asr_addr','tts_addr','transfer_dest','record_dir']) {
        if (f[k] == null) f[k] = '';
      }
      editingId.value = row.id;
      form.value = f;
      activeTab.value = 'basic';
      syncTransferKind();
      showModal.value = true;
      fetchQueueOptions();
    }

    async function save() {
      saving.value = true;
      try {
        if (!form.value.name || !form.value.name.trim()) {
          ElMessage.error('AI Backend Name 不能为空');
          return;
        }
        const payload = { ...form.value };
        payload.name = payload.name.trim();

        // 队列模式下允许只输入队列名，保存时补 queue: 前缀（C 侧按前缀识别）
        if (transferKind.value === 'queue' && payload.transfer_dest &&
            !String(payload.transfer_dest).startsWith('queue:')) {
          payload.transfer_dest = 'queue:' + String(payload.transfer_dest).trim();
        }

        // 空字符串 → null（后端 Option<String>）
        for (const k of ['description','nsb_web_token','llm_api_key','llm_model','llm_endpoint','asr_addr','tts_addr','transfer_dest','vad_threshold','record_dir']) {
          if (payload[k] != null && payload[k] !== '' && payload[k].trim() !== '') {
            payload[k] = payload[k].trim();
          } else {
            payload[k] = null;
          }
        }

        if (editingId.value) {
          await API.put(API_PATH + '/' + editingId.value, payload);
        } else {
          await API.post(API_PATH, payload);
        }
        showModal.value = false;
        await load();
      } catch (e) {
        ElMessage.error('Save 失败: ' + (e.message || e));
      } finally {
        saving.value = false;
      }
    }

    async function del(id) {
      try {
        await API.delete(API_PATH + '/' + id);
        await load();
      } catch (e) { ElMessage.error('Delete 失败: ' + (e.message || e)); }
    }

    onMounted(() => { load(); fetchQueueOptions(); });

    return {
      items, keyword, showModal, editingId, activeTab, form, saving,
      queueOptions, transferKind,
      load, openCreate, openEdit, save, del,
    };
  },
});

// --- 时间策略组 ---
app.component('time-groups', crudTable(
  { label: '时间策略组', icon: 'fa-solid fa-clock' },
  '/time-groups',
  [
    { key: 'name', label: t('Name'), type: 'text' },
    { key: 'enabled', label: t('Status'), type: 'bool' },
    { key: 'fallback_to', label: '回落至', type: 'text' },
    { key: 'description', label: t('Notes'), type: 'text' },
  ],
  [
    { key: 'name', label: t('Name'), type: 'text' },
    { key: 'enabled', label: t('Enable'), type: 'bool', default: true },
    { key: 'time_ranges', label: '时间范围', type: 'json', editor: 'time-ranges',
      default: '[{"days":"1-5","from":"09:00","to":"18:00"}]' },
    { key: 'fallback_to', label: t('回落Target'), type: 'text' },
    { key: 'description', label: t('Notes'), type: 'textarea' },
  ],
));

// --- Extension队列 ---
app.component('queues', crudTable(
  { label: t('Extension队列'), icon: 'fa-solid fa-people-group' },
  '/queues',
  [
    { key: 'tenant_id', label: t('Tenant ID'), type: 'text' },
    { key: 'name', label: t('Name'), type: 'text' },
    { key: 'enabled', label: t('Status'), type: 'bool' },
    { key: 'ring_strategy', label: '策略', type: 'text' },
    { key: 'max_wait_sec', label: '最大等待', type: 'text' },
    { key: 'moh_sound', label: t('Hold音'), type: 'text' },
  ],
  [
    { key: 'tenant_id', label: t('Tenant'), type: 'async-select',
      help: 'Owner tenant (list auto-filtered by current tenant)' },
    { key: 'name', label: t('Name'), type: 'text' },
    { key: 'enabled', label: t('Enable'), type: 'bool', default: true },
    { key: 'ring_strategy', label: t('Ringing策略'), type: 'select', options: [
      { value: 'ringall', label: t('ringall (全Ringing)') },
      { value: 'leastrecent', label: t('leastrecent (最近Idle)') },
      { value: 'roundrobin', label: 'roundrobin (轮询)' },
      { value: 'fewestcalls', label: t('fewestcalls (最少Call)') },
    ], default: 'ringall' },
    { key: 'max_wait_sec', label: t('最大等待 (s)'), type: 'number', default: 600 },
    { key: 'wrap_time_s', label: t('话后整理 (s)'), type: 'number', default: 30 },
    { key: 'require_skill', label: t('技能要求'), type: 'text', default: '',
      help: t('空=不筛技能；座席需具备该技能才会被派发') },
    { key: 'overflow', label: t('溢出目标'), type: 'text', default: '',
      help: t('等待超时后的去向，如 queue:sales2 / ivr:main / voicemail') },
    { key: 'moh_sound', label: t('Hold音乐'), type: 'text', default: 'default' },
    { key: 'members_json', label: '队列成员', type: 'json', editor: 'queue-members',
      default: '[{"extension":"2000","weight":10,"enabled":true}]' },
    { key: 'description', label: t('Notes'), type: 'textarea' },
  ],
  // asyncLoaders：动态下拉数据源
  {
    tenant_id: async () => {
      try {
        const r = await API.get('/tenants?limit=500&offset=0');
        const opts = (r.items || []).map(t => ({ value: t.id, label: t.tenant_name + (t.enabled ? '' : t(' (Disable)')) }));
        if (!opts.length) console.warn('[tenant_id async-loader] /tenants 返回空列表——检查 DB 是否有DefaultTenant');
        return opts;
      } catch (e) {
        console.error('[tenant_id async-loader] /tenants 加载失败:', e.message || e);
        return [];
      }
    },
  },
));

// --- IVR 菜单 ---
app.component('ivr-menus', crudTable(
  { label: 'IVR 菜单', icon: 'fa-solid fa-circle-question' },
  '/ivr-menus',
  [
    { key: 'name', label: t('Name'), type: 'text' },
    { key: 'tenant_id', label: t('Tenant'), type: 'text' },
    { key: 'entry_node', label: 'Entry Node', type: 'text' },
    { key: 'enabled', label: t('Status'), type: 'bool' },
  ],
  [
    { key: 'name', label: t('Name'), type: 'text' },
    { key: 'tenant_id', label: t('Tenant'), type: 'number', default: 1 },
    { key: 'entry_node', label: 'Entry Node', type: 'text', default: 'main' },
    { key: 'enabled', label: t('Enable'), type: 'bool', default: true },
    { key: 'nodes_json', label: t('IVR Menu Tree'), type: 'json', editor: 'ivr-options',
      default: '[{"name":"main","prompt":"welcome.wav","initial_timeout_s":5,"interdigit_timeout_s":3,"max_retries":3,"choices":[{"key":"1","act":"submenu","arg":"sales"},{"key":"2","act":"transfer","arg":"1000"},{"key":"*","act":"hangup","arg":""}],"timeout_action":{"act":"hangup","arg":""},"invalid_action":{"act":"play","arg":"invalid.wav"}}]' },
    { key: 'description', label: t('Notes'), type: 'textarea' },
  ],
));

// --- 高可用组（双机主备 Active-Standby）---
// 字段 1:1 对齐 conf/ha配置.md yaml 配置模型
app.component('ha-groups', crudTable(
  { label: '高可用组', icon: 'fa-solid fa-server' },
  '/ha-groups',
  [
    { key: 'name', label: '组名', type: 'text' },
    { key: 'enabled', label: '启用', type: 'bool' },
    { key: 'peer', label: '对端地址', type: 'text' },
    { key: 'priority', label: '优先级', type: 'text' },
    { key: 'state', label: '当前角色', type: 'text' },
  ],
  [
    { key: 'name', label: '组名', type: 'text', default: '' },
    { key: 'enabled', label: '启用 HA', type: 'bool', default: false,
      help: 'HA 模块总开关；关掉后 nsbcd 运行在 solo-active 模式' },

    // ---- 核心仲裁 ----
    { key: 'peer', label: '对端地址 (peer)', type: 'text', default: '',
      help: '对端 nsbcd 的心跳地址 IP:端口（如 10.0.0.81:5070）；留空则启用 UDP broadcast 自动 discover' },
    { key: 'heartbeat_ms', label: '心跳间隔 (ms)', type: 'number', default: 500,
      help: '默认 500ms；仲裁周期 = heartbeat_ms / 4 = 125ms；范围 [50, 10000]' },
    { key: 'failover_ms', label: 'Failover 窗口 (ms)', type: 'number', default: 3000,
      help: '对端连续不可见超此时长才升主；建议 heartbeat_ms × 6；范围 [100, 60000]' },
    { key: 'priority', label: '仲裁优先级', type: 'number', default: 100,
      help: '大者 ACTIVE；双方相同则 hostname 字典序 tie-break；主高备低避免不确定性' },

    // ---- 健康门阈值（可覆盖默认）----
    { key: 'evlag_warn_ms', label: '调度滞后告警 (ms)', type: 'number', default: 200,
      help: '事件循环滞后告警阈值；默认 200' },
    { key: 'evlag_fail_ms', label: '调度滞后失败 (ms)', type: 'number', default: 1000,
      help: '超过此值 → 健康门 UNHEALTHY → 让位 STANDBY' },
    { key: 'cpu_warn_pct', label: 'CPU 告警 (%)', type: 'number', default: 80,
      help: 'CPU 使用率告警阈值；默认 80' },
    { key: 'cpu_fail_pct', label: 'CPU 失败 (%)', type: 'number', default: 95,
      help: 'CPU 使用率 ≥ 此值 → 健康门 UNHEALTHY → 让位 STANDBY' },

    // ---- 状态复制 (replica) ----
    { key: 'replica_enabled', label: '启用状态复制 (replica)', type: 'bool', default: false,
      help: '主→备注册/对话 TCP 推送总开关；启用需同时填 peer' },
    { key: 'replica_port', label: '复制 TCP 端口', type: 'number', default: 5071,
      help: '主端监听，备端连接；与 UDP 心跳 5070 分离' },
    { key: 'replica_ring_cap', label: '环形缓冲容量 (条)', type: 'number', default: 8192,
      help: '备端 ACK 落后满环 → 主端返回 NOSPC → 触发全量快照' },
    { key: 'replica_payload_max', label: '单条最大载荷 (字节)', type: 'number', default: 4096,
      help: '超过此值的单条 REPL_REG/REPL_DIALOG 会被分片或转 FULL' },
    { key: 'replica_lag_warn', label: '复制水位差告警 (条)', type: 'number', default: 10000,
      help: '备端 applied_seq 落后主端 pub_seq 超过此值 → 健康门 DEGRADED' },
    { key: 'replica_full_resync_interval', label: '全量快照周期 (秒)', type: 'number', default: 0,
      help: '0=关闭；生产建议 3600（每小时全量一次对齐防缺口累积）' },

    // ---- BFD 亚秒探测 ----
    { key: 'bfd_enabled', label: '启用 BFD 亚秒探测', type: 'bool', default: false,
      help: 'RFC 5881；启用后心跳线程额外发 BFD Control 包；建议同步调小 failover_ms' },
    { key: 'bfd_local_disc', label: 'Local Discriminator', type: 'number', default: 1,
      help: '1–2^24-1，双方必须不同' },
    { key: 'bfd_tx_interval', label: '发包间隔 (ms)', type: 'number', default: 200,
      help: 'desired_min_tx_interval；建议 100–500' },
    { key: 'bfd_rx_interval', label: '期望对端发包间隔 (ms)', type: 'number', default: 200,
      help: 'required_min_rx_interval' },
    { key: 'bfd_detect_mult', label: '检测倍数', type: 'number', default: 3,
      help: 'detect_multiplier；经典口径 interval × 3 = failover 时间' },

    // ---- Hitless Upgrade ----
    { key: 'hitless_enabled', label: '启用滚动升级', type: 'bool', default: false,
      help: 'FREEZE→DRAIN→FULL push→yield 无感切换；不走 SHM 共享内存' },
    { key: 'hitless_shm_prefix', label: 'SHM 段名前缀', type: 'text', default: 'nsbc_ha_',
      help: '实际段名 = prefix + hostname；多节点互不干扰' },
    { key: 'hitless_freeze_ms', label: 'Freeze 窗口 (ms)', type: 'number', default: 1000,
      help: '老进程 stop 前冻结输入等待新进程 ready' },
    { key: 'hitless_timeout_ms', label: '升级超时 (ms)', type: 'number', default: 10000,
      help: '新进程在 freeze 内未上线则放弃滚动，回退重启老进程' },

    { key: 'description', label: '备注', type: 'textarea', default: '' },
  ],
));

// --- SIP Profile ---
app.component('sip-profiles', crudTable(
  { label: 'SIP 传输 Profile', icon: 'fa-solid fa-network-wired' },
  '/sip-profiles',
  [
    { key: 'name', label: t('Name'), type: 'text' },
    { key: 'enabled', label: t('Status'), type: 'bool' },
    { key: 'sip_ip', label: 'SIP 绑定 IP', type: 'text' },
    { key: 'sip_udp_port', label: t('UDP Port'), type: 'text' },
    { key: 'dtmf_mode', label: 'DTMF 模式', type: 'text' },
  ],
  [
    { key: 'name', label: t('Profile Name'), type: 'text' },
    { key: 'enabled', label: t('Enable'), type: 'bool', default: true },
    // ---- SIP 监听 ----
    { key: 'sip_ip', label: 'SIP 绑定 IP', type: 'text', default: '0.0.0.0' },
    { key: 'external_sip_ip', label: 'SIP NAT 公网 IP', type: 'text' },
    { key: 'sip_udp_port', label: t('UDP Port'), type: 'number', default: 5060 },
    { key: 'sip_tcp_port', label: t('TCP Port'), type: 'number', default: 5060 },
    { key: 'sip_tls_port', label: t('TLS Port'), type: 'number', default: 5061 },
    { key: 'enable_udp', label: t('Enable UDP'), type: 'bool', default: true },
    { key: 'enable_tcp', label: t('Enable TCP'), type: 'bool', default: true },
    { key: 'enable_tls', label: t('Enable TLS'), type: 'bool', default: false },
    // ---- RTP ----
    { key: 'rtp_ip', label: 'RTP 绑定 IP', type: 'text', default: '0.0.0.0' },
    { key: 'external_rtp_ip', label: 'RTP NAT 公网 IP', type: 'text' },
    { key: 'rtp_port_start', label: t('RTP Port起始'), type: 'number', default: 10000 },
    { key: 'rtp_port_end', label: t('RTP Port结束'), type: 'number', default: 59999 },
    // ---- 协议特性 ----
    { key: 'enable_prack', label: t('Enable PRACK (RFC 3262)'), type: 'bool', default: false },
    { key: 'enable_update', label: t('Enable UPDATE (RFC 3311)'), type: 'bool', default: true },
    // ---- 3GPP/IMS 协议特性（默认关闭，按客户需求开启） ----
    { key: 'enable_precondition', label: t('IMS Precondition (RFC 3312)'), type: 'bool', default: false },
    { key: 'enable_timer', label: t('IMS Session Timer (RFC 4028)'), type: 'bool', default: false },
    { key: 'enable_gruu', label: t('IMS GRUU (+sip.instance)'), type: 'bool', default: false },
    { key: 'dtmf_mode', label: 'DTMF 模式', type: 'select', options: [
      { value: 'rfc4733', label: 'RFC 4733 telephone-event (推荐)' },
      { value: 'info',    label: 'SIP INFO application/dtmf-relay' },
      { value: 'inband',  label: 'Inband 双频 G.711' },
      { value: 'auto',    label: '自动 (SDP 协商优先 RFC4733)' },
    ], default: 'rfc4733' },
    { key: 'realm', label: 'SIP Realm', type: 'text', default: 'nsbc.local' },
    { key: 'description', label: t('Notes'), type: 'textarea' },
  ],
));
