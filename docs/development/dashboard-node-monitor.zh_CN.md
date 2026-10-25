<p align="right">
  <strong>简体中文</strong> · <a href="dashboard-node-monitor.md">English</a>
</p>

# 节点监控仪表盘（固件设计）

把 AI Passport 用作一台长期在线的 Linux 开发节点（`dev-forge`）的物理状态面板的
设计。线上格式见 [`protocol/protocol.zh_CN.md`](../../protocol/protocol.zh_CN.md)；
本文只讲固件侧：代码布局、BLE 服务、RAM 预算、页面与失败状态。

## 范围

v1 包含：

- BLE peripheral，接受单个 central 每 5 秒一次的状态写入。
- 三个英文页面（主机健康、agent、网络）以及链路状态呈现。
- 主机冻结或重启后仍持续增长的“最后心跳距今时间”。

v1 不包含（与任务契约一致）：WiFi、工牌向主机下发命令、OTA、多主机、历史曲线。

## 代码布局

```text
main/app_dashboard/
  dashboard_protocol.h/.c   纯 C：解码 + 心跳距今 + 链路分级（有 host 测试）
  dashboard_ble.h/.c        NimBLE peripheral、GATT 服务、写入回调
  dashboard_state.h/.c      共享快照：最后一条状态、单调时钟接收时刻
  dashboard_ui.h/.c         三个页面、翻页、刷新定时器
  dashboard_app.h/.c        应用生命周期：初始化 BLE 与 UI、按键路由、退出清理
```

只有 `dashboard_protocol.c` 是“一旦退化就必须被发现”的逻辑；它不依赖 ESP-IDF 与
LVGL，由 `tests/test_dashboard_protocol.c` 覆盖。其余部分必须上真机验证。

## 启动路径

`app_main()` 直接启动仪表盘，而不是基线测试菜单。二次开发应用不得把上游 demo
外壳当作成品 UI（见 `AGENTS.md`），启动路径就是这一点的证据。

基线 `demo_*.c` 作为上游硬件参考保留在仓库中。在 `main/main.c` 用一个编译期开关：

```c
#define APP_BOOT_DEMO_MENU 0   // 1 = 启动上游 demo 菜单（仅供维护）
```

让参考菜单在板级调试时仍可进入，同时不把它变成产品界面。这里刻意用编译期开关：
运行期的“开机按住某键”逃生入口会与现有的休眠唤醒按键守卫相互干扰。

## BLE 服务

工牌是 peripheral，主机是 central（公司内网是 802.1X EAP-TLS，工牌无法认证，所以
WiFi 不可行）。

| 项目 | 值 |
| --- | --- |
| 设备名 | `PassportDash` |
| 广播内容 | flags + 完整本地名（17 字节） |
| 扫描响应 | 128 位服务 UUID（18 字节） |
| 服务 UUID | `1f7a0001-6b3c-4a1e-9c2d-5e8f0a9b4c10` |
| 状态特征 | `1f7a0002-6b3c-4a1e-9c2d-5e8f0a9b4c10`，`WRITE` + `READ` |
| 载荷 | 状态串，≤ 200 字节，一次 ATT 写入 |
| 连接数 | 1（`CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1`） |

名称与 UUID 分别放在广播与扫描响应里，因为两者合起来会超出 31 字节的传统广播载荷。

`READ` 返回最后一条被接受的状态串。它只为用 `bluetoothctl`/`gatttool` 调试而存在，
不引入写入路径之外的新状态。

### MTU 与分帧

载荷需要一次写入 200 字节，因此协商后的 ATT MTU 必须至少 203 字节。NimBLE 默认
首选 MTU 为 256，无需改配置即可满足；固件在连接建立时打印实际协商值，让这个假设
在真机上被核实而不是被假定。

v1 不做分块重组。若 central 写入残缺帧，解析器会拒绝它，工牌继续显示上一组数值并
持续增长心跳距今；下一条完整写入即恢复。这是刻意的：把残缺写入静默拼接起来，会
让一个看起来合法的帧被污染。

### 生命周期

写入回调运行在 NimBLE host 任务上，绝不能触碰 LVGL。它只在互斥量保护下把载荷复制
进共享快照后返回；LVGL 定时器在下一个 tick（200 ms）取用。这符合仓库规则：回调不得
阻塞、也不得直接访问 LVGL。

断开连接时快照保留最后心跳时间戳，但把所有测得数值标记为缺失，于是“距今”会跨越
主机重启继续增长（验收条件 A4），而数值显示“无数据”（A3）。

## RAM 预算

目标平台无 PSRAM，最大连续空闲块约 7.7 KB，因此设计为固定尺寸、初始化后不再分配。

| 占用方 | 约计 | 说明 |
| --- | --- | --- |
| LVGL 绘制缓冲 | 19.2 KB（固定） | `BSP_LVGL_DRAW_BUFFER_LINES` 40 × 240 px × 2 B，由 BSP 持有 |
| LVGL 内存池 | 24 KB（固定） | `CONFIG_LV_MEM_SIZE_KILOBYTES` |
| 状态快照 | ~90 B | 单个 `dashboard_status_t`，复用 |
| BLE 写入缓冲 | ≤ 200 B | 一次 `dashboard_status_t` 复制路径 |
| 三个页面 | ~2–3 KB LVGL 对象 | 只有文本控件，无图片 |
| NimBLE host 栈 | 任务栈，沿用 | 与 BLE demo 相同 |

实现约束：

- 应用代码不调用 `malloc`；所有状态静态分配。
- 不引入图片与新字体。当前仅启用 `lv_font_montserrat_14` 与
  `lv_font_montserrat_20`（见 `sdkconfig.defaults`）。
