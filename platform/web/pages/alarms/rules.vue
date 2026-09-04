<script setup lang="ts">
// 告警规则（ALM-02）+ 布防模板（ALM-01）
const api = useApi()

const DAY_NAMES = ['周一', '周二', '周三', '周四', '周五', '周六', '周日']
const KIND_OPTIONS = [
  { value: 'motion', label: '移动侦测' },
  { value: 'humanoid', label: '人形' },
  { value: 'tamper', label: '视频遮挡' },
  { value: 'io', label: 'IO报警' }
]
const kindName = (k: string) => KIND_OPTIONS.find((o) => o.value === k)?.label || k

// schedule 摘要：{days:[1,2],ranges:[["00:00","24:00"]]} → "周一、周二 00:00-24:00"
function fmtSchedule(s: any) {
  if (!s || !s.days?.length) return '未布防'
  const days = [...s.days]
    .sort((a: number, b: number) => a - b)
    .map((d: number) => DAY_NAMES[d - 1] || d)
    .join('、')
  const ranges = (s.ranges || []).map((r: string[]) => `${r[0]}-${r[1]}`).join('、')
  return `${days} ${ranges}`
}

function errText(e: any) {
  return `${e.code} ${e.msg} ${e.suggest || ''}`
}

// ---------- 基础数据（通道 / 模板映射） ----------
const channels = ref<any[]>([])
const templates = ref<any[]>([])
const channelMap = computed(() => Object.fromEntries(channels.value.map((c) => [c.id, c.name])))
const templateMap = computed(() => Object.fromEntries(templates.value.map((t) => [t.id, t.name])))

async function loadBase() {
  try {
    const [cRes, tRes]: any[] = await Promise.all([api.get('/channels'), api.get('/alarm-templates')])
    channels.value = cRes.items || cRes || []
    templates.value = tRes.items || tRes || []
  } catch (e: any) {
    ElMessage.error(errText(e))
  }
}

const tab = ref('rules')

// ---------- Tab A：告警规则 ----------
const rules = ref<any[]>([])
const rulesLoading = ref(false)

async function loadRules() {
  rulesLoading.value = true
  try {
    const res: any = await api.get('/alarm-rules')
    rules.value = res.items || []
  } catch (e: any) {
    ElMessage.error(errText(e))
  } finally {
    rulesLoading.value = false
  }
}

async function toggleRule(row: any, v: any) {
  try {
    await api.put(`/alarm-rules/${row.id}`, { enabled: !!v })
  } catch (e: any) {
    row.enabled = !v // 失败回滚
    ElMessage.error(errText(e))
  }
}

async function delRule(row: any) {
  try {
    await ElMessageBox.confirm(
      `确定删除通道「${channelMap.value[row.channelId] || row.channelId}」的告警规则？`,
      '删除确认',
      { type: 'warning' }
    )
  } catch {
    return
  }
  try {
    await api.del(`/alarm-rules/${row.id}`)
    ElMessage.success('已删除')
    loadRules()
  } catch (e: any) {
    ElMessage.error(errText(e))
  }
}

// 新建规则（多通道 + 多事件类型 + 布防模板）
const ruleDlg = ref(false)
const ruleSaving = ref(false)
const ruleForm = reactive({ channelIds: [] as string[], kinds: [] as string[], templateId: '' })

function openRuleDlg() {
  ruleForm.channelIds = []
  ruleForm.kinds = []
  ruleForm.templateId = ''
  ruleDlg.value = true
}

async function saveRule() {
  if (!ruleForm.channelIds.length) { ElMessage.warning('请选择通道'); return }
  if (!ruleForm.kinds.length) { ElMessage.warning('请选择事件类型'); return }
  if (!ruleForm.templateId) { ElMessage.warning('请选择布防模板'); return }
  ruleSaving.value = true
  try {
    await api.post('/alarm-rules', {
      channelIds: ruleForm.channelIds,
      kinds: ruleForm.kinds,
      templateId: ruleForm.templateId
    })
    ElMessage.success('规则已创建')
    ruleDlg.value = false
    loadRules()
  } catch (e: any) {
    ElMessage.error(errText(e))
  } finally {
    ruleSaving.value = false
  }
}

// ---------- Tab B：布防模板 ----------
const tplLoading = ref(false)
const tplDlg = ref(false)
const tplSaving = ref(false)
const tplForm = reactive({
  name: '',
  schedule: { days: [] as number[], ranges: [] as string[][] }
})

async function loadTemplates() {
  tplLoading.value = true
  try {
    const res: any = await api.get('/alarm-templates')
    templates.value = res.items || []
  } catch (e: any) {
    ElMessage.error(errText(e))
  } finally {
    tplLoading.value = false
  }
}

function openTplDlg() {
  tplForm.name = ''
  tplForm.schedule = { days: [], ranges: [] }
  tplDlg.value = true
}

async function saveTpl() {
  if (!tplForm.name.trim()) { ElMessage.warning('请填写模板名称'); return }
  tplSaving.value = true
  try {
    await api.post('/alarm-templates', { name: tplForm.name.trim(), schedule: tplForm.schedule })
    ElMessage.success('模板已创建')
    tplDlg.value = false
    loadTemplates()
  } catch (e: any) {
    ElMessage.error(errText(e))
  } finally {
    tplSaving.value = false
  }
}

