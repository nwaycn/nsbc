// =============================================================================
// NSBC Web — CRUD 实体页面组件
// 加载顺序：必须在 app-core.js 之后
// 依赖：app-core.js 中声明的全局 const app, crudTable
// =============================================================================

// --- 租户 ---
app.component('tenants', crudTable(
  { label: '租户（SIP Realm）', icon: 'fa-solid fa-building' },
  '/tenants',
  [
    { key: 'id', label: 'ID', type: 'text' },
    { key: 'tenant_name', label: 'SIP Realm', type: 'text' },
    { key: 'enabled', label: '状态', type: 'bool' },
  ],
  [
    { key: 'tenant_name', label: 'SIP Realm（租户名）', type: 'text',
      help: 'SIP 认证域 / 租户唯一标识，默认 nsbc.local。呼入 INVITE 的 From 域或分机注册 realm 必须与此值一致。如 "nsbc.local"、"acme.com"' },
    { key: 'enabled', label: '启用', type: 'bool', default: true },
    { key: 'description', label: '备注', type: 'textarea' },
    { key: 'match_domain', label: '呼入 From 域匹配', type: 'text',
      placeholder: 'acme.com', help: '呼入 From 域=此值时归入本租户（可选，留空则只靠 tenant_name/realm 匹配）' },
    { key: 'match_did_prefix', label: '呼入 DID 号段前缀', type: 'text',
      placeholder: '+86101234', help: '呼入号码以此前缀开头时归入本租户（可选）' },
    { key: 'match_trunk', label: '呼入中继名匹配', type: 'text',
      placeholder: '运营商A', help: '呼入走此中继时归入本租户（可选）' },
  ],
));

// --- 分机 ---
app.component('extensions', crudTable(
  { label: '分机', icon: 'fa-solid fa-phone' },
  '/extensions',
  [
    { key: 'tenant_id', label: '租户ID', type: 'text' },
    { key: 'extension', label: '分机号', type: 'text' },
    { key: 'ext_type', label: '类型', type: 'text' },
    { key: 'full_name', label: '姓名', type: 'text' },
    { key: 'enabled', label: '状态', type: 'bool' },
  ],
  [
    { key: 'tenant_id', label: '租户', type: 'async-select',
      help: '所属租户（列表会按当前租户自动过滤）' },
    { key: 'extension', label: '分机号', type: 'text' },
    { key: 'ext_type', label: '类型', type: 'select',
      options: [{value:'internal', label:'内部分机（SIP 注册）'},
                {value:'external', label:'外部分机（经中继外呼）'}],
      default: 'internal', help: 'external 时下面三项必填' },
    { key: 'password', label: '密码', type: 'password', help: '外部分机不注册可留空' },
    { key: 'full_name', label: '姓名', type: 'text' },
    { key: 'enabled', label: '启用', type: 'bool', default: true },
    { key: 'description', label: '备注', type: 'textarea' },
    // 外部分机配置（ext_type=external 时有效）
    { key: 'external_number', label: '外部号码（E.164，如 +8613800138000）', type: 'text',
      placeholder: '+8613800138000', help: '外呼拨打的真实号码' },
    { key: 'trunk_name', label: '出局中继', type: 'text',
      placeholder: '运营商IMS', help: '走哪个 trunk 外呼（Web 中继管理里的名字）' },
    { key: 'display_number', label: '主叫显示号码（Caller ID）', type: 'text',
      placeholder: '+861012345678', help: '对端看到的来电号码（空=用 trunk 默认）' },
  ],
  // asyncLoaders：动态下拉数据源
  {
    tenant_id: async () => {
      try {
        const r = await API.get('/tenants?limit=500&offset=0');
        const opts = (r.items || []).map(t => ({ value: t.id, label: t.tenant_name + (t.enabled ? '' : ' (禁用)') }));
        if (!opts.length) console.warn('[tenant_id async-loader] /tenants 返回空列表——检查 DB 是否有默认租户');
        return opts;
      } catch (e) {
        console.error('[tenant_id async-loader] /tenants 加载失败:', e.message || e);
        return [];
      }
    },
  },
));

// --- 网关 ---
app.component('gateways', crudTable(
  { label: '网关', icon: 'fa-solid fa-tower-broadcast' },
  '/gateways',
  [
    { key: 'name', label: '名称', type: 'text' },
    { key: 'enabled', label: '状态', type: 'bool' },
    { key: 'description', label: '备注', type: 'text' },
  ],
  [
    { key: 'name', label: '名称', type: 'text' },
    { key: 'enabled', label: '启用', type: 'bool', default: true },
    { key: 'description', label: '备注', type: 'textarea' },
  ],
));

