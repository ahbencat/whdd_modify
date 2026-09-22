[![Stand With Ukraine](https://raw.githubusercontent.com/vshymanskyy/StandWithUkraine/main/banner-direct-single.svg)](https://stand-with-ukraine.pp.ua)

# WHDD — Linux 硬盘诊断与数据恢复工具

[English](README.md) | 简体中文

WHDD 是 Linux 下的硬盘诊断与数据恢复工具。其字符界面（ncurses/libdialog）
沿袭经典 MHDD 的使用方式：整盘读测试并逐块可视化访问耗时、容错读策略的设备
复制、SMART 信息查看等。

- 项目主页：http://whdd.github.io
- 许可证：GNU GPL v3
- 上游源码与问题反馈：https://github.com/whdd/whdd

本仓库是 [whdd/whdd](https://github.com/whdd/whdd) 的修改版。上游功能全部
保留，本版新增的内容如下。

## 本修改版的改动

### 读测试
- 固定 100×40 字符布局，锚定屏幕左上角。扫描过程中改变终端尺寸不会再打乱
  排版：终端小于布局时暂停绘制，恢复到足够大小后自动继续。
- 启动时终端小于布局会弹出明确的错误提示框，不再静默失败。
- MHDD 风格的耗时配色（深灰 / 灰 / 淡灰 / 绿 / 淡红 / 红）。阈值随读块大小
  线性缩放，任何块大小下颜色含义一致。
- 读块大小可选：256、1024、4096 扇区每块（128 KiB / 512 KiB / 2 MiB）。
- 扫描界面显示设备型号与序列号。

### 快速硬盘诊断
过程菜单中新增 **Quick disk diagnosis（快速硬盘诊断）**，执行只读的快速检查：
- 报告内核可见的逻辑/物理扇区大小，并识别为 512n、512e、原生 4Kn 或自定义/未知；
- 如果底层传输支持原生 SCSI READ CAPACITY(16)，读取 PI 元数据，包括 PI 是否启用、P_TYPE 和 P_I_EXPONENT；
- 在盘头、盘中部和盘尾各读取一个逻辑扇区。

该功能不写入数据、不启用 SMART，也不会执行格式化或坏块重映射。PI 查询不支持时会显示“未知”，不会误报为“未启用”。样本读取通过只表示快速兼容性/可读性检查，并不等同于全盘健康结论；需要进一步诊断时，请运行完整读测试以及只读的 `smartctl -x`/`sg_readcap -l` 检查。

### 扫描报告
读测试结束后自动生成两个文件：
- `WHDD_REPORT_<序列号>_<年月日>_<时分秒>.report` —— 汇总：参数、速度、
  耗时分布直方图、错误类型计数、错误 LBA 范围。
- `WHDD_REPORT_<序列号>_<年月日>_<时分秒>.dg` —— DiskGenius 风格坏道清单，
  按严重程度分三节：`damaged`（读错误）、`severe`（红，≥ 500 ms）、
  `slow`（淡红，150..500 ms），均以连续 LBA 区间列出。阈值随块大小缩放。

### 实验性
`dev` 分支正在开发一个独立的 dd 风格工具（`whdd-dd`）：按 LBA 读取单个块
并显示状态与耗时，支持 POSIX `O_DIRECT` 读取或经 SG_IO（SAT）的
ATA READ DMA EXT，并完整感知逻辑扇区大小（512n / 512e / 原生 4Kn 盘）。

## 编译与安装

Debian/Ubuntu 依赖：`./build_depends.sh`（make、gcc、pkgconf、dialog、
libncurses-dev）。

```
./build.sh && sudo make install
```

或分步执行：

```
make                # 正式版
make whdd_g         # 调试版
sudo make install   # 安装到 /usr/local/bin
```

也支持 CMake：`cmake . && make`；`cmake -DSTATIC=ON . && make` 可编译静态
版本（会把 ncurses 和 dialog 构建到 `external/`）。

访问块设备需要以 root 运行。

## 许可证

GNU GPL v3（见 LICENSE）。原 WHDD 版权归 Andrey Utkin 及贡献者所有；
本仓库中的修改以相同许可证发布。
