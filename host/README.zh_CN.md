<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# 主机侧：采集器与 BLE 推送端

本目录的所有内容都运行在**主机**（被监控的 Linux 节点）上，不在工牌上。工牌是 BLE
peripheral，这一侧是连接它并写入状态的 central。

| 文件 | 用途 |
| --- | --- |
| `collector.py` | 读取节点健康与 agent 活动，输出一条状态串 |
| `protocol.py` | 状态串格式的参考编解码器 |
| `ble_central.py` | 连接工牌并按间隔写入状态 |
| `dashboard.conf` | 连接参数与间隔（无凭证、无地址） |
| `systemd/ai-passport-dashboard.service` | 把 `ble_central.py` 作为服务运行 |
| `http_debug.py` | 把状态串用 HTTP 暴露，便于 `curl` 调试 |

格式定义见 [`../protocol/protocol.zh_CN.md`](../protocol/protocol.zh_CN.md)。本目录
不硬编码任何网络地址：接口与地址在运行时从内核读取。

## 依赖

- Linux + BlueZ 5.43 或更高，且蓝牙适配器可用
- Python 3.9 或更高
- 仅 BLE 路径需要 `bleak`（`collector.py` 与 `http_debug.py` 只用标准库）

## 安装

```bash
sudo apt-get install -y bluez python3-venv
sudo usermod -aG bluetooth "$USER"     # 需要重新登录才生效
python3 -m venv ~/.venvs/dashboard
~/.venvs/dashboard/bin/pip install bleak
```

确认适配器就绪：

```bash
bluetoothctl --version
systemctl is-active bluetooth
bluetoothctl list
```

## 工牌到手前的验证

`--dry-run` 既不需要蓝牙也不需要 `bleak`，可用来确认采集器输出合法状态串并观察数值
变化：

```bash
python3 ble_central.py --dry-run --frames 5 --interval 1
```

```text
[1] 114 B  v=1;t=1792919687;h=Ubuntu-Box-Dev;up=151346;ld=0.07;cpu=0;mem=8;dsk=5;tmp=40;net=eno1,up,10.2.65.61;ag=0;as=150955
```

每条状态串在打印前都会先解码校验，所以格式错误会在这里暴露，而不是悄悄在工牌上失败。

## 对工牌运行

```bash
~/.venvs/dashboard/bin/python ble_central.py --verbose
```

推送端按服务 UUID 扫描工牌（找不到则退回按广播名称匹配），连接后打印协商到的 ATT
MTU，并按 `interval_s` 写入。断线会以指数退避重连，上限为 `max_retry_delay_s`；
工牌关机属于正常情况，进程不会退出。

## 安装为服务

```bash
sudo install -m 0644 host/systemd/ai-passport-dashboard.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now ai-passport-dashboard
journalctl -u ai-passport-dashboard -f
```

若代码目录或 venv 不在 `/home/vt/ai-passport` 与 `/home/vt/.venvs/dashboard`，需修改
单元文件里的三个路径（`WorkingDirectory`、`ExecStart`）。安装前先校验：

```bash
systemd-analyze verify host/systemd/ai-passport-dashboard.service
```

## 配置

所有可调项都在 `dashboard.conf`；命令行参数可对单次运行覆盖它。`sessions_dir` 默认为
空，表示采集器使用 `~/.pi/agent/sessions`；若服务用户的家目录与运行 agent 的账号不同，
需要显式设置。

`interval_s` 必须远小于 15 秒：工牌在 15 秒后标记链路为 stale，120 秒后标记为 lost。

## HTTP 调试

```bash
python3 http_debug.py --host 127.0.0.1 --port 8000
curl -s http://127.0.0.1:8000/status        # 原始状态串
curl -s http://127.0.0.1:8000/status.json   # 状态串 + 解析后的字段
```

调试服务没有鉴权。除非确实需要局域网访问，否则绑定 `127.0.0.1`，用完即停。

## 排查

| 现象 | 检查项 |
| --- | --- |
| `badge not found in scan` | 工牌开机了吗？广播名称/UUID 与 `dashboard.conf` 一致吗？ |
| `MTU 23 is below 203` | 工牌拒绝了更大的 MTU 请求，状态串无法一次写入 |
| `write failed` 反复出现 | BlueZ 权限：服务用户是否在 `bluetooth` 组？ |
| 状态串正常但数值显示 `--` | `sessions_dir` 下没有 agent 会话；这是如实上报，不是错误 |
| `the 'bleak' package is required` | 往 `ExecStart` 使用的 venv 里安装 `bleak` |
