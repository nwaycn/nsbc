'use strict';

const { contextBridge, ipcRenderer } = require('electron');

/**
 * 只向渲染进程暴露配置读写与「允许自签证书」开关。
 * 渲染进程本身不接触 Node / fs。
 */
contextBridge.exposeInMainWorld('nsbc', {
  /** 读取配置：{ config, defaults } */
  loadConfig: () => ipcRenderer.invoke('config:load'),
  /** 保存配置（写入 userData/config.json），返回规范化后的配置 */
  saveConfig: (config) => ipcRenderer.invoke('config:save', config),
  /** 实时切换证书校验宽松模式 */
  setAllowSelfSigned: (allow) => ipcRenderer.invoke('cert:set-allow-self-signed', allow)
});
