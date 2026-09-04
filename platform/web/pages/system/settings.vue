<script setup lang="ts">
// 系统设置（SET-01）：全局参数 + IDP 服务状态（只读）
const api = useApi()
const { currentProject, loadMe } = useAuth()

const loading = ref(false)
const saving = ref(false)

/* 全局参数表单（缺省时使用默认值） */
const form = reactive({
  streamIdleSec: 30, // 停流等待秒数
  playTokenTtlMin: 10, // 播放 token 有效期（分钟）
  gbPassword: '', // 国标全局 SIP 密码（存储 key: gb.password）
  autoInit: false // 新设备自动应用默认策略
})

// 兼容两种存储形态：{ value: xxx } 包装 或 裸值
function pick(v: any): any {
  if (v && typeof v === 'object' && 'value' in v) return v.value
  return v
}

async function loadSettings() {
  loading.value = true
  try {
    const res: any = await api.get('/settings', { scope: 'global' })
    // 返回可能是 JSONB 键值对象（可能为空 {}），也可能是 { items: [...] }
    let map: Record<string, any> = {}
    if (Array.isArray(res?.items)) {
      res.items.forEach((it: any) => {
        if (it?.key !== undefined) map[it.key] = it.value
      })
    } else if (res && typeof res === 'object') {
      map = res
    }
    const idle = pick(map.streamIdleSec)
    form.streamIdleSec = idle === undefined || idle === null || idle === '' ? 30 : Number(idle)
    const ttl = pick(map.playTokenTtlMin)
    form.playTokenTtlMin = ttl === undefined || ttl === null || ttl === '' ? 10 : Number(ttl)
    const gb = pick(map['gb.password'])
    form.gbPassword = gb === undefined || gb === null ? '' : String(gb)
    form.autoInit = !!pick(map.autoInit)
  } catch (e: any) {
    ElMessage.error(e?.msg || '加载设置失败')
  } finally {
    loading.value = false
  }
}

/* 保存：每个字段单独 PUT /settings { key, value: { value }, scope: 'global' } */
async function saveSettings() {
  saving.value = true
  try {
    await api.put('/settings', {
      key: 'streamIdleSec',
      value: { value: Number(form.streamIdleSec) },
      scope: 'global'
    })
    await api.put('/settings', {
      key: 'playTokenTtlMin',
      value: { value: Number(form.playTokenTtlMin) },
      scope: 'global'
    })
    await api.put('/settings', {
      key: 'gb.password',
      value: { value: String(form.gbPassword || '') },
      scope: 'global'
    })
    await api.put('/settings', {
      key: 'autoInit',
      value: { value: !!form.autoInit },
      scope: 'global'
    })
    ElMessage.success('设置已保存')
  } catch (e: any) {
    ElMessage.error(e?.msg || '保存失败')
  } finally {
    saving.value = false
  }
}

/* ---------------- IDP 服务（只读展示） ---------------- */
const idp = ref<any>(null)
const idpLoading = ref(false)

async function loadIdp() {
  idpLoading.value = true
  try {
    idp.value = await api.get('/idp/config')
  } catch {
    idp.value = null // IDP 未配置时静默展示占位
  } finally {
    idpLoading.value = false
  }
}

onMounted(async () => {
  // 布局可能尚未完成会话加载，兜底拉取一次
  if (!currentProject.value) await loadMe()
  await Promise.all([loadSettings(), loadIdp()])
})
</script>

<template>
  <div v-loading="loading" class="page">
    <!-- 全局参数 -->
    <el-card shadow="never" class="block">
      <template #header><span>全局参数</span></template>
      <el-form label-width="200px" style="max-width: 680px" @submit.prevent>
        <el-form-item label="停流等待秒数">
          <el-input-number v-model="form.streamIdleSec" :min="5" :max="600" controls-position="right" />
          <span class="tip">流空闲超过该秒数后自动停流</span>
        </el-form-item>
        <el-form-item label="播放 token 有效期（分钟）">
          <el-input-number v-model="form.playTokenTtlMin" :min="1" :max="1440" controls-position="right" />
          <span class="tip">播放令牌的有效时长</span>
        </el-form-item>
        <el-form-item label="国标全局 SIP 密码">
          <el-input
            v-model="form.gbPassword"
            show-password
            placeholder="国标设备统一 SIP 认证密码"
            maxlength="64"
            style="max-width: 320px"
          />
        </el-form-item>
        <el-form-item label="新设备自动应用默认策略">
          <el-switch v-model="form.autoInit" />
          <span class="tip">开启后新接入设备自动套用默认录像 / 告警策略</span>
        </el-form-item>
        <el-form-item>
          <el-button type="primary" :loading="saving" @click="saveSettings">保存设置</el-button>
          <el-button @click="loadSettings">重置</el-button>
        </el-form-item>
      </el-form>
    </el-card>

    <!-- IDP 服务（只读） -->
    <el-card v-loading="idpLoading" shadow="never" class="block">
      <template #header><span>IDP 服务</span></template>
      <el-descriptions v-if="idp" :column="2" border>
        <el-descriptions-item label="Broker 服务地址">{{ idp.broker || '—' }}</el-descriptions-item>
        <el-descriptions-item label="TLS">
          <el-tag :type="idp.tls ? 'success' : 'info'" size="small">
            {{ idp.tls ? '已启用' : '未启用' }}
          </el-tag>
        </el-descriptions-item>
        <el-descriptions-item label="CA 状态">{{ idp.caStatus || '—' }}</el-descriptions-item>
        <el-descriptions-item label="CRL">
          {{ Array.isArray(idp.crl) ? idp.crl.length + ' 条' : (idp.crl ?? '—') }}
        </el-descriptions-item>
      </el-descriptions>
      <el-empty v-else description="IDP 服务未配置" :image-size="60" />

      <!-- CRL 列表占位 -->
      <div class="crl-block">
        <div class="crl-title">CRL 列表</div>
        <el-empty description="占位：CRL 列表暂未提供" :image-size="48" />
      </div>
    </el-card>
  </div>
</template>

<style scoped>
.block { margin-bottom: 16px; }
.tip { color: #909399; font-size: 12px; margin-left: 12px; }
.crl-block { margin-top: 16px; border-top: 1px dashed #e4e7ed; padding-top: 12px; }
.crl-title { font-weight: 600; margin-bottom: 8px; }
</style>