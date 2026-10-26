<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# 仪表盘 UI 预览

在**不烧录**的前提下把仪表盘的真实页面渲染成图片，用于评审布局与状态变化。

```bash
./tools/ui_preview/build.sh
```

输出在 `tools/ui_preview/build/`，每个状态一个 BMP。

## 为什么这不是"效果图"

这个工具编译的是固件自己的源码：

```text
main/app_dashboard/dashboard_ui.c        页面、卡片、指标、心跳条
main/app_dashboard/dashboard_health.c    阈值、驻留、升级、滞回
main/app_dashboard/dashboard_format.c    文本格式化
main/app_dashboard/dashboard_protocol.c  帧解码
```

只有设备侧被替身替换：共享快照、`bsp_battery_soc()`、`bsp_display_backlight()`、
`bsp_lvgl_lock()`。**布局没有任何一行被重新实现**，所以图不对就意味着固件不对。

时间按 1 秒步进推进，同时驱动 UI 自己的 200ms 定时器，因此驻留窗口、升级阶梯和滞回
的表现与真机一致。

## 会渲染哪些状态

| 图片 | 状态 |
| --- | --- |
| `live-1-status-ok` | 主机健康，`OK` 卡片配运行时长 |
| `live-2-detail` | DETAIL 页 |
| `live-3-agents` | AGENTS 页 |
| `live-4-status-crit-disk` | 磁盘越线，红色接管卡片 |
| `live-4b-status-reboot` | 状态行显示主机重启提示 |
| `live-5-status-silent` | 链路在、帧停了 |
| `live-6-status-nolink-warn` | 链路断开，琥珀阶段 |
| `live-7-status-nolink-crit` | 链路断开超过两分钟 |
| `live-8b-battery-warn` | 工牌电量 18%，琥珀提示 |
| `live-8-battery-crit` | 工牌电量 8%，`LOW BATT` 接管 |
| `live-9-battery-charging` | 充电抑制电池告警 |

## 依赖

- 主机 C 工具链（与静态门禁用的是同一个 `cc`）
- `managed_components/` 下的 LVGL 源码，由固件构建时拉取。目录不存在时先跑一次
  `./tools/validate.sh --firmware`。

本工具**不纳入** `./tools/validate.sh`：CI 上不存在 `managed_components`，而且它产出
的是给人看的图片而非通过/失败结论。必须防退化的逻辑放在 `tests/` 里。

## 局限

- **不模拟**面板 30px 圆角遮罩：真机四角会被裁成黑色，这里保持直角。
- 不模拟面板反色、色彩响应和真实 SPI 刷新率。
- 直接写 BMP，因此不依赖任何图像库。macOS 预览可直接打开；需要 PNG 可用
  `sips -s format png <文件>.bmp --out <文件>.png`。