async function delTpl(row: any) {
  if (row.builtin) return // 内置模板不可删
  try {
    await ElMessageBox.confirm(`确定删除布防模板「${row.name}」？`, '删除确认', { type: 'warning' })
  } catch {
    return
  }
  try {
    await api.del(`/alarm-templates/${row.id}`)
    ElMessage.success('已删除')
    loadTemplates()
  } catch (e: any) {
    ElMessage.error(errText(e))
  }
}

onMounted(async () => {
  await loadBase()
  loadRules()
})
</script>

<template>
  <div class="rules-page">
    <el-tabs v-model="tab">
      <!-- Tab A：告警规则 -->
      <el-tab-pane label="告警规则" name="rules">
        <div class="toolbar">
          <el-button type="primary" size="small" @click="openRuleDlg">新建规则</el-button>
        </div>
        <el-table v-loading="rulesLoading" :data="rules">
          <el-table-column label="通道" min-width="160">
            <template #default="{ row }">{{ channelMap[row.channelId] || row.channelId }}</template>
          </el-table-column>
          <el-table-column label="事件类型" min-width="220">
            <template #default="{ row }">
              <el-tag v-for="k in row.kinds" :key="k" size="small" style="margin-right: 4px">
                {{ kindName(k) }}
              </el-tag>
            </template>
          </el-table-column>
          <el-table-column label="布防模板" width="160">
            <template #default="{ row }">{{ templateMap[row.templateId] || row.templateId || '-' }}</template>
          </el-table-column>
          <el-table-column label="启用" width="80">
            <template #default="{ row }">
              <el-switch :model-value="row.enabled" @change="toggleRule(row, $event)" />
            </template>
          </el-table-column>
          <el-table-column label="操作" width="80">
            <template #default="{ row }">
              <el-button size="small" type="danger" plain link @click="delRule(row)">删除</el-button>
            </template>
          </el-table-column>
        </el-table>
      </el-tab-pane>

      <!-- Tab B：布防模板 -->
      <el-tab-pane label="布防模板" name="templates">
        <div class="toolbar">
          <el-button type="primary" size="small" @click="openTplDlg">新建模板</el-button>
        </div>
        <el-table v-loading="tplLoading" :data="templates">
          <el-table-column label="模板名称" min-width="140" prop="name" />
          <el-table-column label="布防时间" min-width="240">
            <template #default="{ row }">{{ fmtSchedule(row.schedule) }}</template>
          </el-table-column>
          <el-table-column label="内置" width="80">
            <template #default="{ row }">
              <el-tag v-if="row.builtin" size="small" type="info">内置</el-tag>
              <span v-else>-</span>
            </template>
          </el-table-column>
          <el-table-column label="操作" width="80">
            <template #default="{ row }">
              <el-button size="small" type="danger" plain link :disabled="row.builtin" @click="delTpl(row)">
                删除
              </el-button>
            </template>
          </el-table-column>
        </el-table>
      </el-tab-pane>
    </el-tabs>

    <!-- 新建告警规则 -->
    <el-dialog v-model="ruleDlg" title="新建告警规则" width="480px">
      <el-form label-width="80px">
        <el-form-item label="通道">
          <el-select
            v-model="ruleForm.channelIds" multiple filterable
            placeholder="选择一个或多个通道" style="width: 100%"
          >
            <el-option v-for="c in channels" :key="c.id" :label="c.name" :value="c.id" />
          </el-select>
        </el-form-item>
        <el-form-item label="事件类型">
          <el-checkbox-group v-model="ruleForm.kinds">
            <el-checkbox v-for="o in KIND_OPTIONS" :key="o.value" :value="o.value">{{ o.label }}</el-checkbox>
          </el-checkbox-group>
        </el-form-item>
        <el-form-item label="布防模板">
          <el-select v-model="ruleForm.templateId" placeholder="选择布防模板" style="width: 100%">
            <el-option v-for="t in templates" :key="t.id" :label="t.name" :value="t.id" />
          </el-select>
        </el-form-item>
      </el-form>
      <template #footer>
        <el-button @click="ruleDlg = false">取消</el-button>
        <el-button type="primary" :loading="ruleSaving" @click="saveRule">确定</el-button>
      </template>
    </el-dialog>

    <!-- 新建布防模板 -->
    <el-dialog v-model="tplDlg" title="新建布防模板" width="600px">
      <el-form label-width="80px">
        <el-form-item label="模板名称">
          <el-input v-model="tplForm.name" placeholder="如：夜间布防" maxlength="30" />
        </el-form-item>
        <el-form-item label="布防时间">
          <ScheduleGrid v-model="tplForm.schedule" />
        </el-form-item>
      </el-form>
      <template #footer>
        <el-button @click="tplDlg = false">取消</el-button>
        <el-button type="primary" :loading="tplSaving" @click="saveTpl">确定</el-button>
      </template>
    </el-dialog>
  </div>
</template>

<style scoped>
.rules-page { background: #fff; border: 1px solid #e4e7ed; border-radius: 6px; padding: 4px 16px 16px; }
.toolbar { display: flex; justify-content: flex-end; margin-bottom: 12px; }
</style>