// ===== Security Center（/api/v1/security/*） =====
// Tab 1 Dynamic Ban：内置 fail2ban（REGISTER 认证失败 N 次/窗口 → 自动封 IP）
//   的查看/Search/Unban入口。误封真实客户时在此Unban。
// Tab 2 Fence Rules：NIP 三层 + deny/allow 双Mode
//   rule_type: ip_cidr / ua / called_prefix / caller_prefix
//   kind: deny（命中即丢）/ allow（命中才放；allow 配了未命中一律丢）
//   After save, auto-push to nsbcd（security.blacklist.reload 整表替换）。
// Tab 3 Global Config：fence_mode（security/performance）+ method_blocked（BitmapDisable）。
app.component('security-center', {
  template: `
    <div>
      <div class="flex items-center justify-between mb-4">
        <h2 class="text-lg font-semibold">
          <i class="fa-solid fa-shield-halved"></i> Security Center
        </h2>
      </div>

      <el-tabs v-model="tab">

        <!-- ================ Tab 1: Dynamic Ban ================ -->
        <el-tab-pane label="🚫 Dynamic Ban" name="bans">
          <div class="flex items-center justify-between mb-3">
            <el-input v-model="query" placeholder="Search by IP / UA / reason"
              size="small" style="width:240px" clearable
              @keyup.enter="reload" @clear="reload"></el-input>
            <div class="flex items-center gap-2">
              <el-button size="small" type="primary" @click="reload">
                <i class="fa-solid fa-magnifying-glass"></i> Search
              </el-button>
              <el-button size="small" @click="load">
                <i class="fa-solid fa-rotate"></i> Refresh
              </el-button>
            </div>
          </div>

          <el-alert type="info" :closable="false" class="mb-3"
            title="Auto-ban when same IP hits REGISTER auth failure threshold within the counting window；Ban期间其 REGISTER/INVITE 均静默丢弃。Observing = count below threshold; auto-unban when remaining reaches zero. Click Unban to clear immediately. Restarting nsbcd also clears the table." />

          <div v-if="loading" class="text-gray-400 py-8 text-center">{{ t('Loading......') }}</div>
          <div v-else-if="error" class="text-red-500 py-8 text-center">⚠ {{ error }}</div>
          <div v-else-if="!rows.length" class="text-gray-400 py-8 text-center">
            <i class="fa-solid fa-circle-check text-2xl mb-2 block text-green-500"></i>
            No banned or observing IPs
          </div>
          <el-table v-else :data="rows" size="small" border>
            <el-table-column label="IP Address" min-width="150">
              <template #default="{ row }">
                <span class="font-mono">{{ row.ip }}</span>
              </template>
            </el-table-column>
            <el-table-column label="State" width="100">
              <template #default="{ row }">
                <el-tag v-if="row.state === 'ban'" type="danger" size="small">
                  Banned
                </el-tag>
                <el-tag v-else type="warning" size="small">Observing</el-tag>
              </template>
            </el-table-column>
            <el-table-column label="Fails" width="90" align="center">
              <template #default="{ row }">{{ row.fails }}</template>
            </el-table-column>
            <el-table-column label="Remaining" width="110" align="center">
              <template #default="{ row }">
                <span v-if="row.state === 'ban'" class="text-red-600 font-mono">
                  {{ fmtDur(row.remaining_s) }}
                </span>
                <span v-else>-</span>
              </template>
            </el-table-column>
            <el-table-column label="Last Activity" width="110" align="center">
              <template #default="{ row }">{{ row.act_ago_s }}s ago</template>
            </el-table-column>
            <el-table-column label="Action" width="90" align="center">
              <template #default="{ row }">
                <el-popconfirm :title="'Unban ' + row.ip + '？(clear all counts & ban status)'"
                  confirm-button-text="Unban" cancel-button-text="Cancel"
                  @confirm="unban(row.ip)">
                  <template #reference>
                    <el-button size="small" type="danger" plain>Unban</el-button>
                  </template>
                </el-popconfirm>
              </template>
            </el-table-column>
          </el-table>
        </el-tab-pane>

        <!-- ================ Tab 2: Fence Rules ================ -->
        <el-tab-pane label="🛡️ Fence Rules" name="fence">
          <div class="flex items-center justify-between mb-3">
            <div class="flex items-center gap-3">
              <el-radio-group v-model="blKind" size="small" @change="loadBl">
                <el-radio-button label="">{{ t('All') }}</el-radio-button>
                <el-radio-button label="deny">{{ t('Blacklist (deny)') }}</el-radio-button>
                <el-radio-button label="allow">{{ t('Whitelist (allow)') }}</el-radio-button>
              </el-radio-group>
              <el-radio-group v-model="blType" size="small" @change="loadBl">
                <el-radio-button label="">{{ t('All') }}</el-radio-button>
                <el-radio-button label="ip_cidr">IP CIDR</el-radio-button>
                <el-radio-button label="ua">UA</el-radio-button>
                <el-radio-button label="called_prefix">{{ t('Called Prefix') }}</el-radio-button>
                <el-radio-button label="caller_prefix">{{ t('Caller Prefix') }}</el-radio-button>
              </el-radio-group>
            </div>
            <div class="flex items-center gap-2">
              <el-button size="small" @click="loadBl">
                <i class="fa-solid fa-rotate"></i> Refresh
              </el-button>
              <el-button size="small" type="primary" @click="openAdd">
                <i class="fa-solid fa-plus"></i> New Rule
              </el-button>
            </div>
          </div>

          <el-alert type="warning" :closable="false" class="mb-3"
            title="3-layer NIP fence — deny rules silently drop on match (no SIP reply); allow rules only pass on match. Priority: method disable → deny allow → allowlist miss → deny hit → pass.After save, auto-push to nsbcd; takes effect immediately。" />

          <div v-if="blLoading" class="text-gray-400 py-8 text-center">{{ t('Loading......') }}</div>
          <div v-else-if="blError" class="text-red-500 py-8 text-center">⚠ {{ blError }}</div>
          <div v-else-if="!blRows.length" class="text-gray-400 py-8 text-center">
            <i class="fa-solid fa-circle-check text-2xl mb-2 block text-green-500"></i>
            No fence rules
          </div>
          <el-table v-else :data="blRows" size="small" border>
            <el-table-column label="Mode" width="100">
              <template #default="{ row }">
                <el-tag v-if="row.kind === 'allow'" type="success" size="small">ALLOW</el-tag>
                <el-tag v-else type="danger" size="small">DENY</el-tag>
              </template>
            </el-table-column>
            <el-table-column label="Type" width="110">
              <template #default="{ row }">
                <el-tag v-if="row.rule_type === 'ip_cidr'" type="info" size="small">IP CIDR</el-tag>
                <el-tag v-else-if="row.rule_type === 'ua'" type="warning" size="small">UA</el-tag>
                <el-tag v-else-if="row.rule_type === 'called_prefix'" type="" size="small">Called Prefix</el-tag>
                <el-tag v-else type="" size="small">Caller Prefix</el-tag>
              </template>
            </el-table-column>
            <el-table-column label="Value" min-width="200">
              <template #default="{ row }">
                <span class="font-mono">{{ row.value }}</span>
              </template>
            </el-table-column>
            <el-table-column label="Notes" min-width="120" show-overflow-tooltip>
              <template #default="{ row }">{{ row.comment || '-' }}</template>
            </el-table-column>
            <el-table-column label="Enabled" width="80" align="center">
              <template #default="{ row }">
                <el-switch :model-value="row.enabled === 1" size="small"
                  @change="v => toggleBl(row, v)" />
              </template>
            </el-table-column>
            <el-table-column label="Created" width="170">
              <template #default="{ row }">{{ row.created_at }}</template>
            </el-table-column>
            <el-table-column label="Action" width="90" align="center">
              <template #default="{ row }">
                <el-popconfirm :title="'Delete rule ' + row.value + '？'"
                  confirm-button-text="Delete" cancel-button-text="Cancel"
                  @confirm="removeBl(row.id)">
                  <template #reference>
                    <el-button size="small" type="danger" plain>Delete</el-button>
                  </template>
                </el-popconfirm>
              </template>
            </el-table-column>
          </el-table>

          <!-- New Rule dialog -->
          <el-dialog v-model="dlgVisible" title="New Fence Rule" width="480px">
            <el-form label-width="90px">
              <el-form-item label="Mode">
                <el-radio-group v-model="form.kind">
                  <el-radio label="deny">Blacklist (match → drop)</el-radio>
                  <el-radio label="allow">Whitelist (match → allow)</el-radio>
                </el-radio-group>
              </el-form-item>
              <el-form-item label="Type">
                <el-select v-model="form.type" style="width:200px">
                  <el-option label="IP CIDR" value="ip_cidr"></el-option>
                  <el-option label="UA Substring" value="ua"></el-option>
                  <el-option label="Called Prefix" value="called_prefix"></el-option>
                  <el-option label="Caller Prefix" value="caller_prefix"></el-option>
                </el-select>
              </el-form-item>
              <el-form-item :label="valueLabel">
                <el-input v-model="form.value" :placeholder="valuePlaceholder"></el-input>
              </el-form-item>
              <el-form-item label="Notes">
                <el-input v-model="form.comment" maxlength="64"
                  placeholder="Optional" />
              </el-form-item>
              <el-form-item label="Enable">
                <el-switch v-model="form.enabled" />
              </el-form-item>
            </el-form>
            <template #footer>
              <el-button @click="dlgVisible = false">{{ t('Cancel') }}</el-button>
              <el-button type="primary" :loading="saving" @click="saveBl">{{ t('Save') }}</el-button>
            </template>
          </el-dialog>
        </el-tab-pane>

        <!-- ================ Tab 3: Global Config ================ -->
        <el-tab-pane label="⚙️ Global Config" name="config">
          <div v-if="cfgLoading" class="text-gray-400 py-8 text-center">{{ t('Loading......') }}</div>
          <div v-else-if="cfgError" class="text-red-500 py-8 text-center">⚠ {{ cfgError }}</div>
          <div v-else class="space-y-5">

            <!-- fence_mode -->
            <el-card shadow="never" class="border">
              <template #header>
                <div class="flex items-center gap-2 font-semibold">
                  <i class="fa-solid fa-layer-group"></i> Fence Mode
                </div>
              </template>
              <div class="flex items-center justify-between">
                <div>
                  <div class="text-sm font-medium">fence_mode</div>
                  <div class="text-xs text-gray-500 mt-1">
                    performance (0): fully disable software fence, rely on hardware；
                    security (1): full fence check (recommended)
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
                  <i class="fa-solid fa-ban"></i> Disable SIP Methods
                </div>
              </template>
              <div class="text-xs text-gray-500 mb-3">
                Requests matching disabled methods are silently dropped. Bitmap (1<<(idx-1)), 0=allow.
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
                Current bitmap：<span class="font-mono text-red-500">0x{{ cfg.method_blocked.toString(16) }}</span>
              </div>
            </el-card>

            <!-- fail2ban 五维 -->
            <el-card shadow="never" class="border">
              <template #header>
                <div class="flex items-center gap-2 font-semibold">
                  <i class="fa-solid fa-triangle-exclamation"></i> Auto-Ban (fail2ban)
                </div>
              </template>

              <div class="flex items-center justify-between mb-4">
                <div>
                  <div class="text-sm font-medium">auto_ban</div>
                  <div class="text-xs text-gray-500 mt-1">
                    连续 REGISTER 鉴权失败 threshold 次 / 统计窗口内 → 自动封禁 IP；
                    封禁期间其 REGISTER/INVITE 静默丢弃
                  </div>
                </div>
                <el-switch
                  :model-value="cfg.auto_ban === 1"
                  active-text="ON" inactive-text="OFF"
                  inline-prompt
                  @change="v => cfg.auto_ban = v ? 1 : 0"
                />
              </div>

              <div class="grid grid-cols-2 md:grid-cols-3 gap-4">
                <div>
                  <label class="text-xs text-gray-500 block mb-1">
                    <i class="fa-solid fa-crosshairs text-orange-500 mr-1"></i>
                    Fail Threshold（次 / 窗口）
                  </label>
                  <el-input-number v-model="cfg.authfail_threshold"
                    :min="1" :max="1000" :step="1" size="small"
                    style="width:100%" />
                  <div class="text-xs text-gray-400 mt-1">1–1000，默认 3</div>
                </div>
                <div>
                  <label class="text-xs text-gray-500 block mb-1">
                    <i class="fa-solid fa-clock text-blue-500 mr-1"></i>
                    统计窗口（秒）
                  </label>
                  <el-input-number v-model="cfg.authfail_window_s"
                    :min="1" :max="86400" :step="10" size="small"
                    style="width:100%" />
                  <div class="text-xs text-gray-400 mt-1">1–86400，默认 60</div>
                </div>
                <div>
                  <label class="text-xs text-gray-500 block mb-1">
                    <i class="fa-solid fa-hourglass-half text-red-500 mr-1"></i>
                    封禁时长（秒）
                  </label>
                  <el-input-number v-model="cfg.ban_duration_s"
                    :min="1" :max="604800" :step="60" size="small"
                    style="width:100%" />
                  <div class="text-xs text-gray-400 mt-1">1–604800(7d)，默认 300</div>
                </div>
                <div>
                  <label class="text-xs text-gray-500 block mb-1">
                    <i class="fa-solid fa-gauge-high text-purple-500 mr-1"></i>
                    REGISTER 速率上限（条 / 窗口）
                  </label>
                  <el-input-number v-model="cfg.ratelimit_regs"
                    :min="1" :max="1000" :step="1" size="small"
                    style="width:100%" />
                  <div class="text-xs text-gray-400 mt-1">1–1000，默认 20</div>
                </div>
                <div>
                  <label class="text-xs text-gray-500 block mb-1">
                    <i class="fa-solid fa-clock text-blue-500 mr-1"></i>
                    速率窗口（秒）
                  </label>
                  <el-input-number v-model="cfg.ratelimit_window_s"
                    :min="1" :max="3600" :step="5" size="small"
                    style="width:100%" />
                  <div class="text-xs text-gray-400 mt-1">1–3600，默认 60</div>
                </div>
              </div>

              <el-alert type="warning" :closable="false" class="mt-4"
                title="生效时机：保存后 nsbc-web 立即 push 给 nsbcd（security.blacklist.reload <json>），C 侧原子替换 — 新 REGISTER 立刻走新阈值；已在 ban/观察计数器里的 IP 不受影响。" />
            </el-card>

            <div class="flex justify-end gap-2">
              <el-button size="small" @click="loadCfg">{{ t('Reload') }}</el-button>
              <el-button size="small" type="primary" :loading="cfgSaving" @click="saveCfg">
                <i class="fa-solid fa-floppy-disk"></i> Save & Push to Core
              </el-button>
            </div>
          </div>
        </el-tab-pane>

      </el-tabs>
    </div>
  `,

  setup() {
    const tab = ref('bans');

    // ============ Tab 1: Dynamic Ban ============
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
        ElementPlus.ElMessage.success('Unbanned ' + ip);
        load();
      } catch (e) {
        ElementPlus.ElMessage.error('Unban failed: ' + e.message);
      }
    }

    // ============ Tab 2: Fence Rules ============
    const blRows = ref([]), blLoading = ref(false), blError = ref(null);
    const blKind = ref(''), blType = ref('');
    const dlgVisible = ref(false), saving = ref(false);
    const form = reactive({ kind: 'deny', type: 'ip_cidr', value: '', comment: '', enabled: true });

    const valueLabel = computed(() => {
      switch (form.type) {
        case 'ip_cidr': return 'IP / CIDR';
        case 'ua': return t('UA Substring');
        case 'called_prefix': return t('Callee Number Prefix');
        case 'caller_prefix': return t('Caller ID Prefix');
        default: return 'Value';
      }
    });
    const valuePlaceholder = computed(() => {
      switch (form.type) {
        case 'ip_cidr': return t('e.g. 66.94.0.0/16 or 1.2.3.4 (single IP auto-adds /32)');
        case 'ua': return t('e.g. friendly-scanner (case-insensitive substring)');
        case 'called_prefix': return t('e.g. 100* or 100 (exact)');
        case 'caller_prefix': return t('e.g. 900*');
        default: return 'Value';
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
      if (!v) { ElementPlus.ElMessage.warning('Please fill in rule value'); return; }
      saving.value = true;
      try {
        await API.post('/security/blacklist', {
          kind: form.kind, type: form.type, value: v,
          comment: (form.comment || '').trim(), enabled: form.enabled,
        });
        ElementPlus.ElMessage.success('Saved & pushed to Core');
        dlgVisible.value = false;
        loadBl();
      } catch (e) {
        ElementPlus.ElMessage.error('Save failed: ' + e.message);
      }
      saving.value = false;
    }

    async function toggleBl(row, v) {
      try {
        await API.put('/security/blacklist/' + row.id, { enabled: v });
        row.enabled = v ? 1 : 0;
        ElementPlus.ElMessage.success(v ? t('Enabled & pushed') : t('Disabled & pushed'));
      } catch (e) { ElementPlus.ElMessage.error('Action failed: ' + e.message); }
    }

    async function removeBl(id) {
      try {
        await API.delete('/security/blacklist/' + id);
        ElementPlus.ElMessage.success('Deleted & pushed');
        loadBl();
      } catch (e) { ElementPlus.ElMessage.error('Delete failed: ' + e.message); }
    }

    // ============ Tab 3: Global Config ============
    const cfg = reactive({
      fence_mode: 1, method_blocked: 0,
      // fail2ban 五维（与后端 security_config 表对齐）
      auto_ban: 1, authfail_threshold: 3, authfail_window_s: 60,
      ban_duration_s: 300, ratelimit_regs: 20, ratelimit_window_s: 60,
    });
    const cfgLoading = ref(false), cfgError = ref(null), cfgSaving = ref(false);

    // method_blocked Bitmap定义（与 C 侧 sip_method_t 对齐）
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
        cfg.fence_mode           = data.fence_mode         ?? 1;
        cfg.method_blocked       = data.method_blocked     ?? 0;
        cfg.auto_ban             = data.auto_ban           ?? 1;
        cfg.authfail_threshold   = data.authfail_threshold ?? 3;
        cfg.authfail_window_s    = data.authfail_window_s  ?? 60;
        cfg.ban_duration_s       = data.ban_duration_s     ?? 300;
        cfg.ratelimit_regs       = data.ratelimit_regs     ?? 20;
        cfg.ratelimit_window_s   = data.ratelimit_window_s ?? 60;
      } catch (e) { cfgError.value = e.message; }
      cfgLoading.value = false;
    }

    async function saveCfg() {
      cfgSaving.value = true;
      try {
        const data = await API.put('/security/config', {
          fence_mode: cfg.fence_mode,
          method_blocked: cfg.method_blocked,
          auto_ban: cfg.auto_ban,
          authfail_threshold: cfg.authfail_threshold,
          authfail_window_s: cfg.authfail_window_s,
          ban_duration_s: cfg.ban_duration_s,
          ratelimit_regs: cfg.ratelimit_regs,
          ratelimit_window_s: cfg.ratelimit_window_s,
        });
        // API 返回完整行 — 用后端值覆写（防御后端 clamp 默认值）
        Object.assign(cfg, data);
        ElementPlus.ElMessage.success('Config saved & pushed to Core');
      } catch (e) { ElementPlus.ElMessage.error('Save failed: ' + e.message); }
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
