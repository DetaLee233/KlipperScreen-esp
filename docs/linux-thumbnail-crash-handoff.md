# Linux 上位机缩略图崩溃排查交接（内部文档，不入 mkdocs nav）

> 2026-09-27，Kimi → 接手方。所有改动**未提交 git**，在工作区里。

## 2026-09-27 Codex 接手结论

红米2上的两条崩溃链已经分别定位并修复：

1. `gcode_thumbnail.c` 的 POSIX 下载线程直接调用 `lv_async_call()`，但桌面端配置是
   `LV_OS_NONE`。该调用会直接修改 LVGL timer 链表，与主线程的
   `lv_timer_handler()` 并发后会破坏链表，随机表现为 SIGSEGV 或 `lv_free`
   断言后的 SIGABRT。修复为与 Moonraker 投递相同：在调用前后持
   `bsp_lvgl_lock()`。
2. 本地和远程缩略图销毁时释放了 `lv_image_dsc_t` 与图片字节，却没有先调用
   `lv_image_cache_drop()`。LVGL 9.3 的图片缓存以 descriptor 指针为 key，原实现
   会留下悬空 key。两个 DELETE 回调现已在释放资源前主动 drop cache。
3. 打开 core dump 后抓到第二条确定栈（core PID 324889）：
   `sdl_event_handler -> lv_deinit -> lv_display_delete -> release_disp_cb -> libSDL2`
   SIGSEGV。LVGL 9.3 SDL 驱动处理 `SDL_QUIT` 时先执行 `SDL_Quit()`，随后
   `lv_deinit()` 的显示析构器又调用 `SDL_DestroyRenderer/Window`，顺序错误。
   Linux 全屏 kiosk 现于 SDL event filter 丢弃 compositor 的 QUIT/CLOSE 请求；
   同时在 `SDL_Init()` 前设置 `SDL_HINT_NO_SIGNAL_HANDLERS=1`，使真正的 systemd
   SIGTERM 仍按正常进程信号退出，不会被误吞为 SDL_QUIT。

验证证据：修复版 ARM64 真机包已部署红米2；当前连接的 .216 正在打印中文名
G-code，启动会自动走 `panel_job_status` 的远程缩略图路径。修复后多轮冷启动均已
越过旧版 12～33 秒崩溃窗口，单实例静置超过 2 分钟 PID 保持不变。快速连续
restart 偶尔会让 Weston 来不及释放 DRM 而首启失败，这是测试脚本制造的服务启动
竞争（status=1），不是应用 SIGSEGV/SIGABRT。

## 接手时现场（历史记录）

**红米2（Linux arm64 上位机）上程序仍间歇 SIGSEGV(139)/SIGABRT(134) 崩溃**，systemd 自动拉起（restart counter 递增）。
触发条件不完全确定：用户报告"点进 gcode 文件详情后按返回崩溃"；但也观察到启动后约 15 秒无人操作自行崩溃（17:53:49 那次）。
已排除的：投递双重释放（见下，已修）。**还没拿到 backtrace**——gdb 刚装好，第一次 attach 被用户打断。

### 当时计划的下一步（已完成）

在红米2 上抓 backtrace：

```bash
plink -batch -pw 1234 umeko@192.168.31.154
# 红米2 上：
PID=$(pgrep -f 'bin/KlipperScreen-esp' | head -1)
echo 1234 | sudo -S gdb -p $PID -batch \
  -ex 'handle SIGPIPE nostop noprint pass' \
  -ex continue -ex 'bt 30' -ex detach -ex quit 2>&1 | tail -40
```

该命令会阻塞直到崩溃（SIGSEGV/SIGABRT 停下并打 bt）。若迟迟不崩，让用户在屏幕上点"文件→详情→返回"复现。**注意：attach 期间进程是停的，界面会冻结，属正常；若中途打断 SSH，必须 `sudo pkill -9 gdb && sudo systemctl restart KlipperScreen-esp`**，否则应用会一直停在 stopped 状态。

## 这轮的完整背景

用户四个需求 + 两个顺手发现的 bug，全部改完未提交：

1. **缩略图自适应缩放**：`src/ui/widgets/gcode_thumb.c` 的 `fit_scale()`，等比适配 max_w/max_h，放大上限 4x。
2. **超长文件名滚动**：`panel_files.c` 文件名、`titlebar.c` 标题、`theme.c` 各行 key/value 标签全部限宽 + `LV_LABEL_LONG_SCROLL_CIRCULAR`（value 列是 DOT 右对齐）。
3. **ESP32 PSRAM 机型灰度缩略图**：`BSP_HAS_GCODE_THUMB` 宏（`src/bsp/bsp_caps.h`，Linux=1；ESP32 需 CONFIG_SPIRAM+CONFIG_LV_USE_LODEPNG）；8 个 PSRAM 板型的 sdkconfig.defaults + 已生成 sdkconfig 都加了 `CONFIG_LV_USE_LODEPNG=y`。ESP32 路径：`gcode_thumbnail.c` 里 esp_http_client 异步拉取 → `gcode_thumb.c` lodepng_decode32 → 灰度公式 `(77r+150g+29b)>>8` → RGB565。**没上过真机**。
4. **法语长文本遮挡**：同 2 的限宽滚动。

Bug 修复（都已验证/部署）：

