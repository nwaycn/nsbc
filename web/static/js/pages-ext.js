// =============================================================================
// NSBC Web — External API 管理 (REQ-EXT-API-001)
// 加载顺序：app-core.js 之后
// 依赖：全局 app (Vue), API (app-core.js 定义)
// =============================================================================

// --- 弹窗：API Key 创建后只显示一次 raw_key ---
app.component('ext-key-created-dialog', {
  props: ['visible', 'data'],
  emits: ['close'],
  template: `
    <el-dialog v-model="localVisible" title="🔑 API Key 创建成功" width="560px"
      :close-on-click-modal="false">
      <el-alert type="warning" :closable="false" show-icon style="margin-bottom:12px">
        <b>⚠️ 这是 API Key 原文唯一一次显示！</b><br/>
        DB 只存 SHA-256 hash，你以后无法再查看这个值。<br/>
        请立即复制到安全的密码管理器。
      </el-alert>
      <div style="font-family:monospace;background:#f4f4f5;padding:12px;border-radius:6px;
                  word-break:break-all;border:1px solid #e4e4e7">
        {{ data?.raw_key }}
      </div>
      <div style="margin-top:16px;color:#71717a">
        ID: <b>{{ data?.id }}</b> &nbsp;|&nbsp;
        Scope: <b>{{ data?.scope }}</b> &nbsp;|&nbsp;
        Label: <b>{{ data?.label }}</b>
      </div>
      <template #footer>
        <el-button type="primary" @click="$emit('close')">我已保存，关闭</el-button>
      </template>
    </el-dialog>
  `,
  computed: {
    localVisible: {
      get() { return this.visible; },
      set(v) { this.$emit('close', v); }
    }
  }
});

// =============================================================================
// API Keys 管理页
// =============================================================================