- 页面只创建一次并原地更新；不在每次刷新时创建/删除。
- 真机验证：在启动时与首次完整渲染后打印
  `heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)` 与总空闲堆。决定是否装得下的
  是这个实测数字，不是本表。

## 页面

240×320 竖屏、反色、30 px 圆角遮罩。内容保持在 12 px 内缩范围内并避开四角，避免遮罩
裁掉文字。

每个页面共用的外框：

- 顶行：链路点（绿/琥珀/红/灰）+ 心跳距今（`12s`、`3m 20s`、`NEVER`）+ 来自
  `bsp_battery_soc()` 的电量百分比（为 `-1` 时隐藏）。
- 底行：三个页面圆点，当前页填充。

第 1 页 —— HOST（`up`、`ld`、`cpu`、`mem`、`dsk`、`tmp`）：

```text
 dev-forge            87%
 UPTIME
 1d 11h 34m
 LOAD        CPU
 0.42        7%
 MEM  [####----] 38%
 DISK [###-----] 33%
 TEMP   51 C
```

第 2 页 —— AGENTS（`ag`、`as`）：

```text
 AGENTS
   2
 ACTIVE SESSIONS
 last activity
 5s ago
```

第 3 页 —— NETWORK（`net`）：

```text
 NETWORK
 eno1
 ONLINE / OFFLINE
 10.2.65.61
```

任何在最近一条载荷中缺失的字段都渲染为 `--`，绝不渲染为 `0`，这样缺失字段不会被
误当成真实测量值。

链路状态（取自 `protocol/protocol.zh_CN.md`）：`none` 显示 `NEVER` 且所有数值为 `--`；
`live` 正常绘制；`stale` 数值变暗并显示琥珀色点；`lost` 数值替换为 `--`、显示红点，
并保留距今行。

## 按键交互

三个按键经分压电阻共用 GPIO0；回调不得阻塞，只入队到现有的输入任务。

| 输入 | 动作 |
| --- | --- |
| UP 单击 | 上一页 |
| DOWN 单击 | 下一页 |
| OK 单击 | 强制重绘当前页 |
| OK 长按（1 秒） | 背光在 100 % 与 20 % 之间切换 |

v1 不使用深睡：会睡觉的面板不是状态面板。`main/key_guard.h` 中的休眠唤醒按键守卫
保持不动，因为启动路径的切换没有触碰它。

## 失败状态

| 情况 | 行为 |
| --- | --- |
| 显示/LVGL 初始化失败 | 打印引脚号后返回，与当前 `app_main` 一致 |
| NimBLE 初始化失败 | 界面仍渲染，链路点置灰，显示 `BLE FAILED` 行 |
| 从未收到状态 | `NEVER`，所有数值 `--`，灰点 |
| 载荷格式错误 | 拒绝该帧，保留上一组数值，距今继续增长 |
| 协议版本不支持 | 拒绝该帧；链路点转红以暴露版本不一致 |
| 电量计缺失 | 隐藏电量百分比，不留占位符 |
| 按键初始化失败 | 界面仍渲染与刷新，仅失去翻页能力 |

## 验证

主机（任意环境可跑）：

```bash
./tools/validate.sh --static
```

覆盖 `check_repo.py`、现有 BSP/demo host 测试，以及新增的
`tests/test_dashboard_protocol.c` 与 Python 生产端测试，全部回放共享用例文件
`protocol/fixtures/status_cases.txt`。

真机（需要工牌、ESP-IDF 5.5.3 以及明确的烧录授权）：

1. `./tools/validate.sh --firmware` 构建并校验合并镜像。
2. 启动：出现仪表盘；不得出现 demo 菜单。
3. 主机持续推送 60 秒：数值随主机变化，距今始终 ≤ 5 秒。
4. 停掉主机 3 分钟：数值变为 `--`，链路点转红，距今持续增长。
5. 重启主机：链路恢复，距今归零。
6. 给工牌断电重启：重新广播，主机重连成功。
7. 记录 RAM 预算章节要求的空闲堆数字。

## 上板清单（首次烧录）

前置条件：已激活 ESP-IDF 5.5.3、工牌用支持数据传输的 USB 线连接、且已获得烧录授权。

```bash
source ~/esp/esp-idf/export.sh
./tools/validate.sh --firmware
idf.py -p /dev/cu.usbmodem* flash monitor
```

`GPIO0` 是启动 strapping 脚，且被三个按键共用。板子复位或烧录时不要按住 UP、DOWN
或 OK：复位期间该脚为低可能让芯片进入下载模式而不运行应用。

先看启动日志里的四件事，再看屏幕：

1. 没有 panic 或重启循环。
2. `dash_ble: advertising as PassportDash`。
3. 连接时：`dash_ble: connected; ATT MTU <n> (need >= 203)`。这里出现告警意味着状态串
   需要换一种传输方式，而不是重试。
4. `dash_app: [boot]` 与 `dash_app: [ui started]` 的空闲堆与最大连续块数字，与 RAM
   预算表对照；以实测值为准。

然后确认产品启动路径：必须出现仪表盘，不得出现基线 demo 菜单。

```bash
~/.venvs/dashboard/bin/python host/ble_central.py --verbose
```

最后是两条行为检查：

- **A3：** 停掉推送端（`Ctrl-C`），观察数值变为 `--` 而“距今”继续增长。
- **A4：** 让主机空闲数分钟，再给工牌断电重启：工牌应重新广播、推送端应自行重连、
  “距今”应归零。