5. **POSIX `reconnect_requested` 不清零**（`moonraker_client_posix.c` worker_main connect 成功后清零）——之前切打印机槽位后热循环断连（"216 显示未连接"的根因）。
6. **POSIX `http_get` recv 循环漏 `used += got`**（`gcode_thumbnail.c` ~292 行）——远程缩略图此前从未成功过；ring_PETG 能显示纯粹因为红米2 本地 `~/printer_data/gcodes/` 碰巧有该文件走了本地解析。
7. **投递双重释放**：`thumb_deliver()`（core 层）在 cb 之后又 `free(job->png)`，而 cb（`async_thumb_deliver`，widget 层）也会 free 或接管该指针。已修：core 层只 free job 本体，png 所有权移交 cb（头文件注释本来就是这么写的）。**修完后崩溃仍在**——说明还有第二个崩溃点，很可能也在"远程缩略图首次真正跑通"才暴露的路径里（POSIX 投递路径此前从未执行过）。

### 重点怀疑对象（第二个崩溃点）

- `src/ui/widgets/gcode_thumb.c` 的 `async_thumb_t` 所有权：`img` 的 DELETE 回调与 `async_thumb_deliver` 的"后到者释放"逻辑。后续核对 LVGL 9.3 源码确认 `lv_obj_delete()` 会同步发送 `LV_EVENT_DELETE`，因此 fetch 启动失败分支不是此前怀疑的延迟删除 UAF。
- LVGL 9.3 image cache 以 src 指针（`&t->dsc`）为 key，t 释放后 cache 留有悬垂 key；新分配复用同地址会错命中旧图（显示错乱）——理论分析不太可能 segfault，但没实测排除。
- `panel_job_status.c`：如果 216 正在打印，主界面/打印状态页会**自动**加载缩略图，能解释"无人操作 15 秒后自崩"。
- 也不排除崩溃根本不在缩略图，而在这批 titlebar/theme 的 SCROLL_CIRCULAR 改动（每秒都在跑）。

### 缩略图链路关键事实（curl 实测过）

- .92 和 .216 两台 Moonraker 的缩略图都在 **`.thumbs/`**（不是 `.thumbnails/`），最大仅 128x128（.216）或 50x50（.92）。实现用 metadata 返回的 `relative_path`，不硬编码目录名，两边都通。
- `/server/files/gcodes/.thumbs/xxx.png` 隐藏路径 Moonraker 正常 200。
- 中文文件名 url_encode 按 UTF-8 字节百分号编码，正确。
- 串口看 Metadata 返回 JSON 在 Windows Git Bash 里中文是乱码，那只是控制台显示问题。

## 环境与操作手册

- **红米4** = 192.168.31.92（也是编译机，arm64 源码树 `~/kr-src`）；**红米2** = 192.168.31.154（被测机）。均 umeko/1234。
- Windows 本机用 `plink -batch -pw 1234 umeko@<ip> "..."` / `pscp -batch -pw 1234 ...`。
- **增量编译**（推荐，改单文件时）：`pscp` 单文件到 `红米4:/home/umeko/kr-src/src/...`，然后
  `plink ... "cd /home/umeko/kr-src/src/ports/desktop/build && cmake --build . -j4 --target KlipperScreen-esp"`。
  全量同步：`tar -czf tmp/src-sync.tar.gz --exclude='src/ports/desktop/build*' --exclude='*.conf' src/ui src/core src/bsp src/ports/desktop`（约 21MB）。
- **部署大坑（已写进 AGENTS.md）**：build 目录里 `KlipperScreen-esp`（真机）和 `klipper_remote_simulator`（mock 假数据）并存。**必须拷 `KlipperScreen-esp`**；误拷 simulator 表现为：文件列表是假文件、无缩略图、不连 Moonraker。这次已踩过一次。
- 红米4→红米2 之间 scp 没配 key，要经 Windows 中转（pscp 拉回 tmp/ 再推过去，25MB 约 1 分钟）。
- 部署命令：
  ```bash
  plink -batch -pw 1234 umeko@192.168.31.154 "echo 1234 | sudo -S systemctl stop KlipperScreen-esp; sleep 1; cp /tmp/ksesp-real ~/.local/share/KlipperScreen-esp/bin/KlipperScreen-esp && chmod +x ~/.local/share/KlipperScreen-esp/bin/KlipperScreen-esp && echo 1234 | sudo -S systemctl start KlipperScreen-esp"
  ```
- 日志：红米上 `~/printer_data/logs/KlipperScreen-esp.log`（weston 启动日志也混在里面，70k+ 行）；崩溃记录 `sudo journalctl -u KlipperScreen-esp -n 20 --no-pager`（134=SIGABRT 139=SIGSEGV）。
- gdb 已在红米2 装好（apt 直连源可用，无需代理）。
- 服务环境：weston kiosk + wayland-0，`XDG_RUNTIME_DIR=/run/user/1000`，启动脚本 `~/.local/share/KlipperScreen-esp/KlipperScreen-esp-start.sh`。
- GitHub 走代理 `curl --proxy http://127.0.0.1:8635`；git push 走 git config 里的 8635。
- 根目录 `moonraker.conf` 永不提交（git status 里的 untracked 是它）。

## 验收/收尾标准

- 红米2：真实文件列表 + 远程缩略图显示 + 点返回不崩 + 静置 2 分钟不自崩。
- 然后：`git diff --check`、Windows desktop 编译（`bash tools/build-desktop.sh`）、一个 ESP32 PSRAM 板型编译验证，提交推送。是否发 v0.6.2 问用户。
- ESP32 灰度缩略图真机验证还没做（等用户插 PSRAM 板子）。
