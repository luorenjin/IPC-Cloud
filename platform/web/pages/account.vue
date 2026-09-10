<script setup lang="ts">
// 个人中心（ACC-09）：修改密码 / 个人信息 / 界面语言切换
const api = useApi()
const toast = useToast()
const { user, currentProject, loadMe } = useAuth()
const { locale, t, setLocale } = useI18n()

/* ---------------- 修改密码 ---------------- */
const pwdForm = reactive({ oldPassword: '', newPassword: '', confirm: '' })
const pwdSaving = ref(false)

function pwdRuleOk(p: string) {
  return p.length >= 8 && /[A-Za-z]/.test(p) && /[0-9]/.test(p)
}

async function changePwd() {
  if (!pwdForm.oldPassword) return toast.warning(t('account.msg.pwdOldRequired'))
  if (!pwdRuleOk(pwdForm.newPassword)) return toast.warning(t('account.msg.pwdRuleFailed'))
  if (pwdForm.newPassword !== pwdForm.confirm) return toast.warning(t('account.msg.pwdMismatch'))
  if (pwdForm.newPassword === pwdForm.oldPassword) return toast.warning(t('account.msg.pwdSameAsOld'))
  pwdSaving.value = true
  try {
    await api.put('/me/password', { oldPassword: pwdForm.oldPassword, newPassword: pwdForm.newPassword })
    toast.success(t('account.msg.pwdChanged'))
    pwdForm.oldPassword = ''
    pwdForm.newPassword = ''
    pwdForm.confirm = ''
  } catch (e: any) {
    toastApiError(e, t('account.msg.pwdChangeFailed'))
  } finally {
    pwdSaving.value = false
  }
}

/* ---------------- 个人信息 ---------------- */
const infoForm = reactive({ name: '' })
const infoSaving = ref(false)

watch(user, (u) => { infoForm.name = u?.name || '' }, { immediate: true })

async function saveInfo() {
  if (!infoForm.name.trim()) return toast.warning(t('account.msg.nameRequired'))
  infoSaving.value = true
  try {
    await api.put('/me', { name: infoForm.name.trim(), contact: user.value?.contact || '' })
    toast.success(t('account.msg.infoSaved'))
    await loadMe()
  } catch (e: any) {
    toastApiError(e, t('common.saveFailed'))
  } finally {
    infoSaving.value = false
  }
}

/* ---------------- 界面语言 ---------------- */
function onLangChange(l: any) {
  setLocale(l as any)
  toast.success(l === 'zh-CN' ? t('account.msg.langZhSwitched') : t('account.msg.langEnSwitched'))
}

onMounted(async () => {
  if (!user.value) await loadMe()
})
</script>

<template>
  <div class="mx-auto max-w-2xl space-y-3">
    <!-- 个人信息 -->
    <UiCard :title="t('account.profile.infoTitle')" flat>
      <div class="grid grid-cols-[100px_1fr] items-center gap-x-3 gap-y-3">
        <span class="text-right text-sm text-body">{{ t('account.profile.username') }}</span>
        <UiInput :model-value="user?.username || ''" disabled />
        <span class="text-right text-sm text-body"><span class="text-danger">*</span>{{ t('account.profile.name') }}</span>
        <UiInput v-model="infoForm.name" :placeholder="t('account.profile.namePlaceholder')" :maxlength="50" @enter="saveInfo" />
        <span class="text-right text-sm text-body">{{ t('account.profile.contact') }}</span>
        <UiInput :model-value="user?.contact || ''" disabled />
        <span class="text-right text-sm text-body">{{ t('account.profile.accountStatus') }}</span>
        <div>
          <UiTag :color="user?.status === 'active' ? 'success' : 'danger'" dot>
            {{ user?.status === 'active' ? t('account.profile.statusActive') : t('account.profile.statusDisabled') }}
          </UiTag>
        </div>
        <span class="text-right text-sm text-body">{{ t('account.profile.currentProject') }}</span>
        <span class="text-sm text-body">{{ currentProject?.name || '—' }}</span>
      </div>
      <!-- 表单操作区独立于字段网格，与设置页footer 风格保持一致 -->
      <div class="mt-3 flex justify-end border-t border-line-soft pt-3">
        <UiButton variant="primary" :disabled="infoSaving" @click="saveInfo">{{ infoSaving ? t('common.saving') : t('account.profile.saveName') }}</UiButton>
      </div>
    </UiCard>

    <!-- 修改密码 -->
    <UiCard :title="t('account.profile.pwdTitle')" flat>
      <div class="grid grid-cols-[100px_1fr] items-center gap-x-3 gap-y-3">
        <span class="text-right text-sm text-body"><span class="text-danger">*</span>{{ t('account.profile.oldPassword') }}</span>
        <UiInput v-model="pwdForm.oldPassword" type="password" :placeholder="t('account.profile.oldPasswordPlaceholder')" :maxlength="64" />
        <span class="text-right text-sm text-body"><span class="text-danger">*</span>{{ t('account.profile.newPassword') }}</span>
        <UiInput v-model="pwdForm.newPassword" type="password" :placeholder="t('account.profile.newPasswordPlaceholder')" :maxlength="64" />
        <span class="text-right text-sm text-body"><span class="text-danger">*</span>{{ t('account.profile.confirmPassword') }}</span>
        <UiInput v-model="pwdForm.confirm" type="password" :placeholder="t('account.profile.confirmPasswordPlaceholder')" :maxlength="64" @enter="changePwd" />
      </div>
      <div class="mt-3 flex items-center justify-between gap-3 border-t border-line-soft pt-3">
        <p class="text-xs text-placeholder">{{ t('account.profile.pwdRuleHint') }}</p>
        <UiButton variant="primary" class="shrink-0" :disabled="pwdSaving" @click="changePwd">{{ pwdSaving ? t('account.profile.submitting') : t('account.profile.submitPwd') }}</UiButton>
      </div>
    </UiCard>

    <!-- 界面语言 -->
    <UiCard :title="t('user.language')" flat>
      <div class="flex items-center gap-3">
        <UiSegmented
          :model-value="locale"
          :items="[{ label: t('account.profile.langZh'), value: 'zh-CN' }, { label: t('account.profile.langEn'), value: 'en' }]"
          @update:model-value="onLangChange"
        />
        <span class="text-xs text-placeholder">{{ t('account.profile.langHint') }}</span>
      </div>
    </UiCard>
  </div>
</template>
