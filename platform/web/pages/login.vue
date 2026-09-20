<script setup lang="ts">
// 登录页（ACC-01）：账号密码登录、居中卡片、记住账号
definePageMeta({ layout: 'auth' })
const { login } = useAuth()
const route = useRoute()
const { t } = useI18n()

const username = ref('')
const password = ref('')
const remember = ref(true)
const loading = ref(false)
const showPwd = ref(false)
const err = ref<{ code: string; msg: string; suggest?: string } | null>(null)

async function doLogin() {
  err.value = null
  if (!username.value || !password.value) {
    err.value = { code: '', msg: t('account.login.emptyFields') }
    return
  }
  loading.value = true
  try {
    await login(username.value, password.value, remember.value)
    // 登录后优先进入控制台企业/项目选择页，或指定重定向
    const redirect = typeof route.query.redirect === 'string' ? route.query.redirect : '/console'
    navigateTo(redirect)
  } catch (e: any) {
    err.value = { code: e?.code || '', msg: e?.msg || t('account.login.failed'), suggest: e?.suggest }
  } finally {
    loading.value = false
  }
}
</script>

<template>
  <div class="relative w-[400px] max-w-[calc(100vw-32px)] rounded-signal border border-line bg-surface p-10 shadow-pop">
    <!-- 标题：强调值守场景，而非平铺产品名 -->
    <div class="mb-8">
      <h2 class="text-lg font-semibold text-ink">{{ t('account.login.title') }}</h2>
      <p class="mt-1 text-sm text-muted">{{ t('account.login.subtitle') }}</p>
    </div>

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
            class="ipc-focus-none w-full bg-transparent text-sm text-ink outline-none placeholder:text-placeholder"
            :placeholder="t('account.login.usernamePlaceholder')"
            autocomplete="username"
          />
        </div>
      </div>

      <div>
        <div class="relative flex h-10 items-center rounded-chrome border border-line bg-canvas px-3 transition-colors focus-within:border-primary focus-within:ring-1 focus-within:ring-primary/25">
          <input
            v-model="password"
            :type="showPwd ? 'text' : 'password'"
            class="ipc-focus-none w-full bg-transparent text-sm text-ink outline-none placeholder:text-placeholder"
            :placeholder="t('account.login.passwordPlaceholder')"
            autocomplete="current-password"
          />
          <button type="button" class="ml-2 rounded-chrome text-placeholder hover:text-body" :aria-label="showPwd ? t('account.login.hidePassword') : t('account.login.showPassword')" @click="showPwd = !showPwd">
            <Icon :name="showPwd ? 'eye-off' : 'eye'" :size="16" />
          </button>
        </div>
      </div>

      <div class="flex items-center justify-between pt-1 text-xs">
        <UiCheckbox v-model="remember" :label="t('account.login.remember')" />
        <span class="text-placeholder">{{ t('account.login.forgotHint') }}</span>
      </div>

      <UiButton variant="primary" size="lg" block :disabled="loading" @click="doLogin">
        <Icon v-if="loading" name="refresh" :size="14" class="ipc-spin" />{{ loading ? t('account.login.submitting') : t('account.login.submit') }}
      </UiButton>
    </form>
  </div>
</template>
