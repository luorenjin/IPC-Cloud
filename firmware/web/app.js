'use strict';
/**
 * IpcCloud 控制台 · 入口（只负责启动）
 * 模块：js/core.js → js/ui.js → js/router.js → js/views/*
 */
(function (IPC) {
  if (!IPC || !IPC.boot) {
    console.error('[app] 核心模块未加载');
    return;
  }
  IPC.boot();
})(window.IPC);
