<script setup lang="ts">
// 录像设置（REC-05）：通道录像计划管理
const api = useApi()

const PROFILE_MAP: Record<string, string> = { main: '主码流', sub: '子码流' }

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
    const [cRes, tRes]: any[] = await Promise.all([api.get('/channels'), api.get('/record-templates')])
    channels.value = cRes.items || cRes || []
    templates.value = tRes.items || tRes || []
  } catch (e: any) {
    ElMessage.error(errText(e))
  }
}

// ---------- 计划列表 ----------
const plans = ref<any[]>([])
const loading = ref(false)

async function loadPlans() {
  loading.value = true
  try {
    const res: any = await api.get('/record-plans')
    plans.value = res.items || []
  } catch (e: any) {
    ElMessage.error(errText(e))
  } finally {
    loading.value = false
  }
}

async function togglePlan(row: any, v: any) {
  try {
    await api.put(`/record-plans/${row.id}`, { enabled: !!v })
  } catch (e: any) {
    row.enabled = !v // 失败回滚
    ElMessage.error(errText(e))
  }
}

async function delPlan(row: any) {
  try {
    await ElMessageBox.confirm(
      `确定删除通道「${channelMap.value[row.channelId] || row.channelId}」的录像计划？`,
      '删除确认',
      { type: 'warning' }
    )
  } catch {
    return
  }
  try {
    await api.del(`/record-plans/${row.id}`)
    ElMessage.success('已删除')
    loadPlans()
  } catch (e: any) {
    ElMessage.error(errText(e))
  }
}

// ---------- 新建计划 ----------
const dlg = ref(false)
const saving = ref(false)
const form = reactive({
  channelIds: [] as string[],
  templateId: '',
  profile: 'main'
})

function openDlg() {
  form.channelIds = []
  form.templateId = ''
  form.profile = 'main'
  dlg.value = true
}

async function save() {
  if (!form.channelIds.length) { ElMessage.warning('请选择通道'); return }
  if (!form.templateId) { ElMessage.warning('请选择录像模板'); return }
  saving.value = true
  try {
    await api.post('/record-plans', {
      channelIds: form.channelIds,
      templateId: form.templateId,
      profile: form.profile
    })
    ElMessage.success('录像计划已创建')
    dlg.value = false
    loadPlans()
  } catch (e: any) {
    ElMessage.error(errText(e))
  } finally {
    saving.value = false
  }
}

onMounted(async () => {
  await loadBase()
  loadPlans()
})
</script>

<template>
  <div class="plan-page">
    <div class="toolbar">
      <span class="title">录像计划</span>
      <el-button type="primary" size="small" @click="openDlg">新建计划</el-button>
    </div>

    <el-table v-loading="loading" :data="plans">
      <el-table-column label="通道" min-width="160">
        <template #default="{ row }">{{ channelMap[row.channelId] || row.channelId }}</template>
      </el-table-column>
      <el-table-column label="录像模板" min-width="160">
        <template #default="{ row }">{{ templateMap[row.templateId] || row.templateId || '-' }}</template>
      </el-table-column>
      <el-table-column label="码流" width="100">
        <template #default="{ row }">
          <el-tag size="small" :type="row.profile === 'sub' ? 'info' : 'primary'">
            {{ PROFILE_MAP[row.profile] || row.profile }}
          </el-tag>
        </template>
      </el-table-column>
      <el-table-column label="启用" width="80">
        <template #default="{ row }">
          <el-switch :model-value="row.enabled" @change="togglePlan(row, $event)" />
        </template>
      </el-table-column>
      <el-table-column label="操作" width="80">
        <template #default="{ row }">
          <el-button size="small" type="danger" plain link @click="delPlan(row)">删除</el-button>
        </template>
      </el-table-column>
    </el-table>

    <!-- 新建计划 -->
    <el-dialog v-model="dlg" title="新建录像计划" width="480px">
      <el-form label-width="80px">
        <el-form-item label="通道">
          <el-select
            v-model="form.channelIds" multiple filterable
            placeholder="选择一个或多个通道" style="width: 100%"
          >
            <el-option v-for="c in channels" :key="c.id" :label="c.name" :value="c.id" />
          </el-select>
        </el-form-item>
        <el-form-item label="录像模板">
          <el-select v-model="form.templateId" placeholder="选择录像模板" style="width: 100%">
            <el-option v-for="t in templates" :key="t.id" :label="t.name" :value="t.id" />
          </el-select>
        </el-form-item>
        <el-form-item label="码流">
          <el-select v-model="form.profile" style="width: 100%">
            <el-option label="主码流" value="main" />
            <el-option label="子码流" value="sub" />
          </el-select>
        </el-form-item>
      </el-form>
      <template #footer>
        <el-button @click="dlg = false">取消</el-button>
        <el-button type="primary" :loading="saving" @click="save">确定</el-button>
      </template>
    </el-dialog>
  </div>
</template>

<style scoped>
.plan-page { background: #fff; border: 1px solid #e4e7ed; border-radius: 6px; padding: 16px; }
.toolbar { display: flex; align-items: center; justify-content: space-between; margin-bottom: 12px; }
.title { font-weight: 600; color: #303133; }
</style>