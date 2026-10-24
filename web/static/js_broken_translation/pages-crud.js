// =============================================================================
// NSBC Web — CRUD 实体页面组件
// 加载顺序：必须在 app-core.js 之后
// 依赖：app-core.js 中声明的全局 const app, crudTable
// =============================================================================

// --- 租户 ---
app.component('tenants', crudTable(
  { label: t('tenant.title'), icon: 'fa-solid fa-building' },
  '/tenants',
  [
    { key: 'id', label: 'ID', type: 'text' },
    { key: 'tenant_name', label: t('tenant.matchFrom'), type: 'text' },
    { key: 'enabled', label: t('generic.state'), type: 'bool' },
  ],
  [
    { key: 'tenant_name', label: t('tenant.name'), type: 'text',
      help: t('crud.tenant.realm.help') },
    { key: 'enabled', label: t('generic.enabled'), type: 'bool', default: true },
    { key: 'description', label: t('generic.remarks'), type: 'textarea' },
    { key: 'match_domain', label: t('tenant.matchFrom'), type: 'text',
      placeholder: 'acme.com', help: t('crud.tenant.matchDomain.help') },
    { key: 'match_did_prefix', label: t('tenant.matchDid'), type: 'text',
      placeholder: '+86101234', help: t('crud.tenant.matchDid.help') },
    { key: 'match_trunk', label: t('tenant.matchTrunk'), type: 'text',
      placeholder: 'OperatorA', help: t('crud.tenant.matchTrunk.help') },
  ],
));

// --- 分机 ---
app.component('extensions', crudTable(
  { label: t('ext.title'), icon: 'fa-solid fa-phone' },
  '/extensions',
  [
    { key: 'tenant_id', label: 'Tenant ID', type: 'text' },
    { key: 'extension', label: t('ext.number'), type: 'text' },
    { key: 'ext_type', label: t('ext.type'), type: 'text' },
    { key: 'full_name', label: t('ext.fullName'), type: 'text' },
    { key: 'enabled', label: t('generic.state'), type: 'bool' },
  ],
  [
    { key: 'tenant_id', label: t('tenant.name'), type: 'async-select',
      help: t('crud.common.tenant.help') },
    { key: 'extension', label: t('ext.number'), type: 'text' },
    { key: 'ext_type', label: t('ext.type'), type: 'select',
      options: [{value:'internal', label:t('ext.internal')},
                {value:'external', label:t('ext.external')}],
      default: 'internal', help: t('ext.externalHint') },
    { key: 'password', label: t('login.password'), type: 'password', help: t('crud.ext.password.help') },
    { key: 'full_name', label: t('ext.fullName'), type: 'text' },
    { key: 'enabled', label: t('generic.enabled'), type: 'bool', default: true },
    { key: 'description', label: t('generic.remarks'), type: 'textarea' },
    // 外部分机配置（ext_type=external 时有效）
    { key: 'external_number', label: t('ext.externalNumber'), type: 'text',
      placeholder: '+8613800138000', help: t('crud.ext.externalNumber.help') },
    { key: 'trunk_name', label: t('ext.outboundTrunk'), type: 'text',
      placeholder: 'OperatorIMS', help: t('crud.ext.outboundTrunk.help') },
    { key: 'display_number', label: t('ext.callerId'), type: 'text',
      placeholder: '+861012345678', help: t('crud.ext.callerId.help') },
  ],
  // asyncLoaders：动态下拉数据源
  {
    tenant_id: async () => {
      try {
        const r = await API.get('/tenants?limit=500&offset=0');
        const opts = (r.items || []).map(t => ({ value: t.id, label: t.tenant_name + (t.enabled ? '' : ' ' + t('tenant.disabled')) }));
        if (!opts.length) console.warn('[tenant_id async-loader] /tenants returned empty — check DB for default tenant');
        return opts;
      } catch (e) {
        console.error('[tenant_id async-loader] /tenants load failed:', e.message || e);
        return [];
      }
    },
  },
));

// --- 网关 ---
app.component('gateways', crudTable(
  { label: t('gw.title'), icon: 'fa-solid fa-tower-broadcast' },
  '/gateways',
  [
    { key: 'name', label: t('generic.name'), type: 'text' },
    { key: 'enabled', label: t('generic.state'), type: 'bool' },
    { key: 'description', label: t('generic.remarks'), type: 'text' },
  ],
  [
    { key: 'name', label: t('generic.name'), type: 'text' },
    { key: 'enabled', label: t('generic.enabled'), type: 'bool', default: true },
    { key: 'description', label: t('generic.remarks'), type: 'textarea' },
  ],
));

