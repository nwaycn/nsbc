// ===== Config Hot-Editor (GET /config + GET /config/:key + POST /config/set + POST /config/validate) =====
// Backend contract (web/src/routes/config.rs):
//   GET  /config                  → ConfigExport { redacted, items: [{key, value?, secret}] }
//   GET  /config/:key             → ConfigValue  { key, value?, layer, secret }
//   POST /config/validate         → { valid, key } or 400 + reason
//   POST /config/set              → ConfigSetResult { key, changed }
//   GET  /listens                 → List of core listen addresses

app.component('config-editor', {
  template: `
    <div class="crud-page">
      <div class="page-header">
        <h2>{{ t('Config Hot-Editor') }}</h2>
        <div class="header-actions">
          <el-button size="small" @click="load">
            <i class="fas fa-sync-alt"></i> {{ t('Refresh') }}
          </el-button>
        </div>
      </div>

      <!-- Listen addresses (core bind points, read-only) -->
      <div class="mb-4 p-3 bg-gray-50 rounded border border-gray-200" v-if="listens.length">
        <div class="text-xs text-gray-500 mb-1">
          <i class="fas fa-network-wired mr-1"></i>{{ t('Core Listen Addresses') }}
        </div>
        <div v-for="l in listens" :key="l" class="text-xs font-mono text-gray-700">{{ l }}</div>
      </div>

      <el-table :data="items" v-loading="loading" style="width:100%" :height="'calc(100vh - 260px)'">
        <el-table-column prop="key" :label="t('Key')" width="280" fixed />
        <el-table-column :label="t('Effective Value')" min-width="300">
          <template #default="{row}">
            <span v-if="row.secret" class="text-gray-400 italic">
              <i class="fas fa-lock mr-1"></i>{{ row.value || '****' }}
            </span>
            <span v-else class="font-mono break-all text-sm">{{ row.value ?? t('(unset)') }}</span>
          </template>
        </el-table-column>
        <el-table-column prop="layer" :label="t('Layer')" width="120" />
        <el-table-column :label="t('Action')" width="140" fixed="right">
          <template #default="{row}">
            <el-button
              size="small"
              :disabled="row.secret"
              @click="openEdit(row)"
            >
              <i class="fas fa-edit mr-1"></i>{{ t('Edit') }}
            </el-button>
          </template>
        </el-table-column>
      </el-table>

      <!-- Edit Dialog -->
      <el-dialog
        v-model="dialogVisible"
        :title="t('Hot-Set Config')"
        width="520px"
      >
        <el-form label-width="120px">
          <el-form-item :label="t('Key')">
            <el-input v-model="editKey" disabled />
          </el-form-item>
          <el-form-item :label="t('Layer')" v-if="editLayer">
            <el-tag type="info">{{ editLayer }}</el-tag>
          </el-form-item>
          <el-form-item :label="t('New Value')">
            <!-- Enum keys: dropdown for safe selection; text-input for all others -->
            <el-select
              v-if="enumOptions"
              v-model="editValue"
              @change="clearValidate"
              style="width:100%"
            >
              <el-option
                v-for="opt in enumOptions"
                :key="opt.value"
                :label="opt.label"
                :value="opt.value"
              />
            </el-select>
            <el-input
              v-else
              v-model="editValue"
              type="textarea"
              :rows="3"
              :placeholder="t('(empty = delete)')"
              @input="clearValidate"
            />
          </el-form-item>
          <el-form-item v-if="validateMsg">
            <el-alert
              :title="validateMsg"
              :type="validOk ? 'success' : 'error'"
              show-icon
              :closable="false"
            />
          </el-form-item>
        </el-form>
        <template #footer>
          <el-button @click="dialogVisible = false">{{ t('Cancel') }}</el-button>
          <el-button type="warning" @click="doValidate" :loading="validating">
            <i class="fas fa-check-circle mr-1"></i>{{ t('Validate (Dry-run)') }}
          </el-button>
          <el-button type="primary" :disabled="!validOk" @click="doSet" :loading="setting">
            <i class="fas fa-save mr-1"></i>{{ t('Hot-Set') }}
          </el-button>
        </template>
      </el-dialog>
    </div>
  `,
  setup() {
    const api = API;
    const items = Vue.ref([]);
    const listens = Vue.ref([]);
    const loading = Vue.ref(false);

    const dialogVisible = Vue.ref(false);
    const editKey = Vue.ref('');
    const editValue = Vue.ref('');
    const editLayer = Vue.ref('');
    const validateMsg = Vue.ref('');
    const validOk = Vue.ref(false);
    const validating = Vue.ref(false);
    const setting = Vue.ref(false);

    /* Enum key → dropdown options map (kept in sync with src/core/cfg_schema.c enum lists) */
    const ENUM_OPTIONS = {
      'recording.format': [
        { value: 'wav',  label: 'WAV PCM (原生, 无依赖)' },
        { value: 'mp3',  label: 'MP3 (libmp3lame, 通用播放器兼容)' },
        { value: 'opus', label: 'Opus (OGG Opus, 省存储, 专利无关)' },
        { value: 'mp4',  label: 'MP4 AAC (AAC 音频轨, .mp4 容器, 可被 <audio> 直接播放)' },
      ],
      'recording.mode': [
        { value: 'single',  label: '单声道 (mix 两路 PCM)' },
        { value: 'mixed',   label: '混合单声道 (两路叠加)' },
        { value: 'separate', label: '双声道 (L=A路, R=B路)' },
      ],
      'force_register_ok': [
        { value: '0', label: '0 — 启用 digest auth + security fence' },
        { value: '1', label: '1 — 跳过 auth (IAD/国产 SIP 设备必选)' },
      ],
    };
    const enumOptions = Vue.computed(() => ENUM_OPTIONS[editKey.value] || null);

    const load = async () => {
      loading.value = true;
      try {
        const r = await api.get('/config');
        items.value = r.data.items || [];
      } catch (e) {
        ElementPlus.ElMessage.error(e.message);
      }
      // Listen addresses
      try {
        const lr = await api.get('/listens');
        listens.value = lr.data || [];
      } catch (_) { /* optional */ }
      loading.value = false;
    };

    const openEdit = async (row) => {
      clearValidate();
      editKey.value = row.key;
      editLayer.value = row.layer || '';
      // Fetch current effective value via GET /config/:key for accuracy
      try {
        const r = await api.get('/config/' + encodeURIComponent(row.key));
        editValue.value = r.data.value ?? '';
        editLayer.value = r.data.layer || editLayer.value;
      } catch (_) {
        editValue.value = row.value ?? '';
      }
      dialogVisible.value = true;
    };

    const clearValidate = () => {
      validateMsg.value = '';
      validOk.value = false;
    };

    const doValidate = async () => {
      validating.value = true;
      try {
        await api.post('/config/validate', { key: editKey.value, value: editValue.value });
        validOk.value = true;
        validateMsg.value = t('Valid — core accepts this value');
      } catch (e) {
        validOk.value = false;
        validateMsg.value = e.message || t('Core rejected this value');
      }
      validating.value = false;
    };

    const doSet = async () => {
      setting.value = true;
      try {
        const r = await api.post('/config/set', { key: editKey.value, value: editValue.value });
        const changed = r.data.changed;
        ElementPlus.ElMessage.success(
          changed ? t('Hot-set applied — core value changed') : t('Idempotent — value already equals target')
        );
        dialogVisible.value = false;
        await load();
      } catch (e) {
        ElementPlus.ElMessage.error(e.message);
      }
      setting.value = false;
    };

    Vue.onMounted(load);
    return {
      items, listens, loading,
      dialogVisible, editKey, editValue, editLayer,
      enumOptions,
      validateMsg, validOk, validating, setting,
      load, openEdit, clearValidate, doValidate, doSet,
    };
  },
});