app.component('ext-keys', {
  template: `
    <div class="p-4">
      <div class="flex items-center justify-between mb-4">
        <h2 class="text-xl font-bold">🔑 API Key 管理</h2>
        <div>
          <el-button @click="load" :loading="loading">🔄 刷新</el-button>
          <el-button type="primary" @click="showCreate">+ 创建 API Key</el-button>
        </div>
      </div>

      <el-table :data="rows" v-loading="loading" stripe style="width:100%">
        <el-table-column prop="id" label="ID" width="70"/>
        <el-table-column prop="label" label="标签 (Label)" min-width="160"/>
        <el-table-column prop="scope" label="Scope" width="110">
          <template #default="{row}">
            <el-tag :type="scopeColor(row.scope)" size="small">{{ row.scope }}</el-tag>
          </template>
        </el-table-column>
        <el-table-column label="状态" width="90">
          <template #default="{row}">
            <el-tag v-if="row.active" type="success" size="small">有效</el-tag>
            <el-tag v-else type="info" size="small">已吊销</el-tag>
          </template>
        </el-table-column>
        <el-table-column prop="created_at" label="创建时间" width="170"/>
        <el-table-column prop="last_used_at" label="最后使用" width="170">
          <template #default="{row}">{{ row.last_used_at || '—' }}</template>
        </el-table-column>
        <el-table-column label="操作" width="200">
          <template #default="{row}">
            <el-button v-if="row.active" size="small" type="warning"
              @click="revoke(row)">吊销</el-button>
            <el-button size="small" type="danger" @click="remove(row)">删除</el-button>
          </template>
        </el-table-column>
      </el-table>

      <!-- 创建表单 -->
      <el-dialog v-model="formVisible" title="创建新 API Key" width="440px">
        <el-form :model="form" label-width="80px">
          <el-form-item label="标签">
            <el-input v-model="form.label" placeholder="如: Acme Call Center CRM" maxlength="128"/>
          </el-form-item>
          <el-form-item label="Scope">
            <el-select v-model="form.scope" style="width:100%">
              <el-option value="readonly" label="🔍 readonly — 只允许 GET 监控接口"/>
              <el-option value="readwrite" label="📞 readwrite — 允许控制接口 (originate/hangup) + SSE 事件"/>
              <el-option value="admin" label="👑 admin — 全部权限（含 API Key 管理）"/>
            </el-select>
          </el-form-item>
        </el-form>
        <template #footer>
          <el-button @click="formVisible=false">取消</el-button>
          <el-button type="primary" @click="create">创建并显示 Key</el-button>
        </template>
      </el-dialog>

      <!-- 一次性显示 Key -->
      <ext-key-created-dialog :visible="createdVisible" :data="createdData"
        @close="createdVisible=false; createdData=null"/>
    </div>
  `,

  data() {
    return {
      loading: false,
      rows: [],
      formVisible: false,
      form: { label: '', scope: 'readwrite' },
      createdVisible: false,
      createdData: null,
    };
  },

  mounted() { this.load(); },

  methods: {
    scopeColor(s) {
      return { readonly: 'info', readwrite: 'primary', admin: 'danger' }[s] || '';
    },

    async load() {
      this.loading = true;
      try {
        const res = await API.get('/ext-keys');
        this.rows = res || [];
      } catch (e) {
        this.$message.error(e.message || '加载失败');
      } finally {
        this.loading = false;
      }
    },

    showCreate() {
      this.form = { label: '', scope: 'readwrite' };
      this.formVisible = true;
    },

    async create() {
      if (!this.form.label.trim()) {
        this.$message.warning('请输入标签'); return;
      }
      try {
        const res = await API.post('/ext-keys', this.form);
        this.formVisible = false;
        this.createdData = res;
        this.createdVisible = true;
        await this.load();
      } catch (e) {
        this.$message.error(e.message || '创建失败');
      }
    },

    async revoke(row) {
      try {
        await this.$confirm(`确定吊销 "${row.label}" (id=${row.id})？吊销后立即失效，不可撤销。`,
          '确认吊销', { type: 'warning' });
        await API.post(`/ext-keys/${row.id}/revoke`);
        this.$message.success('已吊销');
        await this.load();
      } catch (e) {
        if (e !== 'cancel') this.$message.error(e.message || '操作失败');
      }
    },

    async remove(row) {
      try {
        await this.$confirm(`彻底删除 "${row.label}" (id=${row.id})？无法恢复。`,
          '确认删除', { type: 'warning' });
        await API.delete(`/ext-keys/${row.id}`);
        this.$message.success('已删除');
        await this.load();
      } catch (e) {
        if (e !== 'cancel') this.$message.error(e.message || '操作失败');
      }
    }
  }
});

// =============================================================================
// Webhooks 管理页
// =============================================================================

