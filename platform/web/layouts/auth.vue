<script setup lang="ts">
// 登录/向导等独立页布局
</script>

<template>
  <div class="auth-wrap">
    <slot />
  </div>
</template>

<style scoped>
.auth-wrap {
  min-height: 100vh; display: flex; align-items: center; justify-content: center;
  background: linear-gradient(135deg, #1d2129 0%, #2b3a55 100%);
}
</style>
