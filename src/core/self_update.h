#pragma once
/*
 * Linux 上位机自更新（仅 BSP_HAS_LINUX_HOST 有实现，其余平台不编译）：
 * 关于页"检查更新"→ 查询 GitHub latest release → 下载对应架构的
 * desktop-linux-<arch>.tar.gz（curl，自动继承 http_proxy/https_proxy 环境
 * 变量）→ md5 边车文件校验 → 用户确认后原地替换二进制。
 * 重启走 bsp_restart()（exit），由 systemd Restart=always 拉起新版本。
 * 全程 worker 线程干活，UI 经 supd_poll() 轮询快照，无 LVGL 跨线程访问。
 */
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SUPD_IDLE = 0,
    SUPD_CHECKING,      /* 查询 GitHub latest release 中 */
    SUPD_UPTODATE,      /* 已是最新 */
    SUPD_DOWNLOADING,   /* 有新版本，下载中：progress 千分比 0..1000 */
    SUPD_PAUSED,        /* 下载暂停（SIGSTOP curl，可续） */
    SUPD_VERIFYING,     /* 下载完毕，md5 校验中 */
    SUPD_READY,         /* 校验通过，可 supd_apply() */
    SUPD_ERROR,         /* error 里有简述 */
} supd_state_t;

typedef struct {
    supd_state_t state;
    char latest[24];    /* 远端最新版本号（不含 v 前缀），如 "0.6.3" */
    char error[64];
    int  progress;      /* DOWNLOADING 时 0..1000 */
} supd_status_t;

/* 发起"检查更新"：有新版本会直接接着下载+校验。忙碌中重复调用忽略。 */
void supd_check_start(void);
void supd_poll(supd_status_t *out);  /* 取状态快照（UI 节拍调用） */
void supd_progress_tick(void);       /* UI 节拍调用：推进下载进度 */
void supd_pause(void);   /* DOWNLOADING → PAUSED（SIGSTOP curl） */
void supd_resume(void);  /* PAUSED → DOWNLOADING（SIGCONT） */
void supd_cancel(void);  /* 杀 curl、删临时文件、回 IDLE（暂停/下载中可调） */

/* UI 展示用：检查更新的来源页面与本架构对应的下载包名 */
const char *supd_release_page(void);
const char *supd_asset_name(void);

/* READY 时调用：解压并原地替换当前二进制（同目录 .new + rename）。
 * 返回 true 后由 UI 提示并调 bsp_restart() 完成切换。 */
bool supd_apply(void);

#ifdef __cplusplus
}
#endif