app.component('ext-webhooks', {
  template: `
    <div class="p-4">
      <div class="flex items-center justify-between mb-4">
        <h2 class="text-xl font-bold">🔔 Webhook 回调管理</h2>
        <div>
          <el-button @click="load" :loading="loading">🔄 刷新</el-button>
          <el-button type="primary" @click="showCreate">+ 注册 Webhook</el-button>
        </div>
      </div>

      <el-alert v-if="!rows.length" type="info" :closable="false" style="margin-bottom:16px">
        Webhook Dispatcher 会在 nsbc-web 内部异步 POST 事件到你注册的 URL（P1 实现）。
        现在可以先注册 URL，dispatcher 就绪后自动生效。
      </el-alert>

      <el-table :data="rows" v-loading="loading" stripe style="width:100%">
        <el-table-column prop="id" label="ID" width="70"/>
        <el-table-column prop="url" label="回调 URL" min-width="280"/>
        <el-table-column prop="events_filter" label="事件过滤" min-width="200"/>
        <el-table-column label="状态" width="90">
          <template #default="{row}">
            <el-tag v-if="row.active" type="success" size="small">启用</el-tag>
            <el-tag v-else type="info" size="small">禁用</el-tag>
          </template>
        </el-table-column>
        <el-table-column prop="last_ok_at" label="最后成功" width="170">
          <template #default="{row}">{{ row.last_ok_at || '—' }}</template>
        </el-table-column>
        <el-table-column prop="fail_count" label="失败次数" width="100"/>
        <el-table-column label="操作" width="120">
          <template #default="{row}">
            <el-button size="small" type="danger" @click="remove(row)">删除</el-button>
          </template>
        </el-table-column>
      </el-table>

      <el-dialog v-model="formVisible" title="注册 Webhook" width="480px">
        <el-form :model="form" label-width="100px">
          <el-form-item label="回调 URL">
            <el-input v-model="form.url" placeholder="https://your-cc.example.com/api/nsbc-events"
              type="textarea" :rows="2"/>
            <div class="text-xs text-gray-500 mt-1">
              必须 http:// 或 https://，建议 HTTPS
            </div>
          </el-form-item>
          <el-form-item label="事件过滤">
            <el-input v-model="form.events_filter"
              placeholder="CALL_INCOMING,CALL_ANSWERED,CALL_END,CDR"/>
            <div class="text-xs text-gray-500 mt-1">
              逗号分隔。可选: CALL_INCOMING / CALL_ANSWERED / CALL_END / CDR / ALL
            </div>
          </el-form-item>
        </el-form>
        <template #footer>
          <el-button @click="formVisible=false">取消</el-button>
          <el-button type="primary" @click="create">注册</el-button>
        </template>
      </el-dialog>
    </div>
  `,

  data() {
    return {
      loading: false,
      rows: [],
      formVisible: false,
      form: { url: '', events_filter: 'CALL_INCOMING,CALL_ANSWERED,CALL_END' },
    };
  },

  mounted() { this.load(); },

  methods: {
    async load() {
      this.loading = true;
      try {
        const res = await API.get('/webhooks');
        this.rows = res || [];
      } catch (e) {
        this.$message.error(e.message || '加载失败');
      } finally { this.loading = false; }
    },

    showCreate() {
      this.form = { url: '', events_filter: 'CALL_INCOMING,CALL_ANSWERED,CALL_END' };
      this.formVisible = true;
    },

    async create() {
      if (!this.form.url.startsWith('http://') && !this.form.url.startsWith('https://')) {
        this.$message.warning('URL 必须以 http(s):// 开头'); return;
      }
      try {
        await API.post('/webhooks', this.form);
        this.formVisible = false;
        this.$message.success('已注册');
        await this.load();
      } catch (e) {
        this.$message.error(e.message || '注册失败');
      }
    },

    async remove(row) {
      try {
        await this.$confirm(`删除 Webhook "${row.url}"？`, '确认', { type: 'warning' });
        await API.delete(`/webhooks/${row.id}`);
        this.$message.success('已删除');
        await this.load();
      } catch (e) {
        if (e !== 'cancel') this.$message.error(e.message || '操作失败');
      }
    }
  }
});

// =============================================================================
// 外部 API 使用说明页 + 高可用配置指南
// —— 外部 API 数据从后端 GET /api/v1/ext-docs 动态拉取
// —— HA 配置内容来自 conf/ha配置.md（Active-Standby 双机主备模型）
// =============================================================================