// --- 中继 ---
app.component('trunks', crudTable(
  { label: '中继', icon: 'fa-solid fa-signal' },
  '/trunks',
  [
    { key: 'tenant_id', label: '租户ID', type: 'text' },
    { key: 'name', label: '名称', type: 'text' },
    { key: 'gateway_name', label: '网关', type: 'text' },
    { key: 'kind', label: '类型', type: 'text' },
    { key: 'host', label: '主机', type: 'text' },
    { key: 'port', label: '端口', type: 'text' },
    { key: 'enabled', label: '状态', type: 'bool' },
  ],
  [
    { key: 'tenant_id', label: '租户', type: 'async-select',
      help: '所属租户（列表会按当前租户自动过滤）' },
    { key: 'name', label: '名称', type: 'text' },
    { key: 'gateway_name', label: '所属网关', type: 'async-select',
      help: '从「网关」页面配置的组（可留空，表示该中继不属任何组）' },
    { key: 'profile_name', label: 'SIP Profile', type: 'async-select', default: 'external',
      help: '从「SIP Profile」页面配置（internal=默认, external=公网, local=回环）' },
    { key: 'kind', label: '中继类型', type: 'select', options: [
      { value: 'static_peer', label: 'static_peer (静态对端)' },
      { value: 'register',    label: 'register (注册型)' },
      { value: 'auth_out',    label: 'auth_out (出局鉴权)' },
    ]},
    { key: 'proto', label: '协议', type: 'select', options: [
      { value: 0, label: 'UDP (0)' }, { value: 1, label: 'TCP (1)' }, { value: 2, label: 'TLS (2)' },
    ]},
    { key: 'host', label: '主机地址', type: 'text' },
    { key: 'port', label: '端口', type: 'number', default: 5060 },
    { key: 'auth_user', label: '鉴权用户名', type: 'text' },
    { key: 'password', label: '鉴权密码', type: 'text', editor: 'password' },
    { key: 'realm', label: 'Realm', type: 'text' },
    { key: 'codec_set', label: '可用编码（逗号分隔）', type: 'text',
      default: 'PCMA,PCMU' },
    { key: 'weight', label: '权重', type: 'number', default: 10 },
    { key: 'priority', label: '优先级', type: 'number', default: 1 },
    { key: 'max_channels', label: '最大并发（硬性天花板）', type: 'number', default: 0 },
    { key: 'per_number_max_channels', label: '单号码最大并发', type: 'number', default: 0 },
    { key: 'caller_id_pool', label: '号码池（逗号分隔）', type: 'textarea',
      placeholder: '8613800000000,8613800000001,8613800000002' },
    { key: 'enabled', label: '启用', type: 'bool', default: true },
    { key: 'description', label: '备注', type: 'textarea' },
    { key: 'from_user', label: '[FS] From User（空=auth_user）', type: 'text', default: '' },
    { key: 'from_domain', label: 'From Domain（空=走回退链 realm→proxy→host）', type: 'text', default: '' },
    { key: 'proxy', label: '出局代理（空=同 host）', type: 'text', default: '' },
    { key: 'register_proxy', label: '注册专用代理（空=同 proxy）', type: 'text', default: '' },
    { key: 'expire_seconds', label: '注册过期时间（秒）', type: 'number', default: 3600 },
    { key: 'retry_seconds', label: '退避基数（秒，0=FS 语义不重试）', type: 'number', default: 5 },
    { key: 'caller_id_in_from', label: 'caller_id_in_from（=1 只替换 From user part）', type: 'bool', default: false },
    { key: 'contact_params', label: 'Contact 额外参数（如 transport=udp）', type: 'text', default: '' },
    { key: 'extension_in_contact', label: 'extension_in_contact（=1 把 inbound_extension 放入 Contact）', type: 'bool', default: false },
    { key: 'ping_seconds', label: 'OPTIONS 健康探测间隔（0=关闭）', type: 'number', default: 0 },
    { key: 'inbound_extension', label: '入站 DID 映射目标（如 2001）', type: 'text', default: '' },
    { key: 'cid_type', label: 'CID 类型（预留 rpid 等）', type: 'text', default: '' },
    { key: 'rfc5626_enabled', label: 'RFC 5626 Outbound（预留）', type: 'bool', default: false },
    { key: 'reg_id', label: 'reg-id（RFC 5626 预留）', type: 'text', default: '' },
    { key: 'peer_ip', label: 'peer_ip 白名单（CIDR 逗号分隔，auth_out 用）', type: 'textarea', default: '' },
    { key: 'ims_enabled', label: '运营商 IMS 中继（自动注入 PAI/PPID/PCPI 头）', type: 'bool', default: false },
    { key: 'ims_domain', label: 'IMS 根域（如 ims.mnc001.mcc460.3gppnetwork.org）', type: 'text', default: '',
      placeholder: 'ims.mnc001.mcc460.3gppnetwork.org',
      help: '必填：开启 IMS 后，PAI/PPID/PCPI 会用 {auth_user}@{ims_domain} 拼 SIP URI' },
    { key: 'ims_auto_pani', label: '自动注入 P-Access-Network-Info（默认 E-UTRAN-TDD）', type: 'bool', default: false },
    { key: 'ims_pani_value', label: '自定义 PANI 值（空=E-UTRAN-TDD）', type: 'text', default: '',
      placeholder: 'E-UTRAN-TDD; utran-cell-id-3gpp=0001001' },
    { key: 'ims_auto_privacy', label: '自动注入 Privacy: id（CLIR，对端 From 匿名）', type: 'bool', default: false },
    { key: 'custom_headers', label: '自定义 SIP 头（出站 INVITE）', type: 'textarea', default: '',
      placeholder: 'MyID=${callid}\r\nX-Biz-Caller=${caller}\r\nX-Biz-Callee=${callee}',
      help: '每行一条，必须带 \\r\\n 结尾。可用变量：${callid} ${caller} ${callee} ${from_user} ${to_user} ${from_ip} ${to_ip}' },
  ],
  // asyncLoaders：动态下拉数据源
  {
    tenant_id: async () => {
      try {
        const r = await API.get('/tenants?limit=500&offset=0');
        const opts = (r.items || []).map(t => ({ value: t.id, label: t.tenant_name + (t.enabled ? '' : ' (禁用)') }));
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
        return (r.items || []).map(g => ({ value: g.name, label: `${g.name}${g.enabled ? '' : ' (禁用)'}` }));
      } catch (e) { return []; }
    },
    profile_name: async () => {
      try {
        const r = await API.get('/sip-profiles?limit=500&offset=0');
        return (r.items || []).map(p => ({ value: p.name, label: `${p.name}${p.enabled ? '' : ' (禁用)'}` }));
      } catch (e) { return []; }
    },
  },
));