// --- 中继 ---
app.component('trunks', crudTable(
  { label: t('trunk.title'), icon: 'fa-solid fa-signal' },
  '/trunks',
  [
    { key: 'tenant_id', label: 'Tenant ID', type: 'text' },
    { key: 'name', label: t('generic.name'), type: 'text' },
    { key: 'gateway_name', label: t('gw.title'), type: 'text' },
    { key: 'kind', label: t('ext.type'), type: 'text' },
    { key: 'host', label: t('trunk.host'), type: 'text' },
    { key: 'port', label: t('trunk.port'), type: 'text' },
    { key: 'enabled', label: t('generic.state'), type: 'bool' },
  ],
  [
    { key: 'tenant_id', label: t('tenant.name'), type: 'async-select',
      help: t('crud.common.tenant.help') },
    { key: 'name', label: t('generic.name'), type: 'text' },
    { key: 'gateway_name', label: 'Gateway', type: 'async-select',
      help: t('crud.trunk.gateway.help') },
    { key: 'profile_name', label: t('sipProfile.title'), type: 'async-select', default: 'external',
      help: t('crud.trunk.profile.help') },
    { key: 'kind', label: t('crud.trunk.kind.label'), type: 'select', options: [
      { value: 'static_peer', label: t('crud.trunk.kind.staticPeer') },
      { value: 'register',    label: t('crud.trunk.kind.register') },
      { value: 'auth_out',    label: t('crud.trunk.kind.authOut') },
    ]},
    { key: 'proto', label: t('trunk.proto'), type: 'select', options: [
      { value: 0, label: 'UDP (0)' }, { value: 1, label: 'TCP (1)' }, { value: 2, label: 'TLS (2)' },
    ]},
    { key: 'host', label: t('trunk.host'), type: 'text' },
    { key: 'port', label: t('trunk.port'), type: 'number', default: 5060 },
    { key: 'auth_user', label: t('trunk.authUser'), type: 'text' },
    { key: 'password', label: t('trunk.authPwd'), type: 'text', editor: 'password' },
    { key: 'realm', label: t('tenant.matchFrom'), type: 'text' },
    { key: 'codec_set', label: t('trunk.codec'), type: 'text',
      default: 'PCMA,PCMU' },
    { key: 'weight', label: t('trunk.weight'), type: 'number', default: 10 },
    { key: 'priority', label: t('trunk.priority'), type: 'number', default: 1 },
    { key: 'max_channels', label: t('trunk.maxChan'), type: 'number', default: 0 },
    { key: 'per_number_max_channels', label: t('trunk.perNumMax'), type: 'number', default: 0 },
    { key: 'caller_id_pool', label: t('trunk.callerPool'), type: 'textarea',
      placeholder: '8613800000000,8613800000001,8613800000002' },
    { key: 'enabled', label: t('generic.enabled'), type: 'bool', default: true },
    { key: 'description', label: t('generic.remarks'), type: 'textarea' },
    { key: 'from_user', label: t('crud.trunk.fromUser.label'), type: 'text', default: '' },
    { key: 'from_domain', label: t('crud.trunk.fromDomain.label'), type: 'text', default: '' },
    { key: 'proxy', label: t('crud.trunk.proxy.label'), type: 'text', default: '' },
    { key: 'register_proxy', label: t('crud.trunk.registerProxy.label'), type: 'text', default: '' },
    { key: 'expire_seconds', label: t('crud.trunk.expire.label'), type: 'number', default: 3600 },
    { key: 'retry_seconds', label: t('crud.trunk.retry.label'), type: 'number', default: 5 },
    { key: 'caller_id_in_from', label: t('crud.trunk.callerIdInFrom.label'), type: 'bool', default: false },
    { key: 'contact_params', label: t('crud.trunk.contactParams.label'), type: 'text', default: '' },
    { key: 'extension_in_contact', label: t('crud.trunk.extInContact.label'), type: 'bool', default: false },
    { key: 'ping_seconds', label: t('crud.trunk.ping.label'), type: 'number', default: 0 },
    { key: 'inbound_extension', label: t('crud.trunk.inboundExt.label'), type: 'text', default: '' },
    { key: 'cid_type', label: t('crud.trunk.cidType.label'), type: 'text', default: '' },
    { key: 'rfc5626_enabled', label: t('crud.trunk.rfc5626.label'), type: 'bool', default: false },
    { key: 'reg_id', label: t('crud.trunk.regId.label'), type: 'text', default: '' },
    { key: 'peer_ip', label: t('crud.trunk.peerIp.label'), type: 'textarea', default: '' },
    { key: 'ims_enabled', label: t('crud.trunk.imsEnabled.label'), type: 'bool', default: false },
    { key: 'ims_domain', label: t('crud.trunk.imsDomain.label'), type: 'text', default: '',
      placeholder: 'ims.mnc001.mcc460.3gppnetwork.org',
      help: t('crud.trunk.imsDomain.help') },
    { key: 'ims_auto_pani', label: t('crud.trunk.imsAutoPani.label'), type: 'bool', default: false },
    { key: 'ims_pani_value', label: t('crud.trunk.imsPaniValue.label'), type: 'text', default: '',
      placeholder: 'E-UTRAN-TDD; utran-cell-id-3gpp=0001001' },
    { key: 'ims_auto_privacy', label: t('crud.trunk.imsAutoPrivacy.label'), type: 'bool', default: false },
    { key: 'custom_headers', label: t('crud.trunk.customHeaders.label'), type: 'textarea', default: '',
      placeholder: 'MyID=${callid}\\r\\nX-Biz-Caller=${caller}\\r\\nX-Biz-Callee=${callee}',
      help: t('crud.trunk.customHeaders.help') },
  ],
  // asyncLoaders：动态下拉数据源
  {
    tenant_id: async () => {
      try {
        const r = await API.get('/tenants?limit=500&offset=0');
        const opts = (r.items || []).map(t => ({ value: t.id, label: t.tenant_name + (t.enabled ? '' : ' ' + t('tenant.disabled')) }));
        if (!opts.length) console.warn('[trunks tenant_id] /tenants returned empty list');
        return opts;
      } catch (e) {
        console.error('[trunks tenant_id] /tenants load failed:', e.message || e);
        return [];
      }
    },
    gateway_name: async () => {
      try {
        const r = await API.get('/gateways?limit=500&offset=0');
        return (r.items || []).map(g => ({ value: g.name, label: `${g.name}${g.enabled ? '' : ' (disabled)'}` }));
      } catch (e) { return []; }
    },
    profile_name: async () => {
      try {
        const r = await API.get('/sip-profiles?limit=500&offset=0');
        return (r.items || []).map(p => ({ value: p.name, label: `${p.name}${p.enabled ? '' : ' (disabled)'}` }));
      } catch (e) { return []; }
    },
  },
));

