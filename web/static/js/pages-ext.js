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
// 外部 API 使用说明页 —— 所有数据从后端 GET /api/v1/ext-docs 动态拉取
// endpoints / scopes / cmds 都从 nsbc-web 实际注册的路由和 nsbcd core 取
// =============================================================================

app.component('ext-api-guide', {
  template: `
    <div class="p-4 max-w-5xl">
      <h2 class="text-xl font-bold mb-4">📡 外部 API 使用指南
        <el-tag size="small" class="ml-2" v-if="docs">v{{ docs.web_version }}</el-tag>
      </h2>

      <el-alert type="success" :closable="false" show-icon style="margin-bottom:20px">
        外部系统（呼叫中心 / CRM / AI Agent）接入 NSBC 只需要一个 API Key —— 不需要部署
        Python 客户端、不需要 UDS 访问、不需要 Web UI 登录。所有端点元数据由后端
        <code>GET /ext/v1/docs</code> 动态提供（本页数据来自 <code>GET /api/v1/ext-docs</code>）。
      </el-alert>

      <h3 class="font-bold mt-4 mb-2">🔐 认证方式</h3>
      <div class="bg-gray-100 p-3 rounded font-mono text-sm mb-4">
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
        <el-tag size="small" type="warning" class="ml-2">{{ docs?.cmd_count || 0 }} 条（动态从 core 取）</el-tag>
      </h3>
      <el-alert type="info" :closable="false" show-icon style="margin-bottom:12px">
        这些命令全部可以通过 <code>POST /ext/v1/cmd</code> 透明代理执行。
        命令名前缀 <code>[2]</code> 表示 L2 admin 级（二次授权），<code>[1]</code> 表示 L1 monitor 级。
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
        <li>在左侧菜单「外部 API → API Key 管理」创建一个 Key（选 scope，写标签）</li>
        <li>⚠️ 弹窗里只显示一次 Key 原文 —— 立即复制到密码管理器</li>
        <li>用这个 Key 作为 Bearer Token 调 /ext/v1/* 接口（<code>curl -k -X POST https://YOUR_HOST/ext/v1/docs -H "Authorization: Bearer &lt;KEY&gt;"</code> 先看完整端点列表）</li>
        <li>如果需要事件推送，在「外部 API → Webhook 回调管理」注册你的回调 URL</li>
      </ol>

      <h3 class="font-bold mt-4 mb-2">📥 示例（curl）</h3>
      <pre class="bg-gray-900 text-green-400 p-3 rounded text-xs overflow-x-auto">{{ curlExample }}</pre>
    </div>
  `,

  data() {
    return {
      loading: false,
      docs: null,
      cmdFilter: '',
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
      return `# 1) 先拉完整端点文档（拿到 API Key 后第一步）
curl -k https://YOUR_HOST/ext/v1/docs \\
  -H "Authorization: Bearer nsbc_live_xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"

# 2) 发外呼（结构化端点）
curl -k -X POST https://YOUR_HOST/ext/v1/originate \\
  -H "Authorization: Bearer nsbc_live_xxx" \\
  -H "Content-Type: application/json" \\
  -d '{"call_url": "user/1000 &echo"}'

# 3) ★ 透明代理 —— 任意 nsbc_cli 命令直送 core（最强大的能力）
curl -k -X POST https://YOUR_HOST/ext/v1/cmd \\
  -H "Authorization: Bearer nsbc_live_xxx" \\
  -H "Content-Type: application/json" \\
  -d '{"cmd": "help"}'        # 完整命令列表
  #-d '{"cmd": "show channels"}'   # 活动呼叫
  #-d '{"cmd": "status"}'          # 系统状态
  #-d '{"cmd": "originate user/1000 &echo"}'  # 透明代理外呼

# 4) 订阅实时事件流（会一直挂着，每个事件一行 JSON）
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
