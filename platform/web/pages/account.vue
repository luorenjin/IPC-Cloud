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
  if (!pwdForm.oldPassword) return toast.warning('请输入当前密码')
  if (!pwdRuleOk(pwdForm.newPassword)) return toast.warning('新密码至少 8 位且包含字母与数字')
  if (pwdForm.newPassword !== pwdForm.confirm) return toast.warning('两次输入的新密码不一致')
  if (pwdForm.newPassword === pwdForm.oldPassword) return toast.warning('新密码不能与当前密码相同')
  pwdSaving.value = true
  try {
    await api.put('/me/password', { oldPassword: pwdForm.oldPassword, newPassword: pwdForm.newPassword })
    toast.success('密码已修改，下次登录请使用新密码')
    pwdForm.oldPassword = ''
    pwdForm.newPassword = ''
    pwdForm.confirm = ''
  } catch (e: any) {
    toastApiError(e, '修改密码失败')
  } finally {
    pwdSaving.value = false
  }
}

/* ---------------- 个人信息 ---------------- */
const infoForm = reactive({ name: '' })
const infoSaving = ref(false)

watch(user, (u) => { infoForm.name = u?.name || '' }, { immediate: true })

async function saveInfo() {
  if (!infoForm.name.trim()) return toast.warning('请输入姓名')
  infoSaving.value = true
  try {
    await api.put('/me', { name: infoForm.name.trim(), contact: user.value?.contact || '' })
    toast.success('个人信息已保存')
    await loadMe()
  } catch (e: any) {
    toastApiError(e, '保存失败')
  } finally {
    infoSaving.value = false
  }
}

/* ---------------- 界面语言 ---------------- */
function onLangChange(l: any) {
  setLocale(l as any)
  toast.success(l === 'zh-CN' ? '已切换为中文' : 'Switched to English')
}

onMounted(async () => {
  if (!user.value) await loadMe()
})
</script>

<template>
  <div class="mx-auto max-w-2xl space-y-3">
    <!-- 个人信息 -->
    <UiCard title="个人信息" flat>
      <div class="grid grid-cols-[100px_1fr] items-center gap-x-3 gap-y-3">
        <span class="text-right text-sm text-body">用户名</span>
        <UiInput :model-value="user?.username || ''" disabled />
        <span class="text-right text-sm text-body"><span class="text-danger">*</span>姓名</span>
        <UiInput v-model="infoForm.name" placeholder="姓名" :maxlength="50" @enter="saveInfo" />
        <span class="text-right text-sm text-body">手机/邮箱</span>
        <UiInput :model-value="user?.contact || ''" disabled />
        <span class="text-right text-sm text-body">账号状态</span>
        <div>
          <UiTag :color="user?.status === 'active' ? 'success' : 'danger'" dot>
            {{ user?.status === 'active' ? '正常' : '已停用' }}
          </UiTag>
        </div>
        <span class="text-right text-sm text-body">当前项目</span>
        <span class="text-sm text-body">{{ currentProject?.name || '—' }}</span>
      </div>
      <!-- 表单操作区独立于字段网格，与设置页footer 风格保持一致 -->
      <div class="mt-3 flex justify-end border-t border-line-soft pt-3">
        <UiButton variant="primary" :disabled="infoSaving" @click="saveInfo">{{ infoSaving ? '保存中…' : '保存姓名' }}</UiButton>
      </div>
    </UiCard>

    <!-- 修改密码 -->
    <UiCard title="修改密码" flat>
      <div class="grid grid-cols-[100px_1fr] items-center gap-x-3 gap-y-3">
        <span class="text-right text-sm text-body"><span class="text-danger">*</span>当前密码</span>
        <UiInput v-model="pwdForm.oldPassword" type="password" placeholder="请输入当前登录密码" :maxlength="64" />
        <span class="text-right text-sm text-body"><span class="text-danger">*</span>新密码</span>
        <UiInput v-model="pwdForm.newPassword" type="password" placeholder="至少 8 位，包含字母与数字" :maxlength="64" />
        <span class="text-right text-sm text-body"><span class="text-danger">*</span>确认密码</span>
        <UiInput v-model="pwdForm.confirm" type="password" placeholder="再次输入新密码" :maxlength="64" @enter="changePwd" />
      </div>
      <div class="mt-3 flex items-center justify-between gap-3 border-t border-line-soft pt-3">
        <p class="text-xs text-placeholder">密码需至少 8 位且同时包含字母与数字；修改成功后其他设备的登录会话仍保持有效。</p>
        <UiButton variant="primary" class="shrink-0" :disabled="pwdSaving" @click="changePwd">{{ pwdSaving ? '提交中…' : '修改密码' }}</UiButton>
      </div>
    </UiCard>

    <!-- 界面语言 -->
    <UiCard :title="t('user.language')" flat>
      <div class="flex items-center gap-3">
        <UiSegmented
          :model-value="locale"
          :items="[{ label: '中文', value: 'zh-CN' }, { label: 'English', value: 'en' }]"
          @update:model-value="onLangChange"
        />
        <span class="text-xs text-placeholder">切换后立即生效（侧栏与顶栏菜单同步翻译）。</span>
      </div>
    </UiCard>
  </div>
</template>
