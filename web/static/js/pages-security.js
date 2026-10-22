// ===== 安全中心（/api/v1/security/*） =====
// Tab 1 动态封禁：内置 fail2ban（REGISTER 认证失败 N 次/窗口 → 自动封 IP）
//   的查看/搜索/解禁入口。误封真实客户时在此解禁。
// Tab 2 围栏规则：NIP 三层 + deny/allow 双模式
//   rule_type: ip_cidr / ua / called_prefix / caller_prefix
//   kind: deny（命中即丢）/ allow（命中才放；allow 配了未命中一律丢）
//   保存后自动推 nsbcd（security.blacklist.reload 整表替换）。
// Tab 3 全局配置：fence_mode（security/performance）+ method_blocked（位图禁用）。
app.component('security-center', {
  template: `
    <div>
      <div class="flex items-center justify-between mb-4">
        <h2 class="text-lg font-semibold">
          <i class="fa-solid fa-shield-halved"></i> 安全中心
        </h2>
      </div>

      <el-tabs v-model="tab">

        <!-- ================ Tab 1: 动态封禁 ================ -->
        <el-tab-pane label="动态封禁" name="bans">
          <div class="flex items-center justify-between mb-3">
            <el-input v-model="query" placeholder="搜索 IP（子串，如 66.94）"
              size="small" style="width:240px" clearable
              @keyup.enter="reload" @clear="reload"></el-input>
            <div class="flex items-center gap-2">
              <el-button size="small" type="primary" @click="reload">
                <i class="fa-solid fa-magnifying-glass"></i> 搜索
              </el-button>
              <el-button size="small" @click="load">
                <i class="fa-solid fa-rotate"></i> 刷新
              </el-button>
            </div>
          </div>

          <el-alert type="info" :closable="false" class="mb-3"
            title="同一 IP 在计数窗口内 REGISTER 认证失败达到阈值后自动封禁；封禁期间其 REGISTER/INVITE 均静默丢弃。观察中 = 计数未达阈值；封禁剩余归零自动解除。误封处理：点击解禁立即清除该 IP 全部状态。重启 nsbcd 也会清空整表。" />

          <div v-if="loading" class="text-gray-400 py-8 text-center">加载中...</div>
          <div v-else-if="error" class="text-red-500 py-8 text-center">⚠ {{ error }}</div>
          <div v-else-if="!rows.length" class="text-gray-400 py-8 text-center">
            <i class="fa-solid fa-circle-check text-2xl mb-2 block text-green-500"></i>
            当前没有封禁或观察中的 IP
          </div>
          <el-table v-else :data="rows" size="small" border>
            <el-table-column label="源 IP" min-width="150">
              <template #default="{ row }">
                <span class="font-mono">{{ row.ip }}</span>
              </template>
            </el-table-column>
            <el-table-column label="状态" width="100">
              <template #default="{ row }">
                <el-tag v-if="row.state === 'ban'" type="danger" size="small">
                  封禁中
                </el-tag>
                <el-tag v-else type="warning" size="small">观察中</el-tag>
              </template>
            </el-table-column>
            <el-table-column label="失败次数" width="90" align="center">
              <template #default="{ row }">{{ row.fails }}</template>
            </el-table-column>
            <el-table-column label="封禁剩余" width="110" align="center">
              <template #default="{ row }">
                <span v-if="row.state === 'ban'" class="text-red-600 font-mono">
                  {{ fmtDur(row.remaining_s) }}
                </span>
                <span v-else>-</span>
              </template>
            </el-table-column>
            <el-table-column label="最近活动" width="110" align="center">
              <template #default="{ row }">{{ row.act_ago_s }}s 前</template>
            </el-table-column>
            <el-table-column label="操作" width="90" align="center">
              <template #default="{ row }">
                <el-popconfirm :title="'解禁 ' + row.ip + '？（清除全部计数与封禁状态）'"
                  confirm-button-text="解禁" cancel-button-text="取消"
                  @confirm="unban(row.ip)">
                  <template #reference>
                    <el-button size="small" type="danger" plain>解禁</el-button>
                  </template>
                </el-popconfirm>
              </template>
            </el-table-column>
          </el-table>
        </el-tab-pane>

        <!-- ================ Tab 2: 围栏规则 ================ -->
        <el-tab-pane label="围栏规则" name="fence">
          <div class="flex items-center justify-between mb-3">
            <div class="flex items-center gap-3">
              <el-radio-group v-model="blKind" size="small" @change="loadBl">
                <el-radio-button label="">全部</el-radio-button>
                <el-radio-button label="deny">黑名单（deny）</el-radio-button>
                <el-radio-button label="allow">白名单（allow）</el-radio-button>
              </el-radio-group>
              <el-radio-group v-model="blType" size="small" @change="loadBl">
                <el-radio-button label="">全部</el-radio-button>
                <el-radio-button label="ip_cidr">IP CIDR</el-radio-button>
                <el-radio-button label="ua">UA</el-radio-button>
                <el-radio-button label="called_prefix">被叫前缀</el-radio-button>
                <el-radio-button label="caller_prefix">主叫前缀</el-radio-button>
              </el-radio-group>
            </div>
            <div class="flex items-center gap-2">
              <el-button size="small" @click="loadBl">
                <i class="fa-solid fa-rotate"></i> 刷新
              </el-button>
              <el-button size="small" type="primary" @click="openAdd">
                <i class="fa-solid fa-plus"></i> 新增规则
              </el-button>
            </div>
          </div>

          <el-alert type="warning" :closable="false" class="mb-3"
            title="NIP 三层围栏 — deny 规则命中即静默丢弃（不回任何 SIP 响应）；allow 规则配了后仅命中才放行。优先级：method 禁用 → deny allow → allowlist 未命中 → deny 命中 → 放行。保存后自动推 nsbcd 秒级生效。" />

          <div v-if="blLoading" class="text-gray-400 py-8 text-center">加载中...</div>
          <div v-else-if="blError" class="text-red-500 py-8 text-center">⚠ {{ blError }}</div>
          <div v-else-if="!blRows.length" class="text-gray-400 py-8 text-center">
            <i class="fa-solid fa-circle-check text-2xl mb-2 block text-green-500"></i>
            当前没有围栏规则
          </div>
          <el-table v-else :data="blRows" size="small" border>
            <el-table-column label="模式" width="100">
              <template #default="{ row }">
                <el-tag v-if="row.kind === 'allow'" type="success" size="small">ALLOW</el-tag>
                <el-tag v-else type="danger" size="small">DENY</el-tag>
              </template>
            </el-table-column>
            <el-table-column label="类型" width="110">
              <template #default="{ row }">
                <el-tag v-if="row.rule_type === 'ip_cidr'" type="info" size="small">IP CIDR</el-tag>
                <el-tag v-else-if="row.rule_type === 'ua'" type="warning" size="small">UA</el-tag>
                <el-tag v-else-if="row.rule_type === 'called_prefix'" type="" size="small">被叫前缀</el-tag>
                <el-tag v-else type="" size="small">主叫前缀</el-tag>
              </template>
            </el-table-column>
            <el-table-column label="规则值" min-width="200">
              <template #default="{ row }">
                <span class="font-mono">{{ row.value }}</span>
              </template>
            </el-table-column>
            <el-table-column label="备注" min-width="120" show-overflow-tooltip>
              <template #default="{ row }">{{ row.comment || '-' }}</template>
            </el-table-column>
            <el-table-column label="启用" width="80" align="center">
              <template #default="{ row }">
                <el-switch :model-value="row.enabled === 1" size="small"
                  @change="v => toggleBl(row, v)" />
              </template>
            </el-table-column>
            <el-table-column label="添加时间" width="170">
              <template #default="{ row }">{{ row.created_at }}</template>
            </el-table-column>
            <el-table-column label="操作" width="90" align="center">
              <template #default="{ row }">
                <el-popconfirm :title="'删除规则 ' + row.value + '？'"
                  confirm-button-text="删除" cancel-button-text="取消"
                  @confirm="removeBl(row.id)">
                  <template #reference>
                    <el-button size="small" type="danger" plain>删除</el-button>
                  </template>
                </el-popconfirm>
              </template>
            </el-table-column>
          </el-table>

          <!-- 新增规则 dialog -->
          <el-dialog v-model="dlgVisible" title="新增围栏规则" width="480px">
            <el-form label-width="90px">
              <el-form-item label="模式">
                <el-radio-group v-model="form.kind">
                  <el-radio label="deny">黑名单（命中即丢）</el-radio>
                  <el-radio label="allow">白名单（命中才放）</el-radio>
                </el-radio-group>
              </el-form-item>
              <el-form-item label="类型">
                <el-select v-model="form.type" style="width:200px">
                  <el-option label="IP CIDR" value="ip_cidr"></el-option>
                  <el-option label="UA 子串" value="ua"></el-option>
                  <el-option label="被叫号码前缀" value="called_prefix"></el-option>
                  <el-option label="主叫号码前缀" value="caller_prefix"></el-option>
                </el-select>
              </el-form-item>
              <el-form-item :label="valueLabel">
                <el-input v-model="form.value" :placeholder="valuePlaceholder"></el-input>
              </el-form-item>
              <el-form-item label="备注">
                <el-input v-model="form.comment" maxlength="64"
                  placeholder="可选" />
              </el-form-item>
              <el-form-item label="启用">
                <el-switch v-model="form.enabled" />
              </el-form-item>
            </el-form>
            <template #footer>
              <el-button @click="dlgVisible = false">取消</el-button>
              <el-button type="primary" :loading="saving" @click="saveBl">保存</el-button>
            </template>
          </el-dialog>
        </el-tab-pane>

        <!-- ================ Tab 3: 全局配置 ================ -->
        <el-tab-pane label="全局配置" name="config">
          <div v-if="cfgLoading" class="text-gray-400 py-8 text-center">加载中...</div>
          <div v-else-if="cfgError" class="text-red-500 py-8 text-center">⚠ {{ cfgError }}</div>
          <div v-else class="space-y-5">

            <!-- fence_mode -->
            <el-card shadow="never" class="border">
              <template #header>
                <div class="flex items-center gap-2 font-semibold">
                  <i class="fa-solid fa-layer-group"></i> 围栏模式
                </div>
              </template>
              <div class="flex items-center justify-between">
                <div>
                  <div class="text-sm font-medium">fence_mode</div>
                  <div class="text-xs text-gray-500 mt-1">
                    performance（0）：全关软件围栏，依赖硬件处理；
                    security（1）：完整围栏检查（推荐默认）
                  </div>
                </div>
                <el-switch
                  :model-value="cfg.fence_mode === 1"
                  active-text="security" inactive-text="performance"
                  inline-prompt
                  @change="v => cfg.fence_mode = v ? 1 : 0"
                />
              </div>
            </el-card>

            <!-- method_blocked -->
            <el-card shadow="never" class="border">
              <template #header>
                <div class="flex items-center gap-2 font-semibold">
                  <i class="fa-solid fa-ban"></i> 禁用 SIP Method
                </div>
              </template>
              <div class="text-xs text-gray-500 mb-3">
                命中被禁用 method 的请求一律静默丢弃。位图 (1<<(idx-1))，0=允许。
              </div>
              <div class="grid grid-cols-4 gap-3">
                <el-checkbox v-for="m in methodList" :key="m.name"
                  :model-value="isMethodSet(cfg.method_blocked, m.bit)"
                  @change="v => toggleMethod(m.bit, v)"
                >{{ m.name }}
                  <span class="text-gray-400 text-xs ml-1">0x{{ m.bit.toString(16) }}</span>
                </el-checkbox>
              </div>
              <div class="mt-3 text-sm">
                当前位图：<span class="font-mono text-red-500">0x{{ cfg.method_blocked.toString(16) }}</span>
              </div>
            </el-card>

            <div class="flex justify-end gap-2">
              <el-button size="small" @click="loadCfg">重新加载</el-button>
              <el-button size="small" type="primary" :loading="cfgSaving" @click="saveCfg">
                <i class="fa-solid fa-floppy-disk"></i> 保存并推送核心
              </el-button>
            </div>
          </div>
        </el-tab-pane>

      </el-tabs>
    </div>
  `,

  setup() {
    const tab = ref('bans');

    // ============ Tab 1: 动态封禁 ============
    const rows = ref([]), loading = ref(false), error = ref(null);
    const query = ref('');
    let timer = null;

    function fmtDur(s) {
      if (s >= 3600) return Math.floor(s / 3600) + 'h' + Math.floor((s % 3600) / 60) + 'm';
      if (s >= 60) return Math.floor(s / 60) + 'm' + (s % 60) + 's';
      return s + 's';
    }

    async function load() {
      loading.value = true; error.value = null;
      try {
        const q = (query.value || '').trim();
        const data = await API.get('/security/bans' + (q ? '?query=' + encodeURIComponent(q) : ''));
        rows.value = data.data || [];
      } catch (e) {
        error.value = e.message;
      }
      loading.value = false;
    }
    function reload() { load(); }

    async function unban(ip) {
      try {
        await API.delete('/security/bans/' + encodeURIComponent(ip));
        ElementPlus.ElMessage.success('已解禁 ' + ip);
        load();
      } catch (e) {
        ElementPlus.ElMessage.error('解禁失败: ' + e.message);
      }
    }

    // ============ Tab 2: 围栏规则 ============
    const blRows = ref([]), blLoading = ref(false), blError = ref(null);
    const blKind = ref(''), blType = ref('');
    const dlgVisible = ref(false), saving = ref(false);
    const form = reactive({ kind: 'deny', type: 'ip_cidr', value: '', comment: '', enabled: true });

    const valueLabel = computed(() => {
      switch (form.type) {
        case 'ip_cidr': return 'IP / CIDR';
        case 'ua': return 'UA 子串';
        case 'called_prefix': return '被叫号码前缀';
        case 'caller_prefix': return '主叫号码前缀';
        default: return '值';
      }
    });
    const valuePlaceholder = computed(() => {
      switch (form.type) {
        case 'ip_cidr': return '如 66.94.0.0/16 或 1.2.3.4（单 IP 自动补 /32）';
        case 'ua': return '如 friendly-scanner（大小写不敏感子串）';
        case 'called_prefix': return '如 100*（被叫前缀通配）或 100（精确）';
        case 'caller_prefix': return '如 900*（主叫前缀通配）';
        default: return '值';
      }
    });

    async function loadBl() {
      blLoading.value = true; blError.value = null;
      try {
        const params = new URLSearchParams();
        if (blKind.value) params.set('kind', blKind.value);
        if (blType.value) params.set('type', blType.value);
        const p = params.toString();
        const data = await API.get('/security/blacklist' + (p ? '?' + p : ''));
        blRows.value = data.items || [];
      } catch (e) {
        blError.value = e.message;
      }
      blLoading.value = false;
    }

    function openAdd() {
      form.kind = blKind.value || 'deny';
      form.type = blType.value || 'ip_cidr';
      form.value = ''; form.comment = ''; form.enabled = true;
      dlgVisible.value = true;
    }

    async function saveBl() {
      const v = (form.value || '').trim();
      if (!v) { ElementPlus.ElMessage.warning('请填写规则值'); return; }
      saving.value = true;
      try {
        await API.post('/security/blacklist', {
          kind: form.kind, type: form.type, value: v,
          comment: (form.comment || '').trim(), enabled: form.enabled,
        });
        ElementPlus.ElMessage.success('已保存并推送到核心');
        dlgVisible.value = false;
        loadBl();
      } catch (e) {
        ElementPlus.ElMessage.error('保存失败: ' + e.message);
      }
      saving.value = false;
    }

    async function toggleBl(row, v) {
      try {
        await API.put('/security/blacklist/' + row.id, { enabled: v });
        row.enabled = v ? 1 : 0;
        ElementPlus.ElMessage.success(v ? '已启用并推送' : '已禁用并推送');
      } catch (e) { ElementPlus.ElMessage.error('操作失败: ' + e.message); }
    }

    async function removeBl(id) {
      try {
        await API.delete('/security/blacklist/' + id);
        ElementPlus.ElMessage.success('已删除并推送');
        loadBl();
      } catch (e) { ElementPlus.ElMessage.error('删除失败: ' + e.message); }
    }

    // ============ Tab 3: 全局配置 ============
    const cfg = reactive({ fence_mode: 1, method_blocked: 0 });
    const cfgLoading = ref(false), cfgError = ref(null), cfgSaving = ref(false);

    // method_blocked 位图定义（与 C 侧 sip_method_t 对齐）
    const methodList = [
      { name: 'INVITE',   bit: 0x001 },  // SIP_M_INVITE = 1 → (1<<0)
      { name: 'ACK',      bit: 0x002 },  // 2 → (1<<1)
      { name: 'BYE',      bit: 0x004 },  // 3 → (1<<2)
      { name: 'CANCEL',   bit: 0x008 },  // 4
      { name: 'REGISTER', bit: 0x010 },  // 5
      { name: 'OPTIONS',  bit: 0x020 },  // 6
      { name: 'INFO',     bit: 0x040 },  // 7
      { name: 'SUBSCRIBE',bit: 0x080 },  // 8
      { name: 'NOTIFY',   bit: 0x100 },  // 9
      { name: 'PUBLISH',  bit: 0x200 },  // 10
      { name: 'REFER',    bit: 0x400 },  // 11
      { name: 'MESSAGE',  bit: 0x800 },  // 12
      { name: 'UPDATE',   bit: 0x1000 }, // 13
    ];

    function isMethodSet(blocked, bit) { return (blocked & bit) !== 0; }
    function toggleMethod(bit, on) {
      if (on) cfg.method_blocked = cfg.method_blocked | bit;
      else    cfg.method_blocked = cfg.method_blocked & ~bit;
    }

    async function loadCfg() {
      cfgLoading.value = true; cfgError.value = null;
      try {
        const data = await API.get('/security/config');
        cfg.fence_mode = data.fence_mode ?? 1;
        cfg.method_blocked = data.method_blocked ?? 0;
      } catch (e) { cfgError.value = e.message; }
      cfgLoading.value = false;
    }

    async function saveCfg() {
      cfgSaving.value = true;
      try {
        const data = await API.put('/security/config', {
          fence_mode: cfg.fence_mode,
          method_blocked: cfg.method_blocked,
        });
        cfg.fence_mode = data.fence_mode;
        cfg.method_blocked = data.method_blocked;
        ElementPlus.ElMessage.success('配置已保存并推送到核心');
      } catch (e) { ElementPlus.ElMessage.error('保存失败: ' + e.message); }
      cfgSaving.value = false;
    }

    onMounted(() => { load(); loadBl(); loadCfg(); timer = setInterval(load, 10000); });
    onBeforeUnmount(() => clearInterval(timer));
    return {
      tab, rows, loading, error, query, fmtDur, reload, load, unban,
      blRows, blLoading, blError, blKind, blType, loadBl,
      dlgVisible, saving, form, openAdd, saveBl, toggleBl, removeBl,
      valueLabel, valuePlaceholder,
      cfg, cfgLoading, cfgError, cfgSaving,
      methodList, isMethodSet, toggleMethod, loadCfg, saveCfg,
    };
  },
});
