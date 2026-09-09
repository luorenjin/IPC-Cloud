<script setup lang="ts">
// 登录页（ACC-01）：严格对齐图 1（账号登录 / 扫码登录切换、居中卡片、记住账号、免注册体验）
definePageMeta({ layout: 'auth' })
const { login } = useAuth()
const route = useRoute()

const mode = ref<'account' | 'qr'>('account')
const username = ref('admin')
const password = ref('Admin@12345')
const remember = ref(true)
const loading = ref(false)
const showPwd = ref(false)
const err = ref<{ code: string; msg: string; suggest?: string } | null>(null)

async function doLogin() {
  err.value = null
  if (!username.value || !password.value) {
    err.value = { code: '', msg: '请输入账号和密码' }
    return
  }
  loading.value = true
  try {
    await login(username.value, password.value, remember.value)
    // 登录后优先进入控制台企业/项目选择页，或指定重定向
    const redirect = typeof route.query.redirect === 'string' ? route.query.redirect : '/console'
    navigateTo(redirect)
  } catch (e: any) {
    err.value = { code: e?.code || '', msg: e?.msg || '登录失败，请检查账号密码', suggest: e?.suggest }
  } finally {
    loading.value = false
  }
}
</script>

<template>
  <div class="relative w-[400px] max-w-[calc(100vw-32px)] rounded-signal border border-line bg-surface p-10 shadow-pop">
    <!-- 右上角扫码/账号登录切换 -->
    <button
      type="button"
      class="absolute right-4 top-4 flex h-8 w-8 items-center justify-center rounded-chrome text-muted transition-colors hover:bg-zone hover:text-primary"
      :title="mode === 'account' ? '切换为扫码登录' : '切换为账号登录'"
      @click="mode = mode === 'account' ? 'qr' : 'account'"
    >
      <Icon :name="mode === 'account' ? 'qr-code' : 'user'" :size="18" />
    </button>

    <!-- 标题：强调值守场景，而非平铺产品名 -->
    <div class="mb-8">
      <h2 class="text-lg font-semibold text-ink">开始值守</h2>
      <p class="mt-1 text-sm text-muted">登录 IpcCloud 视频管理平台</p>
    </div>

    <!-- 扫码登录模式 -->
    <div v-if="mode === 'qr'" class="flex flex-col items-center py-4">
      <div class="flex h-48 w-48 flex-col items-center justify-center rounded-signal border border-line bg-canvas p-2">
        <div class="mb-2 text-xs text-placeholder">使用 IpcCloud APP 扫码</div>
        <div class="flex h-36 w-36 items-center justify-center rounded-signal border border-line-soft bg-surface">
          <Icon name="qr-code" :size="96" class="text-muted" />
        </div>
      </div>
      <p class="mt-4 text-xs text-placeholder">打开手机客户端扫一扫即可登录</p>
      <button class="mt-4 text-xs text-primary hover:underline" @click="mode = 'account'">返回账号密码登录</button>
    </div>

    <!-- 账号密码登录模式 -->
    <div v-else>
      <div v-if="err" class="mb-4 flex items-start gap-2 rounded-chrome border border-danger/30 bg-danger-soft px-3 py-2 text-xs text-danger">
        <Icon name="alert-circle" :size="15" class="mt-0.5 shrink-0" />
        <div class="min-w-0">
          <p><span v-if="err.code" class="mr-1 font-mono font-bold">{{ err.code }}</span>{{ err.msg }}</p>
          <p v-if="err.suggest" class="mt-0.5 opacity-85">{{ err.suggest }}</p>
        </div>
      </div>

      <form class="space-y-4" @submit.prevent="doLogin">
        <div>
          <div class="relative flex h-10 items-center rounded-chrome border border-line bg-canvas px-3 transition-colors focus-within:border-primary focus-within:ring-1 focus-within:ring-primary/25">
            <input
              v-model="username"
              type="text"
              class="w-full bg-transparent text-sm text-ink outline-none placeholder:text-placeholder"
              placeholder="请输入手机号 / 用户名"
              autocomplete="username"
            />
          </div>
        </div>

        <div>
          <div class="relative flex h-10 items-center rounded-chrome border border-line bg-canvas px-3 transition-colors focus-within:border-primary focus-within:ring-1 focus-within:ring-primary/25">
            <input
              v-model="password"
              :type="showPwd ? 'text' : 'password'"
              class="w-full bg-transparent text-sm text-ink outline-none placeholder:text-placeholder"
              placeholder="请输入密码"
              autocomplete="current-password"
            />
            <button type="button" class="ml-2 text-placeholder hover:text-body" @click="showPwd = !showPwd">
              <Icon :name="showPwd ? 'eye-off' : 'eye'" :size="16" />
            </button>
          </div>
        </div>

        <div class="flex items-center justify-between pt-1 text-xs">
          <UiCheckbox v-model="remember" label="记住账号" />
          <button type="button" class="flex items-center gap-0.5 text-placeholder hover:text-primary" @click="err = { code: '', msg: '请联系管理员重置初始密码' }">
            忘记密码 <Icon name="help-circle" :size="12" />
          </button>
        </div>

        <UiButton variant="primary" size="lg" block :disabled="loading" @click="doLogin">
          <Icon v-if="loading" name="refresh" :size="14" class="ipc-spin" />{{ loading ? '登录中…' : '登录' }}
        </UiButton>
      </form>
    </div>
  </div>
</template>
