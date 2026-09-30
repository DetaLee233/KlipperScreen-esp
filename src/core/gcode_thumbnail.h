#pragma once
/*
 * G-code 缩略图获取（两条路径，由 BSP_HAS_GCODE_THUMB 门控）：
 * - Linux 上位机：本地解析 PrusaSlicer/OrcaSlicer/Cura 的
 *   "; thumbnail begin WxH len" base64 PNG 注释块（扫头/尾各一段）。
 * - ESP32（带 PSRAM）：经 Moonraker HTTP 异步拉取——先
 *   /server/files/metadata 拿 Moonraker 已提取 PNG 的 relative_path，
 *   再下载该小图（不硬编码 .thumbs/ 目录，避免整读大 gcode）。
 * 其余平台一律返回失败/NULL。
 */
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* gcode_name（相对 gcodes 根的路径）→ 本地完整路径。
 * 根目录：$KLIPPER_GCODES_DIR 优先，否则 $HOME/printer_data/gcodes。 */
bool gcode_thumbnail_resolve(const char *gcode_name, char *out, size_t cap);

/* 从 gcode 文件提取缩略图 PNG。成功返回 true：*png_out 为 malloc 的
 * PNG 字节流（调用方负责 free），*png_len 为其长度。 */
bool gcode_thumbnail_load(const char *gcode_path, unsigned char **png_out, size_t *png_len);

/* ESP32 / Linux 远程回退：经 Moonraker HTTP 异步拉取。
 * cb 保证恰好调用一次且总在 LVGL 上下文：
 * 成功 png!=NULL（**所有权移交接收方**，负责 free 或接管），失败 png==NULL。
 * 返回 false = 任务未能启动（cb 不会被调）。 */
bool gcode_thumbnail_fetch_async(const char *gcode_name,
                                 void (*cb)(unsigned char *png, size_t len, void *ud),
                                 void *ud);

#ifdef __cplusplus
}
#endif