app.component('ext-api-guide', {
  template: `
    <div class="p-4 max-w-6xl">
      <h2 class="text-xl font-bold mb-4">📡 外部 API & 高可用配置指南
        <el-tag size="small" class="ml-2" v-if="docs">v{{ docs.web_version }}</el-tag>
      </h2>

      <el-tabs v-model="activeTab" type="border-card">
        <!-- ==================== Tab 1: 外部 API ==================== -->
        <el-tab-pane label="🔐 外部 API 使用指南" name="api">
          <div class="bg-gray-50 border rounded-lg p-4 mb-4">
            <el-alert type="success" :closable="false" show-icon>
              外部系统（呼叫中心 / CRM / AI Agent）接入 NSBC 只需一个 API Key —— 不需要部署 Python 客户端、不需要 UDS 访问。
              所有端点元数据由后端 <code>GET /ext/v1/docs</code> 动态提供。
            </el-alert>
          </div>

          <h3 class="font-bold mt-4 mb-2">🔐 认证方式</h3>
          <div class="bg-gray-900 text-green-400 p-3 rounded font-mono text-sm mb-4">
            Authorization: Bearer nsbc_live_&lt;64hex&gt;
          </div>

          <h3 class="font-bold mt-4 mb-2">📋 Scope 分级</h3>
          <el-table :data="docs?.scopes || []" size="small" stripe style="margin-bottom:20px">
            <el-table-column prop="scope" label="Scope" width="120"/>
            <el-table-column prop="can" label="能做什么"/>
            <el-table-column prop="example" label="适用场景" width="260"/>
          </el-table>

          <h3 class="font-bold mt-4 mb-2">🔗 端点列表
            <el-tag size="small" type="info" class="ml-2">{{ (docs?.endpoints || []).length }} 个端点</el-tag>
          </h3>
          <el-table :data="docs?.endpoints || []" size="small" stripe v-loading="loading">
            <el-table-column prop="method" label="方法" width="80">
              <template #default="{row}">
                <el-tag :type="row.method==='GET'?'success':'warning'" size="small">{{ row.method }}</el-tag>
              </template>
            </el-table-column>
            <el-table-column prop="path" label="路径" width="220"/>
            <el-table-column prop="min_scope" label="最小 Scope" width="120">
              <template #default="{row}">
                <el-tag :type="row.min_scope==='readonly'?'info':row.min_scope==='readwrite'?'primary':'danger'" size="small">{{ row.min_scope }}</el-tag>
              </template>
            </el-table-column>
            <el-table-column prop="desc" label="说明"/>
          </el-table>

          <h3 class="font-bold mt-4 mb-2">🛠 nsbcd 命令注册表
            <el-tag size="small" type="warning" class="ml-2">{{ docs?.cmd_count || 0 }} 条</el-tag>
          </h3>
          <el-alert type="info" :closable="false" show-icon style="margin-bottom:12px">
            这些命令全部可以通过 <code>POST /ext/v1/cmd</code> 透明代理执行。
            <span class="ml-2">命令名前缀 <code>[2]</code> 表示 L2 admin 级，<code>[1]</code> 表示 L1 monitor 级。</span>
          </el-alert>
          <el-input v-model="cmdFilter" placeholder="🔍 搜索命令名或帮助文本..." clearable style="margin-bottom:10px"/>
          <el-table :data="filteredCmds" size="small" stripe height="360">
            <el-table-column label="级别" width="70">
              <template #default="{row}">
                <el-tag :type="row.level===2?'danger':'info'" size="small">L{{ row.level }}</el-tag>
              </template>
            </el-table-column>
            <el-table-column prop="name" label="命令名" width="180">
              <template #default="{row}"><code>{{ row.name }}</code></template>
            </el-table-column>
            <el-table-column prop="help" label="帮助"/>
          </el-table>

          <h3 class="font-bold mt-4 mb-2">💡 接入流程</h3>
          <ol class="list-decimal pl-5 space-y-1">
            <li>在 Web UI「外部 API → API Key 管理」创建一个 Key（选 scope，写标签）</li>
            <li>⚠️ 弹窗里只显示一次 Key 原文 —— 立即复制到密码管理器</li>
            <li>用这个 Key 作为 Bearer Token 调 <code>/ext/v1/*</code> 接口</li>
            <li>需要事件推送？在「外部 API → Webhook 回调管理」注册回调 URL</li>
          </ol>

          <h3 class="font-bold mt-4 mb-2">📥 curl 示例</h3>
          <pre class="bg-gray-900 text-green-400 p-3 rounded text-xs overflow-x-auto">{{ curlExample }}</pre>
        </el-tab-pane>

        <!-- ==================== Tab 2: 高可用配置 ==================== -->
        <el-tab-pane label="🏗 高可用 HA 配置指南" name="ha">
          <div class="bg-gray-50 border rounded-lg p-4 mb-4">
            <el-alert type="warning" :closable="false" show-icon>
              <b>双机主备（Active-Standby）模式</b> —— VIP 漂移由外部 keepalived / HAProxy 执行，本进程只出角色状态 + 信令门控。
              完整细节见项目文档 <code>conf/ha配置.md</code>。
            </el-alert>
          </div>

          <!-- 架构图 -->
          <h3 class="font-bold mt-4 mb-2">📐 架构图</h3>
          <div class="bg-gray-900 text-gray-200 p-4 rounded font-mono text-xs leading-relaxed overflow-x-auto whitespace-pre">
{{ archDiagram }}
          </div>

          <!-- 快速启用三步法 -->
          <h3 class="font-bold mt-6 mb-2">⚡ 快速启用三步法</h3>

          <div class="bg-white border rounded-lg p-4 mb-3">
            <div class="font-bold text-blue-600 mb-2">Step 1 — nsbc.yaml（两台各改 4–5 行）</div>
            <div class="text-xs text-gray-500 mb-2">最简原则：<code>heartbeat_ms</code> / <code>failover_ms</code> 不用写 — 默认值够用。</div>
            <pre class="bg-gray-900 text-green-400 p-3 rounded text-xs overflow-x-auto">
# node-a (主节点 10.0.0.80)
ha:
  enabled: true
  peer: "10.0.0.81:5070"
  priority: 100          # 主高备低，避免 hostname 不确定性

# node-b (备节点 10.0.0.81)
ha:
  enabled: true
  peer: "10.0.0.80:5070"  # 互指对方
  priority: 90
            </pre>
          </div>

          <div class="bg-white border rounded-lg p-4 mb-3">
            <div class="font-bold text-blue-600 mb-2">Step 2 — 防火墙开端口（两台都要）</div>
            <pre class="bg-gray-900 text-green-400 p-3 rounded text-xs overflow-x-auto">
firewall-cmd --permanent --add-port=5060/udp   # SIP
firewall-cmd --permanent --add-port=5070/udp   # HA 心跳
firewall-cmd --permanent --add-port=5071/tcp   # 状态复制（replica.enabled=true 时才需要）
firewall-cmd --reload
            </pre>
          </div>

          <div class="bg-white border rounded-lg p-4 mb-3">
            <div class="font-bold text-blue-600 mb-2">Step 3 — keepalived VIP 漂移（可选但强烈推荐）</div>
            <pre class="bg-gray-900 text-green-400 p-3 rounded text-xs overflow-x-auto">
# node-a (MASTER) / node-b (BACKUP) — 唯一区别是 state + priority
global_defs { router_id NSBC_HA }
vrrp_instance VI_1 {
    state MASTER          # node-b 写 BACKUP
    interface eth0
    virtual_router_id 51
    priority 150          # node-b 写 140
    advert_int 1
    authentication { auth_type PASS auth_pass nsbc_ha }
    virtual_ipaddress { 139.226.66.211/24 }
    track_process { nsbcd }   # nsbcd 挂了自动摘 VIP
}
            </pre>
          </div>

          <!-- P2-9 discover 模式 -->
          <div class="bg-amber-50 border border-amber-200 rounded-lg p-4 mb-3">
            <div class="font-bold text-amber-700 mb-2">💫 P2-9 零配置 discover 模式</div>
            <div class="text-sm text-gray-700">
              两台机器在<b>同一二层子网</b>上时，<b>完全不写 peer 行</b>，HA 模块会通过 UDP broadcast
              <code>255.255.255.255:5070</code> 自动互相发现。启动间隔 ≤ 3 秒即可。
            </div>
            <div class="text-xs text-gray-500 mt-2">前置条件：防火墙允许 broadcast 包穿过 5070/UDP。</div>
          </div>

          <!-- 仲裁规则 -->
          <h3 class="font-bold mt-6 mb-2">⚖️ 仲裁规则</h3>
          <el-table :data="arbRules" size="small" stripe>
            <el-table-column prop="scene" label="场景" width="200"/>
            <el-table-column prop="cond" label="判定条件"/>
            <el-table-column prop="result" label="结果" width="180"/>
          </el-table>

          <!-- 信令门控 -->
          <h3 class="font-bold mt-6 mb-2">🚦 信令门控一览表</h3>
          <el-table :data="signalGate" size="small" stripe>
            <el-table-column prop="path" label="路径" width="160"/>
            <el-table-column label="ACTIVE" width="100">
              <template #default="{row}"><el-tag type="success" size="small">✅ {{ row.active }}</el-tag></template>
            </el-table-column>
            <el-table-column label="STANDBY" width="140">
              <template #default="{row}"><el-tag :type="row.standbyOk?'success':'danger'" size="small">{{ row.standbyOk?'✅ ✅ 但跳过 reg_push':'❌ '+row.standby }}</el-tag></template>
            </el-table-column>
          </el-table>

          <!-- 配置项详解 -->
          <h3 class="font-bold mt-6 mb-2">⚙️ 配置项详解（全部在 nsbc.yaml 的 ha: 段）</h3>
          <pre class="bg-gray-900 text-gray-200 p-3 rounded text-xs overflow-x-auto">{{ haConfigDump }}</pre>

          <!-- 配置变更生效方式 -->
          <h3 class="font-bold mt-6 mb-2">🔄 配置变更生效方式</h3>
          <el-table :data="configEffect" size="small" stripe max-height="360">
            <el-table-column prop="key" label="配置项" width="260"/>
            <el-table-column prop="effect" label="生效方式" width="120">
              <template #default="{row}">
                <el-tag :type="row.effect.includes('重启')?'warning':'success'" size="small">{{ row.effect }}</el-tag>
              </template>
            </el-table-column>
            <el-table-column prop="note" label="备注"/>
          </el-table>

          <!-- 故障降级 -->
          <h3 class="font-bold mt-6 mb-2">🛟 故障降级策略</h3>
          <div class="bg-red-50 border border-red-200 rounded-lg p-4">
            <div class="text-sm text-gray-700 mb-2">HA 模块自身故障<b>绝不拖死整机</b> —— 所有故障点都降级为 <code>solo-active</code>（本机无条件 ACTIVE）：</div>
            <ul class="text-sm text-gray-600 space-y-1 pl-5 list-disc">
              <li>socket / bind / pthread_create 失败 → WARN 日志 + 跳过 HA 初始化</li>
              <li>ha.peer DNS 解析失败 → 单机 ACTIVE</li>
              <li>健康门创建失败 → 静默关闭健康门，心跳仲裁照常</li>
            </ul>
          </div>

          <!-- OAM CLI -->
          <h3 class="font-bold mt-6 mb-2">🛠 OAM CLI 命令</h3>
          <el-table :data="haCommands" size="small" stripe>
            <el-table-column prop="cmd" label="命令" width="200">
              <template #default="{row}"><code>{{ row.cmd }}</code></template>
            </el-table-column>
            <el-table-column prop="perm" label="权限" width="80"/>
            <el-table-column prop="desc" label="说明"/>
          </el-table>

          <!-- 验证清单 -->
          <h3 class="font-bold mt-6 mb-2">✅ 部署验证清单（30 秒）</h3>
          <ol class="list-decimal pl-5 space-y-1 text-sm">
            <li><b>心跳互通</b>：<code>grep 'heartbeat enabled' /var/log/nsbcd.log</code> — 两台都应有 peer=对端</li>
            <li><b>角色收敛</b>：<code>grep 'role' /var/log/nsbcd.log | tail -5</code> — 主节点 STANDBY→ACTIVE，备节点保持 STANDBY</li>
            <li><b>注册热备</b>：UA 在 VIP 上 REGISTER 后，两台 grep REGISTER-200 都能看到</li>
            <li><b>Failover 测试</b>：kill 主节点 nsbcd → 3 秒后备节点升 ACTIVE → keepalived VIP 漂移</li>
            <li><b>恢复测试</b>：主节点重启 → priority 高 → 自动切回 ACTIVE</li>
          </ol>
        </el-tab-pane>
      </el-tabs>
    </div>
  `,

  data() {
    return {
      loading: false,
      docs: null,
      cmdFilter: '',
      activeTab: 'api',

      // ---- HA 配置指南数据 ----
      archDiagram: `

  ┌─────────────────────────────────────────────────────────┐
  │                    外部 keepalived                       │
  │         VIP 139.226.66.211 漂移 → ACTIVE 节点            │
  └──────────────┬───────────────────────────┬─────────────┘
                 │  SIP UDP 5060             │
     ┌───────────▼──────────┐        ┌────────▼─────────────┐
     │   nsbcd (ACTIVE)     │  UDP   │   nsbcd (STANDBY)    │
     │                      │ 5070   │                      │
     │  • INVITE ✅ 接呼     │◄────► │  • INVITE ❌ 丢弃     │
     │  • REGISTER ✅ 受理   │心跳    │  • REGISTER ✅ 受理   │
     │  • 外呼 ✅ 允许        │       │  • 外呼 ❌ 拒绝       │
     │  • reg_push ✅ DB写入 │       │  • reg_push ❌ 跳过   │
     │  • replica TCP 推送   │──────► │  • loc_db ✅ 热备     │
     └──────────────────────┘ 5071   └───────────────────────┘
      `.trim(),

      arbRules: [
        { scene: '单机 (ha.enabled=false)', cond: '无对端配置', result: 'ACTIVE（solo-active）' },
        { scene: '对端不可见', cond: '启动后等满 failover_ms 未收到心跳', result: 'ACTIVE（failover 接管）' },
        { scene: '正常主备', cond: '对端活着', result: 'priority 大的 ACTIVE' },
        { scene: 'Priority 相等', cond: '对端活着 + priority 相同', result: 'hostname 字典序 tie-break' },
        { scene: '健康门让位', cond: '本机 UNHEALTHY 连续 3 周期', result: '主动降 STANDBY' },
      ],

      signalGate: [
        { path: 'REGISTER 受理', active: '✅', standbyOk: true, standby: '✅ loc_db 写内存' },
        { path: 'reg_push (DB/事件)', active: '✅', standbyOk: false, standby: '❌ 跳过防双写' },
        { path: 'INVITE 新呼', active: '✅', standbyOk: false, standby: '❌ 503 兜底（SOS 例外）' },
        { path: 'INVITE in-dialog', active: '✅', standbyOk: true, standby: '✅ 接 BYE/re-INVITE' },
        { path: '外呼 originate', active: '✅', standbyOk: false, standby: '❌ 拒绝' },
      ],

      haConfigDump: `ha:
  enabled: false                 # 默认 false = 单机模式
  peer: "10.0.0.81:5070"         # 对端心跳地址；空则启用 broadcast discover
  heartbeat_ms: 500              # 心跳间隔，实际仲裁周期 = heartbeat_ms / 4 = 125ms
  failover_ms: 3000              # failover 窗口，对端连续不可见超此时长才升主
  priority: 100                  # 仲裁优先级，大者 ACTIVE；相同则 hostname tie-break

  # === 状态复制（replica.enabled=true 需同时 ha.enabled=true + peer 有效）===
  replica:
    enabled: false               # 主→备注册/对话 TCP 推送总开关
    port: 5071                   # 复制 TCP 监听端口
    ring_cap: 8192               # 环形缓冲容量（条）
    lag_warn: 10000              # 备端 applied_seq 落后 pub_seq 超过此值 → DEGRADED

  # === BFD 亚秒探测（RFC 5881）===
  bfd:
    enabled: false               # 启用后建议同步调小 failover_ms
    local_discriminator: 1       # 双方不同
    desired_min_tx_interval: 200 # BFD 控制面发包间隔
    detect_multiplier: 3         # interval × 3 = failover 时间

  # === Hitless Upgrade 滚动升级 ===
  hitless:
    enabled: false               # FREEZE→DRAIN→FULL push→yield 无感切换
    freeze_ms: 1000              # 老进程 stop 前冻结输入等待新进程 ready
    upgrade_timeout_ms: 10000    # 新进程超时未上线则放弃滚动
      `.trim(),

      configEffect: [
        { key: 'ha.enabled', effect: '重启', note: 'CGF_RESTART' },
        { key: 'ha.peer', effect: '重启', note: '解析失败降级 solo-active' },
        { key: 'ha.heartbeat_ms', effect: '热加载', note: '范围 [50, 10000]，默认 500' },
        { key: 'ha.failover_ms', effect: '热加载', note: '范围 [100, 60000]，默认 3000' },
        { key: 'ha.priority', effect: '热加载', note: '范围 [1, 65535]，默认 100' },
        { key: 'ha.replica.enabled', effect: '重启', note: 'transport TCP connect/listen 需重建' },
        { key: 'ha.replica.port', effect: '重启', note: '默认 5071' },
        { key: 'ha.replica.ring_cap', effect: '重启', note: '环形缓冲静态分配' },
        { key: 'ha.replica.lag_warn', effect: '热加载', note: '默认 10000' },
        { key: 'ha.bfd.enabled', effect: '重启', note: 'BFD Control socket 需创建' },
        { key: 'ha.bfd.desired_min_tx_interval', effect: '热加载', note: '默认 200' },
        { key: 'ha.bfd.detect_multiplier', effect: '热加载', note: '默认 3' },
        { key: 'ha.hitless.enabled', effect: '重启', note: '' },
      ],

      haCommands: [
        { cmd: 'ha status', perm: 'L1', desc: '综合状态一屏看：role/peer/hb/failover/priority/健康门/replica/BFD/Hitless' },
        { cmd: 'ha status-frozen', perm: 'L1', desc: 'Hitless Upgrade FREEZE 状态 (yes/no)' },
        { cmd: 'ha hitless_upgrade [force]', perm: 'L2', desc: '触发滚动升级（ACTIVE→STANDBY）；force 绕过配置守卫' },
      ],
    };
  },

  computed: {
    filteredCmds() {
      const all = this.docs?.cmds || [];
      if (!this.cmdFilter) return all;
      const q = this.cmdFilter.toLowerCase();
      return all.filter(c =>
        c.name.toLowerCase().includes(q) ||
        c.help.toLowerCase().includes(q)
      );
    },
    curlExample() {
      return `# 1) 先拉完整端点文档
curl -k https://YOUR_HOST/ext/v1/docs \\
  -H "Authorization: Bearer nsbc_live_xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"

# 2) 发外呼（结构化端点）
curl -k -X POST https://YOUR_HOST/ext/v1/originate \\
  -H "Authorization: Bearer nsbc_live_xxx" \\
  -H "Content-Type: application/json" \\
  -d '{"call_url": "user/1000 &echo"}'

# 3) ★ 透明代理 —— 任意 nsbc_cli 命令直送 core
curl -k -X POST https://YOUR_HOST/ext/v1/cmd \\
  -H "Authorization: Bearer nsbc_live_xxx" \\
  -H "Content-Type: application/json" \\
  -d '{"cmd": "ha status"}'        # HA 综合状态
  #-d '{"cmd": "show channels"}'   # 活动呼叫
  #-d '{"cmd": "help"}'            # 完整命令列表

# 4) 订阅实时事件流（SSE，会一直挂着）
curl -k -N https://YOUR_HOST/ext/v1/events/stream \\
  -H "Authorization: Bearer nsbc_live_xxx"`;
    }
  },

  mounted() { this.load(); },

  methods: {
    async load() {
      this.loading = true;
      try {
        this.docs = await API.get('/ext-docs');
      } catch (e) {
        this.$message.error('加载文档失败: ' + (e.message || e));
      } finally {
        this.loading = false;
      }
    }
  }
});

// 注册（等 app 已初始化后由 app-core.js 统一 load）
window._extComponentsLoaded = true;