// --- 路由组 ---
app.component('route-groups', crudTable(
  { label: t('menu.routes'), icon: 'fa-solid fa-route' },
  '/routes',
  [
    { key: 'name', label: t('generic.name'), type: 'text' },
    { key: 'strategy', label: t('generic.strategy'), type: 'text' },
    { key: 'enabled', label: t('generic.state'), type: 'bool' },
  ],
  [
    { key: 'name', label: t('generic.name'), type: 'text' },
    { key: 'strategy', label: 'Strategy', type: 'select', options: [
      { value: 'priority', label: t('crud.route.strategy.priority') },
      { value: 'weight',   label: t('crud.route.strategy.weight') },
      { value: 'least_busy', label: t('crud.route.strategy.leastBusy') },
      { value: 'ring',     label: t('crud.route.strategy.ring') },
    ], default: 'priority' },
    { key: 'enabled', label: t('generic.enabled'), type: 'bool', default: true },
    { key: 'description', label: t('generic.remarks'), type: 'textarea' },
  ],
));

/* §15 Dialplan rules with internal/external Profile Tab 分区
 * CIDR defaults: internal → RFC1918 private (防 fraud); external → 0.0.0.0/0 */
app.component('dialplan-rules', {
  template: `
    <div class="space-y-4">
      <div class="flex items-center justify-between">
        <h2 class="text-lg font-semibold text-slate-800"><i class="fa-solid fa-code-branch mr-2 text-blue-600"></i>${ t('crud.dp.title') }</h2>
        <div class="flex items-center gap-2">
          <el-button size="small" @click="reloadRules">
            <el-icon><Refresh /></el-icon>${ t('dp.reloadC') }
          </el-button>
          <el-button type="primary" size="small" @click="openCreate">
            <el-icon><Plus /></el-icon>${ t('generic.new') }
          </el-button>
        </div>
      </div>

      <el-tabs v-model="activeProfile" type="border-card">
        <el-tab-pane :label="t('dp.profileInternal')" name="internal">
          <el-table :data="pagedItems" border stripe size="default" style="width:100%">
            <el-table-column type="index" label="#" width="55" align="center"></el-table-column>
            <el-table-column prop="tenant_id" :label="t('crud.dp.tenantCol')" width="80" align="center">
              <template #default="{ row }"><el-tag size="small" effect="plain">T{{ row.tenant_id || 1 }}</el-tag></template>
            </el-table-column>
            <el-table-column prop="name" :label="t('dp.name')" min-width="140"></el-table-column>
            <el-table-column prop="match_expr" :label="t('dp.pattern')" min-width="140">
              <template #default="{ row }"><code class="text-xs bg-slate-100 px-1 rounded">{{ row.match_expr }}</code></template>
            </el-table-column>
            <el-table-column prop="target" :label="t('dp.target')" min-width="160"></el-table-column>
            <el-table-column prop="rec_action" :label="t('crud.dp.recCol')" width="90" align="center">
              <template #default="{ row }">
                <el-tag v-if="row.rec_action === 1" type="danger" effect="light" size="small" round>Start</el-tag>
                <el-tag v-else-if="row.rec_action === 2" type="warning" effect="light" size="small" round>Stop</el-tag>
                <span v-else class="text-slate-400 text-xs">—</span>
              </template>
            </el-table-column>
            <el-table-column prop="rec_path_template" :label="t('crud.dp.recPathCol')" min-width="200">
              <template #default="{ row }">
                <code v-if="row.rec_path_template" class="text-xs text-emerald-600">{{ row.rec_path_template }}</code>
                <span v-else class="text-slate-400 text-xs">${ t('generic.default') }</span>
              </template>
            </el-table-column>
            <el-table-column prop="prio" :label="t('dp.priority')" width="80" align="center"></el-table-column>
            <el-table-column prop="src_ip_cidr" :label="t('crud.dp.ipCidrCol')" min-width="180">
              <template #default="{ row }">
                <code class="text-xs" :class="row.src_ip_cidr === '0.0.0.0/0' ? 'text-amber-600' : 'text-slate-500'">{{ row.src_ip_cidr }}</code>
              </template>
            </el-table-column>
            <el-table-column prop="enabled" :label="t('generic.state')" width="80" align="center">
              <template #default="{ row }">
                <el-tag :type="row.enabled ? 'success' : 'info'" effect="light" size="small" round>
                  {{ row.enabled ? t('generic.enabled') : t('generic.disabled') }}
                </el-tag>
              </template>
            </el-table-column>
            <el-table-column :label="t('table.actions')" width="150" align="center" fixed="right">
              <template #default="{ row }">
                <el-button link type="primary" size="small" @click="openEdit(row)">{{ t('generic.edit') }}</el-button>
                <el-popconfirm :title="t('generic.confirmDelete')" @confirm="del(row.id)">
                  <template #reference><el-button link type="danger" size="small">{{ t('generic.delete') }}</el-button></template>
                </el-popconfirm>
              </template>
            </el-table-column>
          </el-table>
          <div class="flex justify-end pt-2">
            <el-pagination v-model:current-page="page" :page-size="limit" :total="filteredTotal"
              layout="total, prev, pager, next" small background></el-pagination>
          </div>
        </el-tab-pane>

        <el-tab-pane :label="t('dp.profileExternal')" name="external">
          <el-table :data="pagedItems" border stripe size="default" style="width:100%">
            <el-table-column type="index" label="#" width="55" align="center"></el-table-column>
            <el-table-column prop="tenant_id" :label="t('crud.dp.tenantCol')" width="80" align="center">
              <template #default="{ row }"><el-tag size="small" effect="plain">T{{ row.tenant_id || 1 }}</el-tag></template>
            </el-table-column>
            <el-table-column prop="name" :label="t('dp.name')" min-width="140"></el-table-column>
            <el-table-column prop="match_expr" :label="t('dp.pattern')" min-width="140">
              <template #default="{ row }"><code class="text-xs bg-slate-100 px-1 rounded">{{ row.match_expr }}</code></template>
            </el-table-column>
            <el-table-column prop="target" :label="t('dp.target')" min-width="160"></el-table-column>
            <el-table-column prop="rec_action" :label="t('crud.dp.recCol')" width="90" align="center">
              <template #default="{ row }">
                <el-tag v-if="row.rec_action === 1" type="danger" effect="light" size="small" round>Start</el-tag>
                <el-tag v-else-if="row.rec_action === 2" type="warning" effect="light" size="small" round>Stop</el-tag>
                <span v-else class="text-slate-400 text-xs">—</span>
              </template>
            </el-table-column>
            <el-table-column prop="rec_path_template" :label="t('crud.dp.recPathCol')" min-width="200">
              <template #default="{ row }">
                <code v-if="row.rec_path_template" class="text-xs text-emerald-600">{{ row.rec_path_template }}</code>
                <span v-else class="text-slate-400 text-xs">${ t('generic.default') }</span>
              </template>
            </el-table-column>
            <el-table-column prop="prio" :label="t('dp.priority')" width="80" align="center"></el-table-column>
            <el-table-column prop="src_ip_cidr" :label="t('crud.dp.ipCidrCol')" min-width="180">
              <template #default="{ row }">
                <code class="text-xs" :class="row.src_ip_cidr === '0.0.0.0/0' ? 'text-amber-600' : 'text-slate-500'">{{ row.src_ip_cidr }}</code>
              </template>
            </el-table-column>
            <el-table-column prop="enabled" :label="t('generic.state')" width="80" align="center">
              <template #default="{ row }">
                <el-tag :type="row.enabled ? 'success' : 'info'" effect="light" size="small" round>
                  {{ row.enabled ? t('generic.enabled') : t('generic.disabled') }}
                </el-tag>
              </template>
            </el-table-column>
            <el-table-column :label="t('table.actions')" width="150" align="center" fixed="right">
              <template #default="{ row }">
                <el-button link type="primary" size="small" @click="openEdit(row)">{{ t('generic.edit') }}</el-button>
                <el-popconfirm :title="t('generic.confirmDelete')" @confirm="del(row.id)">
                  <template #reference><el-button link type="danger" size="small">{{ t('generic.delete') }}</el-button></template>
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

      <!-- Create/Edit dialog -->
      <el-dialog v-model="showModal" :title="(editingId ? t('generic.edit') : t('generic.new')) + ' ' + t('crud.dp.title')" width="900px" destroy-on-close>
        <el-form :model="form" label-position="top">
          <el-form-item :label="t('crud.dp.tenantCol')">
            <el-select v-model="form.tenant_id" style="width:100%" filterable>
              <el-option v-for="t in tenantOptions" :key="t.id" :value="t.id"
                :label="'T' + t.id + ' — ' + t.tenant_name + (t.enabled ? '' : ' (' + t('tenant.disabled') + ')')">
              </el-option>
            </el-select>
            <div class="text-xs text-slate-400 mt-1">${ t('crud.dp.tenantHelp') }</div>
          </el-form-item>
          <el-form-item :label="t('dp.name')"><el-input v-model="form.name" placeholder="e.g. DID 51350820 -> internal alice"></el-input></el-form-item>
          <el-form-item :label="t('crud.dp.profileLabel')">
            <el-select v-model="form.profile_name" style="width:100%" @change="onProfileChange">
              <el-option value="internal" :label="t('dp.profileInternal')"></el-option>
              <el-option value="external" :label="t('dp.profileExternal')"></el-option>
            </el-select>
            <div class="text-xs text-slate-400 mt-1">${ t('crud.dp.profileHelp') }</div>
          </el-form-item>
          <el-form-item :label="t('crud.dp.ipCidrLabel') + (form.profile_name === 'external' ? t('crud.dp.ipCidrExtSuffix') : t('crud.dp.ipCidrIntSuffix'))">
            <el-input v-model="form.src_ip_cidr" placeholder="10.0.0.0/8,172.16.0.0/12,192.168.0.0/16"></el-input>
          </el-form-item>
          <el-form-item :label="t('crud.dp.patternLabel')">
            <div class="flex gap-2 items-start w-full">
              <el-input v-model="form.match_expr" :placeholder="t('crud.dp.patternPlaceholder')" style="flex:1"></el-input>
              <el-button @click="validateMatchExpr" :type="matchValid === null ? 'info' : (matchValid ? 'success' : 'danger')">${ t('dp.validateExpr') }</el-button>
            </div>
            <div v-if="matchValid !== null" class="mt-1 text-xs" :class="matchValid ? 'text-green-600' : 'text-red-500'">
              <span v-if="matchValid">✓ ${ t('dp.exprValid') }</span>
              <span v-else>✗ ${ t('dp.exprInvalid') }: {{ matchError }}</span>
            </div>
            <div v-if="matchValid" class="mt-1 text-xs text-slate-500">
              <div class="flex gap-2 items-center mt-1">
                <span>${ t('dp.testNum') }:</span>
                <el-input v-model="matchTestNum" size="small" style="width:160px" placeholder="e.g. 11001"></el-input>
                <el-tag v-if="matchTestResult !== null" :type="matchTestResult ? 'success' : 'info'" size="small">
                  {{ matchTestResult ? '✓ ' + t('dp.canMatch') : '✗ ' + t('dp.cannotMatch') }}
                </el-tag>
              </div>
            </div>
          </el-form-item>
          <el-form-item :label="t('dp.matchCaller')"><el-input v-model="form.caller_match" placeholder="empty = no caller restriction"></el-input></el-form-item>
          <div class="grid grid-cols-2 gap-4">
            <el-form-item :label="t('crud.dp.prioLabel')">
              <el-input-number v-model="form.prio" :controls="false" style="width:100%"></el-input-number>
            </el-form-item>
            <el-form-item :label="t('dp.stripPrefix')">
              <el-input-number v-model="form.strip" :controls="false" style="width:100%"></el-input-number>
            </el-form-item>
          </div>
          <el-form-item :label="t('dp.dstOverride')">
            <el-input v-model="form.dst_override"
                      placeholder="empty = strip result; use $1 $2 or \${1} \${2}; e.g. 888$2"
                      clearable>
            </el-input>
            <div class="text-xs text-slate-400 mt-1">
              ${ t('crud.dp.dstOverrideHelp') }
            </div>
          </el-form-item>
          <el-form-item :label="t('crud.dp.targetLabel')">
            <el-select v-model="form.target" filterable allow-create default-first-option
              :placeholder="t('crud.dp.targetPlaceholder')" style="width:100%" clearable>
              <el-option-group v-if="gatewayOptions.length" label="— ${ t('struct.gwGroupOut') } —">
                <el-option v-for="o in gatewayOptions" :key="o.value" :value="o.value" :label="o.label"></el-option>
              </el-option-group>
              <el-option-group v-if="trunkOptions.length" label="— ${ t('struct.trunkDirect') } —">
                <el-option v-for="o in trunkOptions" :key="o.value" :value="o.value" :label="o.label"></el-option>
              </el-option-group>
              <el-option-group v-if="trunkApiOk && !trunkOptions.length && !gatewayOptions.length" label="— OUT —">
                <el-option disabled value="" :label="t('crud.dp.noTargetsTip')"></el-option>
              </el-option-group>
              <el-option-group :label="t('struct.extBiz')">
                <el-option value="extension:" label="extension:<ext> -> EXT"></el-option>
                <el-option value="queue:"     label="queue:<queue> -> BIZ"></el-option>
                <el-option value="ivr:"       label="ivr:<IVR> -> BIZ"></el-option>
                <el-option value="hangup"     label="hangup -> BIZ"></el-option>
              </el-option-group>
            </el-select>
            <div class="text-xs text-slate-400 mt-1">
              ${ t('crud.dp.targetHelp') }
            </div>
          </el-form-item>
          <div class="grid grid-cols-2 gap-4">
            <el-form-item :label="t('dp.recording')">
              <el-select v-model="form.rec_action" style="width:100%">
                <el-option :value="0" label="None (default)"></el-option>
                <el-option :value="1" label="Start"></el-option>
                <el-option :value="2" label="Stop"></el-option>
              </el-select>
              <div class="text-xs text-slate-400 mt-1">0=none 1=start 2=stop</div>
            </el-form-item>
            <el-form-item :label="t('dp.mediaMode')">
              <el-select v-model="form.media_mode" style="width:100%">
                <el-option :value="0" label="auto (default)"></el-option>
                <el-option :value="1" label="bypass"></el-option>
                <el-option :value="2" label="relay"></el-option>
              </el-select>
              <div class="text-xs text-slate-400 mt-1">
                ${ t('struct.mediaDescDetail') }
              </div>
            </el-form-item>
          </div>
          <el-form-item :label="t('dp.recPathTemplate')">
            <el-input v-model="form.rec_path_template"
                      placeholder="e.g. YYYY-MM-DD/{caller}-{callee}-{callid}.wav"
                      clearable>
            </el-input>
            <div class="text-xs text-slate-400 mt-1">
              ${ t('struct.recPathHint') }
            </div>
          </el-form-item>
          <el-form-item :label="t('generic.enabled')">
            <el-switch v-model="form.enabled" :active-text="t('generic.enabled')" :inactive-text="t('generic.disabled')"></el-switch>
          </el-form-item>
          <el-form-item :label="t('generic.remarks')"><el-input v-model="form.description" type="textarea" :rows="2"></el-input></el-form-item>
          <el-form-item :label="t('dp.actionJson')">
            <div class="w-full">
              <div class="flex justify-between items-center mb-1">
                <span class="text-xs text-slate-500">${ t('struct.switchToStruct') } / Raw JSON</span>
                <el-button link size="small" type="primary" @click="rawMode.action_json = !rawMode.action_json">
                  {{ rawMode.action_json ? t('struct.switchToStruct') : 'Raw JSON' }}
                </el-button>
              </div>
              <el-input v-if="rawMode.action_json" v-model="form.action_json" type="textarea" :rows="6"
                :placeholder="t('struct.inputJson')" resize="none"></el-input>
              <dp-action-editor v-else v-model="form.action_json"></dp-action-editor>
            </div>
            <div class="text-xs text-slate-400 mt-1">
              ${ t('crud.dp.actionHelp') }
            </div>
          </el-form-item>
          <el-form-item :label="t('dp.appChain')">
            <div class="w-full">
              <div class="flex justify-between items-center mb-1">
                <span class="text-xs text-slate-500">${ t('struct.appChainOrder') }</span>
                <el-button link size="small" type="primary" @click="rawMode.app_chain = !rawMode.app_chain">
                  {{ rawMode.app_chain ? t('struct.switchToStruct') : 'Raw JSON' }}
                </el-button>
              </div>
              <el-input v-if="rawMode.app_chain" v-model="form.app_chain" type="textarea" :rows="6"
                placeholder='JSON array e.g. [{"app":"pre_answer"},{"app":"playback","data":"welcome.wav"},{"app":"answer"}]' resize="none"></el-input>
              <dp-app-chain-editor v-else v-model="form.app_chain"></dp-app-chain-editor>
            </div>
            <div class="text-xs text-slate-400 mt-1">
              ${ t('crud.dp.appChainHelp') }
            </div>
          </el-form-item>
        </el-form>
        <template #footer>
          <el-button @click="showModal = false">${ t('generic.cancel') }</el-button>
          <el-button type="primary" :loading="saving" @click="save">${ t('generic.save') }</el-button>
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
    const matchValid = ref(null);
    const matchError = ref('');
    const matchTestNum = ref('');
    const matchTestResult = ref(null);

    const trunkOptions = ref([]);
    const gatewayOptions = ref([]);
    const trunkApiOk = ref(false);
    const tenantOptions = ref([]);

    async function fetchTenants() {
      try {
        const r = await API.get('/tenants');
        tenantOptions.value = r.items || [];
      } catch (e) { /* silent */ }
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
          message: `[diag] loaded ${r.items?.length || 0} trunks, enabled=${enabled.length}`,
          type: enabled.length ? 'success' : 'warning',
          duration: 4000, showClose: true,
        });
      } catch (e) {
        trunkApiOk.value = false;
        ElementPlus.ElMessage({
          message: `[diag] fetchTrunkOptions failed: ${e.message}`,
          type: 'error', duration: 6000, showClose: true,
        });
      }
    }

    async function fetchGatewayOptions() {
      try {
        const r = await API.get('/gateways?limit=500&offset=0');
        const enabled = (r.items || []).filter(g => g.enabled);
        gatewayOptions.value = enabled.map(g => ({
          value: 'gateway:' + g.name, label: `group: ${g.name}`,
        }));
      } catch (e) { /* silent */ }
    }

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
          message: t('crud.dp.reloadSuccess') + ': ' + r.loaded,
          type: 'success',
          duration: 3000,
          showClose: true,
        });
        await load();
      } catch (e) {
        ElementPlus.ElMessage({
          message: t('crud.dp.reloadFail') + ': ' + e.message,
          type: 'error',
          duration: 4000,
          showClose: true,
        });
      }
    }
    function actGet(action_json, key, def) {
      try {
        const o = JSON.parse(action_json || '{}');
        return o[key] ?? def;
      } catch { return def; }
    }
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
      f.dst_override = actGet(row.action_json, 'dst_override', '');
      f.media_mode = Number(actGet(row.action_json, 'media_mode', 0));
      if (!f.app_chain || typeof f.app_chain !== 'string' || !f.app_chain.trim().startsWith('['))
        f.app_chain = '[]';
      form.value = f;
      rawMode.action_json = false;
      rawMode.app_chain = false;
      editingId.value = row.id;
      showModal.value = true;
    }
    function onProfileChange() {
      if (!editingId.value) {
        form.value.src_ip_cidr = form.value.profile_name === 'internal' ? RFC1918 : '0.0.0.0/0';
      }
    }
    function validateMatchExpr() {
      const expr = form.value.match_expr || '';
      matchTestResult.value = null;
      if (!expr.trim()) {
        matchValid.value = false;
        matchError.value = t('struct.validateError');
        return;
      }
      try {
        new RegExp(expr);
        matchValid.value = true;
        matchError.value = '';
        const tn = matchTestNum.value.trim();
        if (tn) {
          try {
            const re = new RegExp('^(?:' + expr + ')$');
            matchTestResult.value = re.test(tn);
          } catch {
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
        matchError.value = e.message || t('struct.validateSyntax');
      }
    }
    async function save() {
      saving.value = true;
      try {
        const payload = { ...form.value };
        payload.action_json = actSet(payload.action_json, {
          dst_override: payload.dst_override || '',
          media_mode: payload.media_mode ?? 0,
        });
        delete payload.dst_override;
        delete payload.media_mode;
        if (editingId.value) await API.put(API_PATH + '/' + editingId.value, payload);
        else await API.post(API_PATH, payload);
        showModal.value = false;
        await load();
      } catch (e) { alert(t('crud.dp.saveFail') + ': ' + e.message); }
      finally { saving.value = false; }
    }
    async function del(id) {
      try {
        await API.delete(API_PATH + '/' + id);
        await load();
      } catch (e) { alert(t('crud.dp.deleteFail') + ': ' + e.message); }
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
  { label: t('menu.timeGroups'), icon: 'fa-solid fa-clock' },
  '/time-groups',
  [
    { key: 'name', label: t('generic.name'), type: 'text' },
    { key: 'enabled', label: t('generic.state'), type: 'bool' },
    { key: 'fallback_to', label: 'Fallback to', type: 'text' },
    { key: 'description', label: t('generic.remarks'), type: 'text' },
  ],
  [
    { key: 'name', label: t('generic.name'), type: 'text' },
    { key: 'enabled', label: t('generic.enabled'), type: 'bool', default: true },
    { key: 'time_ranges', label: t('struct.timeRuleDesc'), type: 'json', editor: 'time-ranges',
      default: '[{"days":"1-5","from":"09:00","to":"18:00"}]' },
    { key: 'fallback_to', label: 'Fallback target', type: 'text' },
    { key: 'description', label: t('generic.remarks'), type: 'textarea' },
  ],
));

// --- 分机队列 ---
app.component('queues', crudTable(
  { label: t('queue.title'), icon: 'fa-solid fa-people-group' },
  '/queues',
  [
    { key: 'tenant_id', label: 'Tenant ID', type: 'text' },
    { key: 'name', label: t('generic.name'), type: 'text' },
    { key: 'enabled', label: t('generic.state'), type: 'bool' },
    { key: 'ring_strategy', label: t('generic.strategy'), type: 'text' },
    { key: 'max_wait_sec', label: t('queue.maxWait'), type: 'text' },
    { key: 'moh_sound', label: t('queue.moh'), type: 'text' },
  ],
  [
    { key: 'tenant_id', label: t('tenant.name'), type: 'async-select',
      help: t('crud.common.tenant.help') },
    { key: 'name', label: t('generic.name'), type: 'text' },
    { key: 'enabled', label: t('generic.enabled'), type: 'bool', default: true },
    { key: 'ring_strategy', label: t('queue.ringStrategy'), type: 'select', options: [
      { value: 'ringall', label: 'ringall' },
      { value: 'leastrecent', label: 'leastrecent' },
      { value: 'roundrobin', label: 'roundrobin' },
      { value: 'random', label: 'random' },
      { value: 'fewestcalls', label: 'fewestcalls' },
    ], default: 'ringall' },
    { key: 'max_wait_sec', label: t('queue.maxWait') + ' (sec)', type: 'number', default: 600 },
    { key: 'moh_sound', label: t('queue.moh'), type: 'text', default: 'default' },
    { key: 'members_json', label: t('queue.member') + 's', type: 'json', editor: 'queue-members',
      default: '[{"extension":"2000","weight":10,"enabled":true}]' },
    { key: 'description', label: t('generic.remarks'), type: 'textarea' },
  ],
  // asyncLoaders
  {
    tenant_id: async () => {
      try {
        const r = await API.get('/tenants?limit=500&offset=0');
        const opts = (r.items || []).map(t => ({ value: t.id, label: t.tenant_name + (t.enabled ? '' : ' ' + t('tenant.disabled')) }));
        if (!opts.length) console.warn('[tenant_id async-loader] /tenants returned empty');
        return opts;
      } catch (e) {
        console.error('[tenant_id async-loader] /tenants load failed:', e.message || e);
        return [];
      }
    },
  },
));

// --- IVR 菜单 ---
app.component('ivr-menus', crudTable(
  { label: t('ivr.title'), icon: 'fa-solid fa-circle-question' },
  '/ivr-menus',
  [
    { key: 'name', label: t('generic.name'), type: 'text' },
    { key: 'enabled', label: t('generic.state'), type: 'bool' },
    { key: 'timeout_sec', label: t('ivr.timeout'), type: 'text' },
    { key: 'max_attempts', label: t('ivr.maxAttempts'), type: 'text' },
    { key: 'prompt_audio', label: t('ivr.promptAudio'), type: 'text' },
  ],
  [
    { key: 'name', label: t('generic.name'), type: 'text' },
    { key: 'enabled', label: t('generic.enabled'), type: 'bool', default: true },
    { key: 'prompt_audio', label: t('ivr.promptAudio'), type: 'text' },
    { key: 'invalid_audio', label: t('ivr.invalidAudio'), type: 'text' },
    { key: 'timeout_sec', label: t('ivr.timeout') + ' (sec)', type: 'number', default: 10 },
    { key: 'max_attempts', label: t('ivr.maxAttempts'), type: 'number', default: 3 },
    { key: 'options_json', label: t('ivr.keyOption') + 's', type: 'json', editor: 'ivr-options',
      default: '[{"key":"1","target":"queue-sales"},{"key":"2","target":"vm"}]' },
    { key: 'timeout_target', label: t('ivr.timeoutTarget'), type: 'text' },
    { key: 'invalid_target', label: t('ivr.invalidTarget'), type: 'text' },
    { key: 'description', label: t('generic.remarks'), type: 'textarea' },
  ],
));

// --- 高可用组 ---
app.component('ha-groups', crudTable(
  { label: t('ha.title'), icon: 'fa-solid fa-server' },
  '/ha-groups',
  [
    { key: 'name', label: t('generic.name'), type: 'text' },
    { key: 'enabled', label: t('generic.state'), type: 'bool' },
    { key: 'quorum', label: t('ha.quorum'), type: 'text' },
    { key: 'state', label: t('generic.state'), type: 'text' },
  ],
  [
    { key: 'name', label: t('generic.name'), type: 'text' },
    { key: 'enabled', label: t('generic.enabled'), type: 'bool', default: true },
    { key: 'node_list_json', label: t('ha.node') + 's', type: 'json', editor: 'ha-node-list',
      default: '[{"node_id":"node-1","host":"10.0.0.25","port":8080,"role":"slave"}]' },
    { key: 'quorum', label: t('ha.quorum'), type: 'number', default: 2 },
    { key: 'election_timeout', label: t('ha.electionTimeout'), type: 'number', default: 5000 },
    { key: 'description', label: t('generic.remarks'), type: 'textarea' },
  ],
));

// --- SIP Profile ---
app.component('sip-profiles', crudTable(
  { label: t('sipProfile.title'), icon: 'fa-solid fa-network-wired' },
  '/sip-profiles',
  [
    { key: 'name', label: t('generic.name'), type: 'text' },
    { key: 'enabled', label: t('generic.state'), type: 'bool' },
    { key: 'sip_ip', label: t('sipProfile.sipIp'), type: 'text' },
    { key: 'sip_udp_port', label: t('sipProfile.udpPort'), type: 'text' },
    { key: 'dtmf_mode', label: t('sipProfile.dtmfMode'), type: 'text' },
  ],
  [
    { key: 'name', label: t('sipProfile.name'), type: 'text' },
    { key: 'enabled', label: t('generic.enabled'), type: 'bool', default: true },
    // ---- SIP ----
    { key: 'sip_ip', label: t('sipProfile.sipIp'), type: 'text', default: '0.0.0.0' },
    { key: 'external_sip_ip', label: t('sipProfile.externalIp'), type: 'text' },
    { key: 'sip_udp_port', label: t('sipProfile.udpPort'), type: 'number', default: 5060 },
    { key: 'sip_tcp_port', label: t('sipProfile.tcpPort'), type: 'number', default: 5060 },
    { key: 'sip_tls_port', label: t('sipProfile.tlsPort'), type: 'number', default: 5061 },
    { key: 'enable_udp', label: t('sipProfile.enableUdp'), type: 'bool', default: true },
    { key: 'enable_tcp', label: t('sipProfile.enableTcp'), type: 'bool', default: true },
    { key: 'enable_tls', label: t('sipProfile.enableTls'), type: 'bool', default: false },
    // ---- RTP ----
    { key: 'rtp_ip', label: t('sipProfile.rtpIp'), type: 'text', default: '0.0.0.0' },
    { key: 'external_rtp_ip', label: t('sipProfile.externalRtpIp'), type: 'text' },
    { key: 'rtp_port_start', label: t('sipProfile.rtpStart'), type: 'number', default: 10000 },
    { key: 'rtp_port_end', label: t('sipProfile.rtpEnd'), type: 'number', default: 59999 },
    // ---- Features ----
    { key: 'enable_prack', label: t('sipProfile.enablePrack'), type: 'bool', default: false },
    { key: 'enable_update', label: t('sipProfile.enableUpdate'), type: 'bool', default: true },
    { key: 'dtmf_mode', label: t('sipProfile.dtmfMode'), type: 'select', options: [
      { value: 'rfc4733', label: 'RFC 4733 telephone-event (recommended)' },
      { value: 'info',    label: 'SIP INFO application/dtmf-relay' },
      { value: 'inband',  label: 'Inband G.711' },
      { value: 'auto',    label: 'Auto (SDP prefers RFC4733)' },
    ], default: 'rfc4733' },
    { key: 'realm', label: t('tenant.matchFrom'), type: 'text', default: 'nsbc.local' },
    { key: 'description', label: t('generic.remarks'), type: 'textarea' },
  ],
));
