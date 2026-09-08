<script setup lang="ts">
// 登录页（ACC-01）：严格对齐图 1（账号登录 / 扫码登录切换、居中卡片、记住账号、免注册体验）
definePageMeta({ layout: 'auth' })
const { login } = useAuth()
const route = useRoute()

const mode = ref<'account' | 'qr'>('account')
const username = ref('admin')
const password = ref('Admin123456')
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
  <div class="relative w-[440px] max-w-[calc(100vw-32px)] rounded-xl bg-white p-10 shadow-2xl">
    <!-- 右上角扫码/账号登录切换图标（对齐图 1） -->
    <div class="absolute right-3 top-3 cursor-pointer p-2" :title="mode === 'account' ? '切换为扫码登录' : '切换为账号登录'" @click="mode = mode === 'account' ? 'qr' : 'account'">
      <div class="flex h-9 w-9 items-center justify-center rounded bg-[#ebf5ff] text-[#1785E6] hover:bg-[#d6ebff] transition-colors">
        <Icon :name="mode === 'account' ? 'qr-code' : 'user'" :size="20" />
      </div>
    </div>

    <!-- 卡片标题 -->
    <div class="mb-8 text-center">
      <h2 class="text-xl font-bold tracking-tight text-[#1f2329]">IpcCloud 视频管理平台</h2>
    </div>

    <!-- 扫码登录模式 -->
    <div v-if="mode === 'qr'" class="flex flex-col items-center py-4">
      <div class="h-48 w-48 rounded border border-line bg-surface p-2 shadow-inner flex flex-col items-center justify-center">
        <div class="text-xs text-placeholder mb-2">使用 IpcCloud APP 扫码</div>
        <div class="h-36 w-36 bg-gray-100 flex items-center justify-center border border-gray-200 rounded">
          <Icon name="qr-code" :size="96" class="text-gray-700" />
        </div>
      </div>
      <p class="mt-4 text-xs text-placeholder">打开手机客户端扫一扫即可登录</p>
      <button class="mt-4 text-xs text-[#1785E6] hover:underline" @click="mode = 'account'">返回账号密码登录</button>
    </div>

    <!-- 账号密码登录模式 -->
    <div v-else>
      <div v-if="err" class="mb-4 flex items-start gap-2 rounded border border-[#f7c8c4] bg-[#fef0ef] px-3 py-2 text-xs text-[#f53f3f]">
        <Icon name="alert-circle" :size="15" class="mt-0.5 shrink-0" />
        <div class="min-w-0">
          <p><span v-if="err.code" class="mr-1 font-mono font-bold">{{ err.code }}</span>{{ err.msg }}</p>
          <p v-if="err.suggest" class="mt-0.5 opacity-85">{{ err.suggest }}</p>
        </div>
      </div>

      <form class="space-y-4" @submit.prevent="doLogin">
        <div>
          <div class="relative flex h-10 items-center rounded border border-[#dcdfe6] px-3 focus-within:border-[#1785E6] focus-within:ring-1 focus-within:ring-[#1785E6] transition-all">
            <input
              v-model="username"
              type="text"
              class="w-full bg-transparent text-sm text-[#1f2329] outline-none placeholder:text-[#a8abb2]"
              placeholder="请输入手机号 / 用户名"
              autocomplete="username"
            />
          </div>
        </div>

        <div>
          <div class="relative flex h-10 items-center rounded border border-[#dcdfe6] px-3 focus-within:border-[#1785E6] focus-within:ring-1 focus-within:ring-[#1785E6] transition-all">
            <input
              v-model="password"
              :type="showPwd ? 'text' : 'password'"
              class="w-full bg-transparent text-sm text-[#1f2329] outline-none placeholder:text-[#a8abb2]"
              placeholder="请输入密码"
              autocomplete="current-password"
            />
            <button type="button" class="ml-2 text-[#909399] hover:text-[#606266]" @click="showPwd = !showPwd">
              <Icon :name="showPwd ? 'eye-off' : 'eye'" :size="16" />
            </button>
          </div>
        </div>

        <div class="flex items-center justify-between text-xs pt-1">
          <label class="flex cursor-pointer items-center gap-1.5 text-[#606266]">
            <input v-model="remember" type="checkbox" class="h-3.5 w-3.5 rounded border-gray-300 text-[#1785E6] focus:ring-[#1785E6]" />
            记住账号
          </label>
          <button type="button" class="flex items-center gap-0.5 text-[#909399] hover:text-[#1785E6]" @click="err = { code: '', msg: '请联系管理员重置初始密码' }">
            忘记密码 <Icon name="help-circle" :size="12" />
          </button>
        </div>

        <button
          type="button"
          :disabled="loading"
          class="flex h-10 w-full items-center justify-center rounded bg-[#1785E6] text-sm font-medium text-white transition-colors hover:bg-[#0a6bcc] active:bg-[#0854a0] disabled:opacity-50 shadow-sm"
          @click="doLogin"
        >
          <Icon v-if="loading" name="refresh" :size="14" class="mr-1.5 ipc-spin" />{{ loading ? '登录中…' : '登 录' }}
        </button>

        <div class="pt-2 text-center text-xs">
          <span class="text-[#909399]">还没有账号？</span>
          <button type="button" class="ml-1 text-[#1785E6] hover:underline" @click="err = { code: '', msg: '如需开通新租户账号，请联系商务支持' }">
            注册 IpcCloud ID
          </button>
        </div>
      </form>
    </div>

    <!-- 底部免注册体验提示（对齐图 1 底部文字） -->
    <div class="mt-8 border-t border-[#f2f3f5] pt-4 text-center text-xs text-[#86909c]">
      想先了解 IpcCloud 视频管理平台？
      <button type="button" class="font-medium text-[#1785E6] hover:underline" @click="username = 'demo'; password = 'Demo123456'; doLogin()">
        免费快速体验 &gt;
      </button>
    </div>
  </div>
</template>
