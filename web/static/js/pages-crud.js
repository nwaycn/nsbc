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
 * CIDR DefaultValue：internal → RFC1918 内网网段（防盗打）；external → 0.0.0.0/0（外部Gateway IP 不固定，用户按需Config） */
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
        <el-form :model="form" label-position="top">
          <el-form-item label="所属Tenant">
            <el-select v-model="form.tenant_id" style="width:100%" filterable>
              <el-option v-for="t in tenantOptions" :key="t.id" :value="t.id"
                :label="'T' + t.id + ' — ' + t.tenant_name + (t.enabled ? '' : ' (停用)')">
              </el-option>
            </el-select>
            <div class="text-xs text-slate-400 mt-1">{{ t('跨Tenant规则互相隔离，Default选 T1 (default)') }}</div>
          </el-form-item>
          <el-form-item label="Rule Name"><el-input v-model="form.name" placeholder="例：DID 51350820 → 内部 alice"></el-input></el-form-item>
          <el-form-item label="所属 Profile（= FreeSWITCH context）">
            <el-select v-model="form.profile_name" style="width:100%" @change="onProfileChange">
              <el-option value="internal" label="internal (SIP → internal)"></el-option>
              <el-option value="external" label="external (Gateway/Trunk → internal)"></el-option>
            </el-select>
            <div class="text-xs text-slate-400 mt-1">internal = default.xml；external = public.xml，两个 context 规则完全隔离</div>
          </el-form-item>
          <el-form-item :label="'源 IP 准入 CIDR（多网段逗号分隔）' + (form.profile_name === 'external' ? ' — 外部Gateway请填运营商 IP' : ' — Default RFC1918 内网防盗打')">
            <el-input v-model="form.src_ip_cidr" placeholder="10.0.0.0/8,172.16.0.0/12,192.168.0.0/16"></el-input>
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
              <el-option-group v-if="trunkApiOk && !trunkOptions.length && !gatewayOptions.length" label="— OUT —">
                <el-option disabled value="" label="(none available)"></el-option>
              </el-option-group>
              <el-option-group label="— EXT Extension / BIZ 业务 —">
                <el-option value="extension:" label="extension:<extension> → BIZ"></el-option>
                <el-option value="queue:"     label="queue:<队列> → BIZ"></el-option>
                <el-option value="ivr:"       label="ivr:<IVR> → BIZ"></el-option>
                <el-option value="hangup"     label="hangup → BIZ 挂机"></el-option>
              </el-option-group>
            </el-select>
            <div class="text-xs text-slate-400 mt-1">
              <b>★ 设计解耦</b>：<b>{{ t('Target资源只管路由') }}</b>{{ t('，Callee号码变换（strip/\${N}/dst_override）在下面「拨号Action」里独立Config。避免笛卡尔积爆炸。') }}<br>
              <b>{{ t('e.g.') }}</b>：match=<code>^(\\d{3,6})(\\d{11})$</code>，target=gateway:all_trunks，<b>dst_override=\${2}</b> → 外呼 捕获组2@all_trunks ✅
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
                <el-option :value="0" label="Never"></el-option>
                <el-option :value="1" label="mixed"></el-option>
                <el-option :value="2" label="separate"></el-option>
              </el-select>
              <div class="text-xs text-slate-400 mt-1">
                WebRTC 通话会被强制 relay 加密终结，bypass 对此类通话无效。
              </div>
            </el-form-item>
          </div>
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
    const RFC1918 = '10.0.0.0/8,172.16.0.0/12,192.168.0.0/16,127.0.0.0/8';
    const { ref, computed, onMounted } = Vue;

    const items = ref([]);
    const page = ref(1);
    const limit = ref(50);
    const activeProfile = ref('internal');
    const showModal = ref(false);
    const editingId = ref(null);
    const form = ref({});
    const saving = ref(false);
    const rawMode = Vue.reactive({ action_json: false, app_chain: false });
    // 正则验证Status
    const matchValid = ref(null);    // null=未验证, true=合法, false=非法
    const matchError = ref('');
    const matchTestNum = ref('');
    const matchTestResult = ref(null); // null=未测试, true=能Match, false=不Match

    // 外呼下拉选项（纯路由，不再自动拼 |${N} 组合——与 dst_override 解耦）
    const trunkOptions = ref([]);     // [{ value: 'trunk:xxx', label }]
    const gatewayOptions = ref([]);    // [{ value: 'gateway:xxx', label }]
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
    // 从 action_json 里安全地取出顶层字段（dst_override 等），不存在返回DefaultValue
    function actGet(action_json, key, def) {
      try {
        const o = JSON.parse(action_json || '{}');
        return o[key] ?? def;
      } catch { return def; }
    }
    // 把顶层字段安全地 merge 进 action_json（保留其他字段）
    function actSet(action_json, patches) {
      let o = {};
      try { o = JSON.parse(action_json || '{}'); } catch { o = {}; }
      Object.assign(o, patches);
      return JSON.stringify(o);
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
        /* IMS 三态强制覆盖：0=inherit 1=force on 2=force off */
        force_prack: 0,
        force_precondition: 0,
        force_timer: 0,
        force_gruu: 0,
        action_json: '{}',
        /* REQ-DP-APP-001: Application 链 JSON 数组 —— C 侧 dp_json_parse_one 解析 */
        app_chain: '[]',
        enabled: true,
        description: '',
      };
      rawMode.action_json = false;
      rawMode.app_chain = false;
      editingId.value = null;
      showModal.value = true;
    }
    function openEdit(row) {
      const f = { ...row };
      // 从 action_json 里把顶层快捷字段剥出来
      f.dst_override = actGet(row.action_json, 'dst_override', '');
      f.media_mode = Number(actGet(row.action_json, 'media_mode', 0));
      // app_chain 回显：row.app_chain 是 JSON 字符串（后端 serde 序列化）
      if (!f.app_chain || typeof f.app_chain !== 'string' || !f.app_chain.trim().startsWith('['))
        f.app_chain = '[]';
      form.value = f;
      rawMode.action_json = false;
      rawMode.app_chain = false;
      editingId.value = row.id;
      showModal.value = true;
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
        // 把顶层快捷字段 merge 进 action_json（C 侧从这里解析 dst_override 等）
        // 注意：strip 已独立存 DB 列，同步到 C 侧作为 JSON 顶层字段，不用塞 action_json
        const payload = { ...form.value };
        payload.action_json = actSet(payload.action_json, {
          dst_override: payload.dst_override || '',
          media_mode: payload.media_mode ?? 0,
        });
        delete payload.dst_override; // 后端表没有这列，去掉
        delete payload.media_mode;   // 同上
        if (editingId.value) await API.put(API_PATH + '/' + editingId.value, payload);
        else await API.post(API_PATH, payload);
        showModal.value = false;
        await load();
      } catch (e) { alert('Save失败: ' + e.message); }
      finally { saving.value = false; }
    }
    async function del(id) {
      try {
        await API.delete(API_PATH + '/' + id);
        await load();
      } catch (e) { alert('Delete失败: ' + e.message); }
    }

    onMounted(() => { load(); fetchTrunkOptions(); fetchGatewayOptions(); fetchTenants(); });
    return { items, page, limit, activeProfile, showModal, editingId, form, saving, rawMode,
             trunkOptions, gatewayOptions, trunkApiOk, tenantOptions,
             filteredItems, filteredTotal, pagedItems,
             matchValid, matchError, matchTestNum, matchTestResult, validateMatchExpr,
             load, reloadRules, openCreate, openEdit, onProfileChange, save, del };
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
      { value: 'random', label: 'random (随机)' },
      { value: 'fewestcalls', label: t('fewestcalls (最少Call)') },
    ], default: 'ringall' },
    { key: 'max_wait_sec', label: t('最大等待 (s)'), type: 'number', default: 600 },
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

// --- 高可用组 ---
app.component('ha-groups', crudTable(
  { label: '高可用组', icon: 'fa-solid fa-server' },
  '/ha-groups',
  [
    { key: 'name', label: t('Name'), type: 'text' },
    { key: 'enabled', label: t('Status'), type: 'bool' },
    { key: 'quorum', label: '法定人数', type: 'text' },
    { key: 'state', label: t('CurrentStatus'), type: 'text' },
  ],
  [
    { key: 'name', label: t('Name'), type: 'text' },
    { key: 'enabled', label: t('Enable'), type: 'bool', default: true },
    { key: 'node_list_json', label: '节点列表', type: 'json', editor: 'ha-node-list',
      default: '[{"node_id":"node-1","host":"10.0.0.25","port":8080,"role":"slave"}]' },
    { key: 'quorum', label: '法定人数', type: 'number', default: 2 },
    { key: 'election_timeout', label: '选主超时 (ms)', type: 'number', default: 5000 },
    { key: 'description', label: t('Notes'), type: 'textarea' },
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


