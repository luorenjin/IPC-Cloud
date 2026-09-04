<script setup lang="ts">
// 录像计划模板（REC-06）：定时/事件模板管理
const api = useApi()

const KIND_MAP: Record<string, string> = { timer: '定时录像', event: '事件录像' }
const DAY_NAMES = ['周一', '周二', '周三', '周四', '周五', '周六', '周日']

// schedule 摘要：{days:[1,2],ranges:[["00:00","24:00"]]} → "周一、周二 00:00-24:00"
function fmtSchedule(s: any) {
  if (!s || !s.days?.length) return '未设置'
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

// ---------- 列表 ----------
const items = ref<any[]>([])
const loading = ref(false)

async function load() {
  loading.value = true
  try {
    const res: any = await api.get('/record-templates')
    items.value = res.items || []
  } catch (e: any) {
    ElMessage.error(errText(e))
  } finally {
    loading.value = false
  }
}

// ---------- 新建模板 ----------
const dlg = ref(false)
const saving = ref(false)
const form = reactive({
  name: '',
  kind: 'timer',
  schedule: { days: [] as number[], ranges: [] as string[][] }
})

function openDlg() {
  form.name = ''
  form.kind = 'timer'
  form.schedule = { days: [], ranges: [] }
  dlg.value = true
}

async function save() {
  if (!form.name.trim()) { ElMessage.warning('请填写模板名称'); return }
  saving.value = true
  try {
    await api.post('/record-templates', {
      name: form.name.trim(),
      kind: form.kind,
      schedule: form.schedule
    })
    ElMessage.success('模板已创建')
    dlg.value = false
    load()
  } catch (e: any) {
    ElMessage.error(errText(e))
  } finally {
    saving.value = false
  }
}

// ---------- 删除模板 ----------
async function del(row: any) {
  if (row.builtin) return // 内置模板不可删
  try {
    await ElMessageBox.confirm(`确定删除录像模板「${row.name}」？`, '删除确认', { type: 'warning' })
  } catch {
    return
  }
  try {
    await api.del(`/record-templates/${row.id}`)
    ElMessage.success('已删除')
    load()
  } catch (e: any) {
    ElMessage.error(errText(e))
  }
}

onMounted(load)
</script>

<template>
  <div class="tpl-page">
    <div class="toolbar">
      <span class="title">录像计划模板</span>
      <el-button type="primary" size="small" @click="openDlg">新建模板</el-button>
    </div>

    <el-table v-loading="loading" :data="items">
      <el-table-column label="模板名称" min-width="140" prop="name" />
      <el-table-column label="类型" width="110">
        <template #default="{ row }">
          <el-tag size="small" :type="row.kind === 'event' ? 'warning' : 'primary'">
            {{ KIND_MAP[row.kind] || row.kind }}
          </el-tag>
        </template>
      </el-table-column>
      <el-table-column label="计划时间" min-width="240">
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
          <el-button size="small" type="danger" plain link :disabled="row.builtin" @click="del(row)">
            删除
          </el-button>
        </template>
      </el-table-column>
    </el-table>

    <!-- 新建模板 -->
    <el-dialog v-model="dlg" title="新建录像模板" width="600px">
      <el-form label-width="80px">
        <el-form-item label="模板名称">
          <el-input v-model="form.name" placeholder="如：全天定时录像" maxlength="30" />
        </el-form-item>
        <el-form-item label="类型">
          <el-radio-group v-model="form.kind">
            <el-radio value="timer">定时录像</el-radio>
            <el-radio value="event">事件录像</el-radio>
          </el-radio-group>
        </el-form-item>
        <el-form-item label="录像时间">
          <ScheduleGrid v-model="form.schedule" />
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
.tpl-page { background: #fff; border: 1px solid #e4e7ed; border-radius: 6px; padding: 16px; }
.toolbar { display: flex; align-items: center; justify-content: space-between; margin-bottom: 12px; }
.title { font-weight: 600; color: #303133; }
</style>