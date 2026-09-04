<script setup lang="ts">
// 首次设置向导（AUTH-02）：创建项目 → 添加媒体节点 → 完成
definePageMeta({ layout: 'auth' })
const api = useApi()
const { loadMe } = useAuth()

const step = ref(0)
// 当前项目 id（新建后回填，完成时用于标记 setupDone）
const projectId = ref<string>(((useCookie('ipc_project').value as any) || '') as string)

// ---- 第 1 步：创建项目 ----
const projectName = ref('')
const creating = ref(false)
const err1 = ref('')

async function createProject() {
  if (!projectName.value.trim()) { err1.value = '请输入项目名称'; return }
  err1.value = ''
  creating.value = true
  try {
    const p: any = await api.post('/projects', { name: projectName.value.trim() })
    projectId.value = p.id
    // 写入当前项目 cookie，并刷新会话中的项目信息
    useCookie('ipc_project', { maxAge: 60 * 60 * 24 * 30 }).value = p.id
    await loadMe()
    ElMessage.success('项目创建成功')
    step.value = 1
  } catch (e: any) {
    err1.value = e.msg || '创建失败'
  } finally {
    creating.value = false
  }
}

// ---- 第 2 步：添加媒体节点 ----
const nodeForm = reactive({ name: '', apiUrl: '', secret: '', publicHost: '' })
const addingNode = ref(false)
const err2 = ref('')
const selfcheckTip = ref(false)

async function addNode() {
  if (!nodeForm.name.trim() || !nodeForm.apiUrl.trim()) {
    err2.value = '请填写节点名称和 API 地址'
    return
  }
  err2.value = ''
  addingNode.value = true
  try {
    const n: any = await api.post('/media-nodes', { ...nodeForm })
    // 提交自检（异步执行，结果可在"系统 → 媒体节点"页查看）
    try { await api.post(`/media-nodes/${n.id}/selfcheck`) } catch {}
    selfcheckTip.value = true
  } catch (e: any) {
    err2.value = e.msg || '添加失败'
  } finally {
    addingNode.value = false
  }
}

// ---- 进入完成页：标记项目 setupDone ----
async function finish() {
  step.value = 2
  if (!projectId.value) return
  try { await api.put(`/projects/${projectId.value}`, { setupDone: true }) } catch {}
}
</script>

<template>
  <el-card class="wizard-card">
    <div class="title">首次设置向导</div>
    <el-steps :active="step" align-center finish-status="success" class="steps">
      <el-step title="创建项目" />
      <el-step title="添加媒体节点" />
      <el-step title="完成" />
    </el-steps>

    <!-- 第 1 步：创建项目 -->
    <div v-if="step === 0">
      <el-form label-width="90px" @submit.prevent="createProject">
        <el-form-item label="项目名称" required>
          <el-input v-model="projectName" placeholder="如：园区监控" maxlength="32" />
        </el-form-item>
      </el-form>
      <el-alert v-if="err1" :title="err1" type="error" :closable="false" show-icon class="mb12" />
      <div class="btns">
        <el-button @click="step = 1">跳过</el-button>
        <el-button type="primary" :loading="creating" @click="createProject">创建项目</el-button>
      </div>
    </div>

    <!-- 第 2 步：添加媒体节点 -->
    <div v-if="step === 1">
      <el-form label-width="90px" @submit.prevent="addNode">
        <el-form-item label="节点名称" required>
          <el-input v-model="nodeForm.name" placeholder="如：主媒体节点" />
        </el-form-item>
        <el-form-item label="API 地址" required>
          <el-input v-model="nodeForm.apiUrl" placeholder="如：http://1.2.3.4:8080" />
        </el-form-item>
        <el-form-item label="通信密钥">
          <el-input v-model="nodeForm.secret" placeholder="节点通信密钥" />
        </el-form-item>
        <el-form-item label="公网地址">
          <el-input v-model="nodeForm.publicHost" placeholder="如：stream.example.com" />
        </el-form-item>
      </el-form>
      <el-alert v-if="err2" :title="err2" type="error" :closable="false" show-icon class="mb12" />
      <el-alert
        v-if="selfcheckTip" title="节点已添加，自检任务已提交" type="success" :closable="false" show-icon
        description="自检结果可在【系统 → 媒体节点】页面查看。" class="mb12"
      />
      <div class="btns">
        <el-button @click="finish">跳过</el-button>
        <el-button v-if="!selfcheckTip" type="primary" :loading="addingNode" @click="addNode">添加节点</el-button>
        <el-button v-else type="primary" @click="finish">下一步</el-button>
      </div>
    </div>

    <!-- 第 3 步：完成 -->
    <div v-if="step === 2">
      <el-result icon="success" title="初始设置完成" sub-title="接下来可以添加您的第一批摄像头设备。">
        <template #extra>
          <el-button type="primary" @click="navigateTo('/devices')">去添加设备</el-button>
        </template>
      </el-result>
    </div>
  </el-card>
</template>

<style scoped>
.wizard-card { width: 580px; }
.title { font-size: 18px; font-weight: 700; text-align: center; margin-bottom: 20px; color: #303133; }
.steps { margin-bottom: 28px; }
.btns { text-align: center; margin-top: 8px; }
.mb12 { margin-bottom: 12px; }
</style>