// --- 路由组 ---
app.component('route-groups', crudTable(
  { label: '路由组', icon: 'fa-solid fa-route' },
  '/routes',
  [
    { key: 'name', label: '名称', type: 'text' },
    { key: 'strategy', label: '策略', type: 'text' },
    { key: 'enabled', label: '状态', type: 'bool' },
  ],
  [
    { key: 'name', label: '名称', type: 'text' },
    { key: 'strategy', label: '选路策略', type: 'select', options: [
      { value: 'priority', label: 'priority (主备优先)' },
      { value: 'weight',   label: 'weight (加权散列)' },
      { value: 'least_busy', label: 'least_busy (最闲优先)' },
      { value: 'ring',     label: 'ring (轮询)' },
    ], default: 'priority' },
    { key: 'enabled', label: '启用', type: 'bool', default: true },
    { key: 'description', label: '备注', type: 'textarea' },
  ],
));

/* §15 拨号规则：带 internal/external Profile Tab 分区（= FreeSWITCH default.xml vs public.xml）
 * CIDR 默认值：internal → RFC1918 内网网段（防盗打）；external → 0.0.0.0/0（外部网关 IP 不固定，用户按需配置） */
app.component('dialplan-rules', {
  template: `
    <div class="space-y-4">
      <div class="flex items-center justify-between">
        <h2 class="text-lg font-semibold text-slate-800"><i class="fa-solid fa-code-branch mr-2 text-blue-600"></i>拨号规则</h2>
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
        <el-tab-pane label="internal — 内部 SIP Profile（已注册分机）" name="internal">
          <el-table :data="pagedItems" border stripe size="default" style="width:100%">
            <el-table-column type="index" label="#" width="55" align="center"></el-table-column>
            <el-table-column prop="tenant_id" label="租户" width="80" align="center">
              <template #default="{ row }"><el-tag size="small" effect="plain">T{{ row.tenant_id || 1 }}</el-tag></template>
            </el-table-column>
            <el-table-column prop="name" label="规则名称" min-width="140"></el-table-column>
            <el-table-column prop="match_expr" label="被叫匹配" min-width="140">
              <template #default="{ row }"><code class="text-xs bg-slate-100 px-1 rounded">{{ row.match_expr }}</code></template>
            </el-table-column>
            <el-table-column prop="target" label="目标资源" min-width="160"></el-table-column>
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
                <span v-else class="text-slate-400 text-xs">默认</span>
              </template>
            </el-table-column>
            <el-table-column prop="prio" label="优先级" width="80" align="center"></el-table-column>
            <el-table-column prop="src_ip_cidr" label="IP 准入 CIDR" min-width="180">
              <template #default="{ row }">
                <code class="text-xs" :class="row.src_ip_cidr === '0.0.0.0/0' ? 'text-amber-600' : 'text-slate-500'">{{ row.src_ip_cidr }}</code>
              </template>
            </el-table-column>
            <el-table-column prop="enabled" label="状态" width="80" align="center">
              <template #default="{ row }">
                <el-tag :type="row.enabled ? 'success' : 'info'" effect="light" size="small" round>
                  {{ row.enabled ? '启用' : '禁用' }}
                </el-tag>
              </template>
            </el-table-column>
            <el-table-column label="操作" width="150" align="center" fixed="right">
              <template #default="{ row }">
                <el-button link type="primary" size="small" @click="openEdit(row)">编辑</el-button>
                <el-popconfirm title="确认删除？" @confirm="del(row.id)">
                  <template #reference><el-button link type="danger" size="small">删除</el-button></template>
                </el-popconfirm>
              </template>
            </el-table-column>
          </el-table>
          <div class="flex justify-end pt-2">
            <el-pagination v-model:current-page="page" :page-size="limit" :total="filteredTotal"
              layout="total, prev, pager, next" small background></el-pagination>
          </div>
        </el-tab-pane>

        <el-tab-pane label="external — 外部 SIP Profile（SIP 网关/DID 中继）" name="external">
          <el-table :data="pagedItems" border stripe size="default" style="width:100%">
            <el-table-column type="index" label="#" width="55" align="center"></el-table-column>
            <el-table-column prop="tenant_id" label="租户" width="80" align="center">
              <template #default="{ row }"><el-tag size="small" effect="plain">T{{ row.tenant_id || 1 }}</el-tag></template>
            </el-table-column>
            <el-table-column prop="name" label="规则名称" min-width="140"></el-table-column>
            <el-table-column prop="match_expr" label="被叫匹配" min-width="140">
              <template #default="{ row }"><code class="text-xs bg-slate-100 px-1 rounded">{{ row.match_expr }}</code></template>
            </el-table-column>
            <el-table-column prop="target" label="目标资源" min-width="160"></el-table-column>
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
                <span v-else class="text-slate-400 text-xs">默认</span>
              </template>
            </el-table-column>
            <el-table-column prop="prio" label="优先级" width="80" align="center"></el-table-column>
            <el-table-column prop="src_ip_cidr" label="IP 准入 CIDR" min-width="180">
              <template #default="{ row }">
                <code class="text-xs" :class="row.src_ip_cidr === '0.0.0.0/0' ? 'text-amber-600' : 'text-slate-500'">{{ row.src_ip_cidr }}</code>
              </template>
            </el-table-column>
            <el-table-column prop="enabled" label="状态" width="80" align="center">
              <template #default="{ row }">
                <el-tag :type="row.enabled ? 'success' : 'info'" effect="light" size="small" round>
                  {{ row.enabled ? '启用' : '禁用' }}
                </el-tag>
              </template>
            </el-table-column>
            <el-table-column label="操作" width="150" align="center" fixed="right">
              <template #default="{ row }">
                <el-button link type="primary" size="small" @click="openEdit(row)">编辑</el-button>
                <el-popconfirm title="确认删除？" @confirm="del(row.id)">
                  <template #reference><el-button link type="danger" size="small">删除</el-button></template>
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

      <!-- 新建/编辑弹窗 -->
      <el-dialog v-model="showModal" :title="(editingId ? '编辑' : '新建') + ' 拨号规则'" width="900px" destroy-on-close>
        <el-form :model="form" label-position="top">
          <el-form-item label="所属租户">
            <el-select v-model="form.tenant_id" style="width:100%" filterable>
              <el-option v-for="t in tenantOptions" :key="t.id" :value="t.id"
                :label="'T' + t.id + ' — ' + t.tenant_name + (t.enabled ? '' : ' (停用)')">
              </el-option>
            </el-select>
            <div class="text-xs text-slate-400 mt-1">跨租户规则互相隔离，默认选 T1 (default)</div>
          </el-form-item>
          <el-form-item label="规则名称"><el-input v-model="form.name" placeholder="例：DID 51350820 → 内部 alice"></el-input></el-form-item>
          <el-form-item label="所属 Profile（= FreeSWITCH context）">
            <el-select v-model="form.profile_name" style="width:100%" @change="onProfileChange">
              <el-option value="internal" label="internal — 内部 SIP Profile（已注册分机）"></el-option>
              <el-option value="external" label="external — 外部 SIP Profile（SIP 网关/DID 中继）"></el-option>
            </el-select>
            <div class="text-xs text-slate-400 mt-1">internal = default.xml；external = public.xml，两个 context 规则完全隔离</div>
          </el-form-item>
          <el-form-item :label="'源 IP 准入 CIDR（多网段逗号分隔）' + (form.profile_name === 'external' ? ' — 外部网关请填运营商 IP' : ' — 默认 RFC1918 内网防盗打')">
            <el-input v-model="form.src_ip_cidr" placeholder="10.0.0.0/8,172.16.0.0/12,192.168.0.0/16"></el-input>
          </el-form-item>
          <el-form-item label="被叫匹配表达式">
            <div class="flex gap-2 items-start w-full">
              <el-input v-model="form.match_expr" placeholder="正则 / 通配符：^1[01]\d{2}$ / 100[0-9] / 51350820" style="flex:1"></el-input>
              <el-button @click="validateMatchExpr" :type="matchValid === null ? 'info' : (matchValid ? 'success' : 'danger')">验证表达式</el-button>
            </div>
            <div v-if="matchValid !== null" class="mt-1 text-xs" :class="matchValid ? 'text-green-600' : 'text-red-500'">
              <span v-if="matchValid">✓ 表达式合法（可用正则语法）</span>
              <span v-else>✗ 表达式有问题：{{ matchError }}</span>
            </div>
            <div v-if="matchValid" class="mt-1 text-xs text-slate-500">
              <div class="flex gap-2 items-center mt-1">
                <span>测试号码：</span>
                <el-input v-model="matchTestNum" size="small" style="width:160px" placeholder="如 11001"></el-input>
                <el-tag v-if="matchTestResult !== null" :type="matchTestResult ? 'success' : 'info'" size="small">
                  {{ matchTestResult ? '✓ 能匹配' : '✗ 不匹配' }}
                </el-tag>
              </div>
            </div>
          </el-form-item>
          <el-form-item label="主叫匹配（可选）"><el-input v-model="form.caller_match" placeholder="留空=不限制主叫"></el-input></el-form-item>
          <div class="grid grid-cols-2 gap-4">
            <el-form-item label="同 Profile 内优先级（小值优先）">
              <el-input-number v-model="form.prio" :controls="false" style="width:100%"></el-input-number>
            </el-form-item>
            <el-form-item label="去前导位数（strip）">
              <el-input-number v-model="form.strip" :controls="false" style="width:100%"></el-input-number>
            </el-form-item>
          </div>
          <el-form-item label="被叫覆盖（dst_override）">
            <el-input v-model="form.dst_override"
                      placeholder="空=strip 结果；可用 $1 $2 或 \${1} \${2}；如 888$2"
                      clearable>
            </el-input>
            <div class="text-xs text-slate-400 mt-1">
              当被叫变换不是简单"去前导"时用这个；正则捕获组两种语法都支持：
              <code class="bg-slate-100 px-1 rounded">$1</code>
              <code class="bg-slate-100 px-1 rounded mx-1">$2</code>
              <span class="mx-1">或</span>
              <code class="bg-slate-100 px-1 rounded">\${1}</code>
              <code class="bg-slate-100 px-1 rounded mx-1">\${2}</code>。
              优先级：<b>dst_override</b> > strip > add_prefix。
            </div>
          </el-form-item>
          <el-form-item label="目标资源（只管路由）">
            <el-select v-model="form.target" filterable allow-create default-first-option
              placeholder="选中继 / 网关组，或直接输入 extension:xxx" style="width:100%" clearable>
              <el-option-group v-if="gatewayOptions.length" label="— 网关组（OUT 组选路）—">
                <el-option v-for="o in gatewayOptions" :key="o.value" :value="o.value" :label="o.label"></el-option>
              </el-option-group>
              <el-option-group v-if="trunkOptions.length" label="— 单中继（OUT 直连）—">
                <el-option v-for="o in trunkOptions" :key="o.value" :value="o.value" :label="o.label"></el-option>
              </el-option-group>
              <el-option-group v-if="trunkApiOk && !trunkOptions.length && !gatewayOptions.length" label="— OUT —">
                <el-option disabled value="" label="（暂无可选目标，请先到「中继」「网关」页面创建并启用）"></el-option>
              </el-option-group>
              <el-option-group label="— EXT 分机 / BIZ 业务 —">
                <el-option value="extension:" label="extension:<分机> → EXT"></el-option>
                <el-option value="queue:"     label="queue:<队列> → BIZ"></el-option>
                <el-option value="ivr:"       label="ivr:<IVR> → BIZ"></el-option>
                <el-option value="hangup"     label="hangup → BIZ 挂机"></el-option>
              </el-option-group>
            </el-select>
            <div class="text-xs text-slate-400 mt-1">
              <b>★ 设计解耦</b>：<b>目标资源只管路由</b>，被叫号码变换（strip/\${N}/dst_override）在下面「拨号动作」里独立配置。避免笛卡尔积爆炸。<br>
              <b>示例</b>：match=<code>^(\\d{3,6})(\\d{11})$</code>，target=gateway:all_trunks，<b>dst_override=\${2}</b> → 外呼 捕获组2@all_trunks ✅
            </div>
          </el-form-item>
          <div class="grid grid-cols-2 gap-4">
            <el-form-item label="录音控制（本条规则命中时触发）">
              <el-select v-model="form.rec_action" style="width:100%">
                <el-option :value="0" label="不录音（默认）"></el-option>
                <el-option :value="1" label="Start — 命中后开始录音"></el-option>
                <el-option :value="2" label="Stop — 命中后停止录音"></el-option>
              </el-select>
              <div class="text-xs text-slate-400 mt-1">每条规则独立配置，下一条规则可不同；0=none 1=start 2=stop</div>
            </el-form-item>
            <el-form-item label="媒体模式（本规则路由时）">
              <el-select v-model="form.media_mode" style="width:100%">
                <el-option :value="0" label="auto — 按自动决策（默认）"></el-option>
                <el-option :value="1" label="bypass — 强制媒体绕过 SBC 直连"></el-option>
                <el-option :value="2" label="relay — 强制 SBC 转发媒体"></el-option>
              </el-select>
              <div class="text-xs text-slate-400 mt-1">
                WebRTC 通话会被强制 relay 加密终结，bypass 对此类通话无效。
              </div>
            </el-form-item>
          </div>
          <el-form-item label="录音路径模板（空=系统默认）">
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
          <el-form-item label="启用">
            <el-switch v-model="form.enabled" active-text="启用" inactive-text="禁用"></el-switch>
          </el-form-item>
          <el-form-item label="备注"><el-input v-model="form.description" type="textarea" :rows="2"></el-input></el-form-item>
          <el-form-item label="拨号动作（SIP 头改写 / 号码变换）">
            <div class="w-full">
              <div class="flex justify-between items-center mb-1">
                <span class="text-xs text-slate-500">结构化数据（点击切换 Raw JSON）</span>
                <el-button link size="small" type="primary" @click="rawMode.action_json = !rawMode.action_json">
                  {{ rawMode.action_json ? '结构化编辑' : 'Raw JSON' }}
                </el-button>
              </div>
              <el-input v-if="rawMode.action_json" v-model="form.action_json" type="textarea" :rows="6"
                placeholder='有效 JSON，例如 {"clip":"44","sip_invite_from_uri":"sip:alice@nsbc.local"}' resize="none"></el-input>
              <dp-action-editor v-else v-model="form.action_json"></dp-action-editor>
            </div>
            <div class="text-xs text-slate-400 mt-1">
              设置 SIP INVITE 出站头改写规则（对齐 FreeSWITCH sofia_glue.c 原生变量）。保存后点"Reload 到 C 侧"生效。
            </div>
          </el-form-item>
          <!-- REQ-DP-APP-001: Application 链编辑器 -->
          <el-form-item label="拨号动作链（Application Chain）">
            <div class="w-full">
              <div class="flex justify-between items-center mb-1">
                <span class="text-xs text-slate-500">顺序执行：pre_answer → playback → sleep → answer …（空链则跳过）</span>
                <el-button link size="small" type="primary" @click="rawMode.app_chain = !rawMode.app_chain">
                  {{ rawMode.app_chain ? '结构化编辑' : 'Raw JSON' }}
                </el-button>
              </div>
              <el-input v-if="rawMode.app_chain" v-model="form.app_chain" type="textarea" :rows="6"
                placeholder='JSON 数组，例如 [{"app":"pre_answer"},{"app":"playback","data":"welcome.wav"},{"app":"answer"}]' resize="none"></el-input>
              <dp-app-chain-editor v-else v-model="form.app_chain"></dp-app-chain-editor>
            </div>
            <div class="text-xs text-slate-400 mt-1">
              选中命中本条规则后依次执行的 Application。C 侧 dp_execute 命中规则 → dp_execute_app_chain 逐条调 app_registry 中注册的 17 个内建 apps。
              <code class="bg-slate-100 px-1 rounded ml-1">sleep</code> 返回 PENDING 时 timer_wheel 挂起，到点后自动继续。
            </div>
          </el-form-item>
        </el-form>
        <template #footer>
          <el-button @click="showModal = false">取消</el-button>
          <el-button type="primary" :loading="saving" @click="save">保存</el-button>
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
    // 正则验证状态
    const matchValid = ref(null);    // null=未验证, true=合法, false=非法
    const matchError = ref('');
    const matchTestNum = ref('');
    const matchTestResult = ref(null); // null=未测试, true=能匹配, false=不匹配

    // 外呼下拉选项（纯路由，不再自动拼 |${N} 组合——与 dst_override 解耦）
    const trunkOptions = ref([]);     // [{ value: 'trunk:xxx', label }]
    const gatewayOptions = ref([]);    // [{ value: 'gateway:xxx', label }]
    const trunkApiOk = ref(false);
    // 租户下拉（tenant_id 外键）
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
          message: `[诊断] 拉到 ${r.items?.length || 0} 条 trunk，其中 enabled=${enabled.length}`,
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

    /* 本地按 profile 过滤 + 分页 */
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
          message: `已加载到 C 侧：${r.loaded} 条规则`,
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
    // 从 action_json 里安全地取出顶层字段（dst_override 等），不存在返回默认值
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
    /* profile 切换时智能填默认 CIDR（internal→RFC1918 防盗打，external→0.0.0.0/0 外部网关） */
    function onProfileChange() {
      if (!editingId.value) {
        form.value.src_ip_cidr = form.value.profile_name === 'internal' ? RFC1918 : '0.0.0.0/0';
      }
    }
    /* 正则表达式验证：JS RegExp 编译测试 + 可选号码匹配测试。
     * 注：NSBC C 侧用 POSIX ERE，JS 语法与 POSIX ERE 基本兼容，
     * 但反向引用（\1 \2）等高级特性两端支持不同。这里做基础校验，
     * 避免明显拼写错误（未闭合括号/方括号等）。 */
    function validateMatchExpr() {
      const expr = form.value.match_expr || '';
      matchTestResult.value = null;
      if (!expr.trim()) {
        matchValid.value = false;
        matchError.value = '表达式为空';
        return;
      }
      try {
        new RegExp(expr);
        matchValid.value = true;
        matchError.value = '';
        // 如果有测试号码，同时跑一下匹配
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
        matchError.value = e.message || '语法错误';
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
      } catch (e) { alert('保存失败: ' + e.message); }
      finally { saving.value = false; }
    }
    async function del(id) {
      try {
        await API.delete(API_PATH + '/' + id);
        await load();
      } catch (e) { alert('删除失败: ' + e.message); }
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
    { key: 'name', label: '名称', type: 'text' },
    { key: 'enabled', label: '状态', type: 'bool' },
    { key: 'fallback_to', label: '回落至', type: 'text' },
    { key: 'description', label: '备注', type: 'text' },
  ],
  [
    { key: 'name', label: '名称', type: 'text' },
    { key: 'enabled', label: '启用', type: 'bool', default: true },
    { key: 'time_ranges', label: '时间范围', type: 'json', editor: 'time-ranges',
      default: '[{"days":"1-5","from":"09:00","to":"18:00"}]' },
    { key: 'fallback_to', label: '回落目标', type: 'text' },
    { key: 'description', label: '备注', type: 'textarea' },
  ],
));

// --- 分机队列 ---
app.component('queues', crudTable(
  { label: '分机队列', icon: 'fa-solid fa-people-group' },
  '/queues',
  [
    { key: 'tenant_id', label: '租户ID', type: 'text' },
    { key: 'name', label: '名称', type: 'text' },
    { key: 'enabled', label: '状态', type: 'bool' },
    { key: 'ring_strategy', label: '策略', type: 'text' },
    { key: 'max_wait_sec', label: '最大等待', type: 'text' },
    { key: 'moh_sound', label: '保持音', type: 'text' },
  ],
  [
    { key: 'tenant_id', label: '租户', type: 'async-select',
      help: '所属租户（列表会按当前租户自动过滤）' },
    { key: 'name', label: '名称', type: 'text' },
    { key: 'enabled', label: '启用', type: 'bool', default: true },
    { key: 'ring_strategy', label: '振铃策略', type: 'select', options: [
      { value: 'ringall', label: 'ringall (全振铃)' },
      { value: 'leastrecent', label: 'leastrecent (最近空闲)' },
      { value: 'roundrobin', label: 'roundrobin (轮询)' },
      { value: 'random', label: 'random (随机)' },
      { value: 'fewestcalls', label: 'fewestcalls (最少呼叫)' },
    ], default: 'ringall' },
    { key: 'max_wait_sec', label: '最大等待 (秒)', type: 'number', default: 600 },
    { key: 'moh_sound', label: '保持音乐', type: 'text', default: 'default' },
    { key: 'members_json', label: '队列成员', type: 'json', editor: 'queue-members',
      default: '[{"extension":"2000","weight":10,"enabled":true}]' },
    { key: 'description', label: '备注', type: 'textarea' },
  ],
  // asyncLoaders：动态下拉数据源
  {
    tenant_id: async () => {
      try {
        const r = await API.get('/tenants?limit=500&offset=0');
        const opts = (r.items || []).map(t => ({ value: t.id, label: t.tenant_name + (t.enabled ? '' : ' (禁用)') }));
        if (!opts.length) console.warn('[tenant_id async-loader] /tenants 返回空列表——检查 DB 是否有默认租户');
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
    { key: 'name', label: '名称', type: 'text' },
    { key: 'enabled', label: '状态', type: 'bool' },
    { key: 'timeout_sec', label: '超时', type: 'text' },
    { key: 'max_attempts', label: '重试次数', type: 'text' },
    { key: 'prompt_audio', label: '主提示音', type: 'text' },
  ],
  [
    { key: 'name', label: '名称', type: 'text' },
    { key: 'enabled', label: '启用', type: 'bool', default: true },
    { key: 'prompt_audio', label: '主提示音', type: 'text' },
    { key: 'invalid_audio', label: '无效提示音', type: 'text' },
    { key: 'timeout_sec', label: '超时 (秒)', type: 'number', default: 10 },
    { key: 'max_attempts', label: '最大尝试', type: 'number', default: 3 },
    { key: 'options_json', label: '按键选项', type: 'json', editor: 'ivr-options',
      default: '[{"key":"1","target":"queue-sales"},{"key":"2","target":"vm"}]' },
    { key: 'timeout_target', label: '超时跳转', type: 'text' },
    { key: 'invalid_target', label: '无效跳转', type: 'text' },
    { key: 'description', label: '备注', type: 'textarea' },
  ],
));

// --- 高可用组 ---
app.component('ha-groups', crudTable(
  { label: '高可用组', icon: 'fa-solid fa-server' },
  '/ha-groups',
  [
    { key: 'name', label: '名称', type: 'text' },
    { key: 'enabled', label: '状态', type: 'bool' },
    { key: 'quorum', label: '法定人数', type: 'text' },
    { key: 'state', label: '当前状态', type: 'text' },
  ],
  [
    { key: 'name', label: '名称', type: 'text' },
    { key: 'enabled', label: '启用', type: 'bool', default: true },
    { key: 'node_list_json', label: '节点列表', type: 'json', editor: 'ha-node-list',
      default: '[{"node_id":"node-1","host":"10.0.0.25","port":8080,"role":"slave"}]' },
    { key: 'quorum', label: '法定人数', type: 'number', default: 2 },
    { key: 'election_timeout', label: '选主超时 (ms)', type: 'number', default: 5000 },
    { key: 'description', label: '备注', type: 'textarea' },
  ],
));

// --- SIP Profile ---
app.component('sip-profiles', crudTable(
  { label: 'SIP 传输 Profile', icon: 'fa-solid fa-network-wired' },
  '/sip-profiles',
  [
    { key: 'name', label: '名称', type: 'text' },
    { key: 'enabled', label: '状态', type: 'bool' },
    { key: 'sip_ip', label: 'SIP 绑定 IP', type: 'text' },
    { key: 'sip_udp_port', label: 'UDP 端口', type: 'text' },
    { key: 'dtmf_mode', label: 'DTMF 模式', type: 'text' },
  ],
  [
    { key: 'name', label: 'Profile 名称', type: 'text' },
    { key: 'enabled', label: '启用', type: 'bool', default: true },
    // ---- SIP 监听 ----
    { key: 'sip_ip', label: 'SIP 绑定 IP', type: 'text', default: '0.0.0.0' },
    { key: 'external_sip_ip', label: 'SIP NAT 公网 IP', type: 'text' },
    { key: 'sip_udp_port', label: 'UDP 端口', type: 'number', default: 5060 },
    { key: 'sip_tcp_port', label: 'TCP 端口', type: 'number', default: 5060 },
    { key: 'sip_tls_port', label: 'TLS 端口', type: 'number', default: 5061 },
    { key: 'enable_udp', label: '启用 UDP', type: 'bool', default: true },
    { key: 'enable_tcp', label: '启用 TCP', type: 'bool', default: true },
    { key: 'enable_tls', label: '启用 TLS', type: 'bool', default: false },
    // ---- RTP ----
    { key: 'rtp_ip', label: 'RTP 绑定 IP', type: 'text', default: '0.0.0.0' },
    { key: 'external_rtp_ip', label: 'RTP NAT 公网 IP', type: 'text' },
    { key: 'rtp_port_start', label: 'RTP 端口起始', type: 'number', default: 10000 },
    { key: 'rtp_port_end', label: 'RTP 端口结束', type: 'number', default: 59999 },
    // ---- 协议特性 ----
    { key: 'enable_prack', label: '启用 PRACK (RFC 3262)', type: 'bool', default: false },
    { key: 'enable_update', label: '启用 UPDATE (RFC 3311)', type: 'bool', default: true },
    { key: 'dtmf_mode', label: 'DTMF 模式', type: 'select', options: [
      { value: 'rfc4733', label: 'RFC 4733 telephone-event (推荐)' },
      { value: 'info',    label: 'SIP INFO application/dtmf-relay' },
      { value: 'inband',  label: 'Inband 双频 G.711' },
      { value: 'auto',    label: '自动 (SDP 协商优先 RFC4733)' },
    ], default: 'rfc4733' },
    { key: 'realm', label: 'SIP Realm', type: 'text', default: 'nsbc.local' },
    { key: 'description', label: '备注', type: 'textarea' },
  ],
));
