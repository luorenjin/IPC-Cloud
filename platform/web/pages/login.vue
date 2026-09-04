<script setup lang="ts">
// 登录页（AUTH-01）：账号密码登录，支持"记住我"延长会话
definePageMeta({ layout: 'auth' })
const { login } = useAuth()

const username = ref('')
const password = ref('')
const remember = ref(false)
const loading = ref(false)
const err = ref<{ code: string; msg: string; suggest?: string } | null>(null)

async function doLogin() {
  err.value = null
  if (!username.value || !password.value) {
    err.value = { code: '', msg: '请输入用户名和密码' }
    return
  }
  loading.value = true
  try {
    await login(username.value, password.value, remember.value)
    navigateTo('/')
  } catch (e: any) {
    // 结构化错误 {code, msg, suggest}；E4011 为账号锁定
    err.value = { code: e?.code || '', msg: e?.msg || '登录失败', suggest: e?.suggest }
  } finally {
    loading.value = false
  }
}
</script>

<template>
  <el-card class="login-card">
    <div class="title">IpcCloud 视频管理平台</div>
    <el-alert
      v-if="err" :title="err.msg" type="error" :closable="false" show-icon class="mb12"
      :description="err.code === 'E4011'
        ? (err.suggest || '账号已被锁定，请稍后重试或联系管理员')
        : err.suggest"
    />
    <el-form @submit.prevent="doLogin">
      <el-form-item>
        <el-input v-model="username" placeholder="用户名" size="large" @keyup.enter="doLogin" />
      </el-form-item>
      <el-form-item>
        <el-input
          v-model="password" type="password" placeholder="密码" size="large" show-password
          @keyup.enter="doLogin"
        />
      </el-form-item>
      <el-form-item>
        <el-checkbox v-model="remember">记住我</el-checkbox>
      </el-form-item>
      <el-button type="primary" size="large" style="width: 100%" :loading="loading" native-type="submit">
        登 录
      </el-button>
    </el-form>
  </el-card>
</template>

<style scoped>
.login-card { width: 380px; }
.title { font-size: 18px; font-weight: 700; text-align: center; margin-bottom: 20px; color: #303133; }
.mb12 { margin-bottom: 12px; }
</style>