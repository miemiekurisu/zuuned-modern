# Zuuned Modern 代码审计与 WMA Lossless 编码同步实施方案

*关键链路专项静态审计 / Windows Media Foundation 编码后端 / Zune MTP 设备同步*

版本：V1.0 审查日期：2026 年 10 月 28 日 代码分支：miemiekurisu/zuuned-modern · main

适用范围：Windows 客户端；音频库、转码层、同步调度、MTP/PTP 传输及相关测试。

| **结论维度** | **审计结论**                                                                                                  |
|--------------|---------------------------------------------------------------------------------------------------------------|
| 现有能力     | 已支持扫描 WMA、使用 wmav2 生成有损 WMA、识别 ASF 并进入 WMA 原样同步分支、调用 libzune 发送文件。            |
| 缺失能力     | 缺少 WMA Lossless 编码器、独立无损同步策略、真实 codec 检测、无损验证和按块文件发送。                         |
| 架构判断     | Windows 原生 WMA Lossless MFT + ASF 媒体写入 + 现有 FFmpeg 解码 + 现有 libzune MTP 传输，技术上具备可行路径。 |
| 上线条件     | 必须完成 Windows 编码器检测、PCM 等价性校验、实机同步与可听播放验证；目前均未宣称通过。                       |

审计边界声明：本报告依据在线读取的公开仓库关键源文件和微软官方技术文档编制。未能在当前环境完整克隆仓库、执行 Windows/MSVC 构建、运行自动化测试或连接 Zune 实机。因此它属于“关键链路专项静态审计与实施设计”，不构成全仓逐行审计或完成验收的证明。

# 1. 执行摘要与结论

总体结论：可保留现有 Qt/QML、FFmpeg、SyncEngine、DeviceWorker 和 libzune 架构，通过增加 Windows Media Foundation WMA Lossless 编码后端及同步策略，实现“导入 FLAC/WAV → 自动编码 ASF/WMA Lossless → 上传 Zune → 播放”的完整闭环。项目当前只能对已有的 ASF/WMA 文件进行原样复制，缺少新格式的无损编码能力。

优先处理事项：① 消除现有无损目标被自动回退 MP3 的风险；② 用实际音频 codec 识别代替 ASF 文件头判断；③ 增加 WMA Lossless 编码及容器封装；④ 改造 MTP 发送为常量内存流式传输；⑤ 对输出进行编码格式、PCM 和设备侧验证。

| **优先级** | **审计编号** | **结论摘要**                                                   | **状态**     |
|------------|--------------|----------------------------------------------------------------|--------------|
| P0         | F-01         | MP3 解码器 EAGAIN 分支丢弃解码帧，存在音频数据缺失风险。       | 源码已确认   |
| P0         | F-02         | 无损编码能力缺失，当前“无损同步”没有独立端到端路径。           | 源码已确认   |
| P0         | F-03         | MTP 发送会按文件大小分配内存；无损文件增大会造成显著内存占用。 | 源码已确认   |
| P0         | F-04         | WMA 同步策略在无编码器时可静默降级 MP3，违背无损目标。         | 源码已确认   |
| P1         | F-05         | 依据 ASF GUID 判断可原样同步的 WMA，未充分验证实际 codec。     | 源码已确认   |
| P1         | F-06         | 转码临时路径由源文件名+PID 组成，存在并发重名风险。            | 源码已确认   |
| P1         | F-07         | MP3 标签重写将整段 MP3 读入内存，并以 ANSI 标记写 UTF-8 文本。 | 源码已确认   |
| P1         | F-08         | 全局共享取消/加速标志使用 volatile；跨线程同步语义不足。       | 源码已确认   |
| P1         | F-09         | 设备上传后 verify 为警告式，文件大小未在该调用中强制比对。     | 源码已确认   |
| P2         | F-10         | 无损容量估算无法沿用当前固定码率公式。                         | 改造关联风险 |

严重性说明：P0 表示影响数据正确性、无损承诺或大文件主链路的阻断项；P1 表示需要在合并前修复的可靠性问题；P2 表示应纳入验收的兼容性或资源风险。风险触发频度、Windows 运行行为和设备兼容性尚待复现。

# 2. 审计基线、方法与审查范围

基线：github.com/miemiekurisu/zuuned-modern，公开 main 分支；审查日期 2026-10-28。仓库标识为 Windows 平台移植，使用 Qt 6/QML 和 C++ 应用层、原生 libzune PTP/MTP/MTPZ 协议栈、FFmpeg/LAME 转码层。仓库 README 说明，Windows 实机验证主要针对一台 Zune HD；不能据此推定全部 Zune 型号已通过验证。[S01]

| **层级**     | **已审查的核心文件**                                            | **关注点**                                             |
|--------------|-----------------------------------------------------------------|--------------------------------------------------------|
| 同步策略     | src/sync/SyncEngine.cpp；src/AppSettings.h                      | 目标格式、能力回退、原样传输、转码与上传调度、容量估算 |
| 转码         | transcode/libav_transcode.c/.h；audio_encode.c；transcodetool.c | 编码接口、缓冲、错误、元数据、命令行测试               |
| UI 与诊断    | qml/SettingsPage.qml；src/diagnostics/DiagnosticsService.cpp    | 格式选择、能力检测、错误可见性                         |
| 设备接口     | src/DeviceWorker.cpp；libzune/src/track.c                       | 对象格式标识、上传、后验校验                           |
| 传输协议     | libzune/src/mtp.c；libzune/src/ptp.c                            | 整文件内存占用、USB 数据阶段与分块传输                 |
| 构建与媒体库 | CMakeLists.txt；src/library/LibraryScanner.cpp                  | Windows 链接、输入扩展名识别                           |

方法：源代码片段的控制流审查、数据路径追踪、编码协议与官方 API 对照、边界与失败情景分析。未执行：完整仓库逐文件审阅、运行时竞态检测、Fuzz、MSVC 编译、编码性能基准和设备实机验证。证据索引见附录。

# 3. 现有音频同步链路复盘

```text
LibraryScanner（识别 .flac / .wav / .wma 等文件）
  → AppSettings（syncAudioFormat = mp3 | wma）
  → SyncEngine::readAudioSyncPolicy（能力检测、降级、码率）
  → SyncEngine::prepareEntry（ASF 头判断 / 原样复制 / 转码）
  → zuuned_transcode_audio_ex（MP3: LAME；WMA: wmav2）
  → SyncEngine::onPrepareFinished
  → workerSendTrack → DeviceWorker::doSendTrack
  → zune_smuggle_track_ex → send_track_internal
  → mtp_send_object_from_file → ptp_transaction(PTP_OC_SendObject)
  → Zune 设备 / ZMDB 元数据
```

现有 MTP 代码在检测到文件名后缀 .wma 时采用 MTP_OFC_WMA 对象格式 [S04]；SyncEngine 发送完成前调用同步恢复信息保存，再将已准备文件交给工作线程 [S02]。因此新增无损编码不需要重写设备认证与专辑对象创建逻辑，但应修复协议层文件发送的内存使用及增强校验。

# 4. 详细静态审计发现

## 4.1 F-01：MP3 处理 EAGAIN 时丢失音频帧（P0）

证据：transcode/libav_transcode.c 约 281–294 行。avcodec_send_packet 返回 EAGAIN 后，代码通过 avcodec_receive_frame 取出帧并立即 av_frame_unref，未进入音频重采样及 LAME 编码流程；丢失的数据无法从最终 MP3 文件中恢复。[S03]

整改：提取统一 decode-drain-pump 循环。对 EAGAIN 必须将暂存帧完整送入下游再重试当前 packet；对 EOF/drain/swr flush 明确校验错误码；失败不得保留“成功”输出。WMA Lossless 后端也必须复用正确的泵循环设计。

## 4.2 F-02：缺少 WMA Lossless 编码器与独立目标（P0）

证据：libav_transcode.h 当前只有 ZUUNED_AUDIO_MP3=0、ZUUNED_AUDIO_WMA=1；libav_transcode.c 仅允许这两个目标，WMA 实际调用 wmav2；audio_encode.c 注释承认无法写出无损 WMA。[S03][S05]

根因：仅考察 FFmpeg 和 Windows ACM，未接入 Windows Media Audio Encoder 的 MFT 路线。微软文档明确列出 WMA Lossless 编码输出 WAVE_FORMAT_WMAUDIO_LOSSLESS（0x0163），以及适用的 IMFTransform 接口。[M01]

## 4.3 F-03：上传前全文件 malloc（P0）

证据：libzune/src/mtp.c 中 mtp_send_object_from_file 从文件读取 uint32_t len，先 malloc(len)，再 fread 整首音乐并调用 mtp_send_object；其下游 ptp.c 虽以约 1 MiB 分块 USB 写入，但输入要求仍为完整内存缓冲。[S06][S07]

整改：从文件流按块读取，同一个 PTP SendObject transaction 中写一次 data-container header，再循环上传块并发送所需 ZLP，最后接收 response。不得把每个块拆成新的 SendObject 命令。应保留已有 split_header_data、USB maxpacket 对齐和错误清理语义。当前单个对象仍受 uint32 ObjectCompressedSize 约束；超过该限制须明确失败。

## 4.4 F-04/F-05：静默有损回退与格式判断错误（P0/P1）

证据：SyncEngine::readAudioSyncPolicy 在 wmav2 不可用时将 WMA 请求直接改成 MP3；nativeWma 判断只要求目标是 WMA、允许复制且文件头为 ASF GUID。ASF 容器并不能证明内部一定是 Lossless，也不能排除视频、受保护或设备不兼容流。[S02]

整改：增加目标枚举 WMA_LOSSLESS=2；无损目标检测失败必须阻断并显示原因。对原生复制采用 avformat/avcodec 的音频 codec_id、stream count、声道/采样率/位深及 DRM/ASF 头参数检测；将设备支持能力抽象为独立 Profile。

## 4.5 F-06/F-07：临时文件与元数据安全问题（P1）

证据：make_temp_path 用文件基名+PID 组成路径，不具备同进程任务的唯一性；MP3 重写先 malloc(raw_size) 读入整文件，再重写标签。部分 ID3v2.3 帧以编码字节 0x00（ISO-8859-1）标记，却写入来自 FFmpeg 的原始 UTF-8 元数据。[S03]

整改：Qt/QTemporaryFile 或 Windows CreateFile(CREATE_NEW) 生成独占临时文件；所有文件写入应进行返回值检查和原子发布。MP3 标签用合规 UTF-16（ID3v2.3）或经过验证的标签库处理中文。转码输出通过分块复制或直接写前置 ID3 方式消除整文件额外缓冲。

## 4.6 F-08/F-09/F-10：线程、验证与容量估算（P1/P2）

证据：libav_transcode.c 采用 static volatile int 共享取消标志；DeviceWorker::doSendTrack 虽调用 zune_verify，但其 expected_size 参数传入 0 且错误只被记录；estimateTrackBytes 依固定 kbps 计算，不能代表 WMA Lossless 的实际空间需求。[S03][S08][S02]

整改：使用 C11 _Atomic 或由 C++ 控制层传递线程安全取消 token；上传后强制读取 GetObjectInfo 并比对编码文件大小，必要时执行读回哈希；无损任务采用真实输出大小作为最终发送门槛，在编码前用 PCM 容量及安全裕量做保守估算。

# 5. 目标功能与行为契约

| **编号** | **功能要求**     | **通过条件**                                                                                |
|----------|------------------|---------------------------------------------------------------------------------------------|
| R01      | 无损目标独立选项 | Settings 可选 MP3 / WMA（有损）/ WMA Lossless；用户选择可持久化                             |
| R02      | 源文件处理       | FLAC、WAV 等可解码无损音频转为正确的 WMA Lossless + ASF；受支持的原生 WMA Lossless 优先直传 |
| R03      | 禁止隐式降质     | 无损模式下编码器缺失、参数不支持、校验失败时显式报错；绝不自动降为 MP3、wmav2 或降采样      |
| R04      | 设备兼容         | MTP 以 WMA 对象格式上传；Zune 能识别、显示、播放并完整结束歌曲                              |
| R05      | 元数据一致       | 标题、艺人、专辑、专辑艺人、编号、年份、流派与中文字符保持正确                              |
| R06      | 资源与可靠性     | 编码/发送增量内存有界，支持取消、断开、满盘清理和重试                                       |
| R07      | 无损证据         | 相同 PCM 参数下，源文件及输出文件解码为规范 PCM 的帧数和哈希一致                            |

参数策略：第一阶段限定 Windows 上的 PCM 16-bit / 44.1 kHz / 双声道（可另测 48 kHz），作为“设备优先的严格无损”MVP；已有更高位深、采样率、或多声道素材默认报明确的兼容性错误。若后续提供“转换到设备支持参数”的模式，必须单独命名，并明确其相对原始 PCM 并非逐样本无损。

# 6. 方案选型与关键技术论证

| **候选方案**                             | **优点**                                              | **限制**                                                                  | **建议**             |
|------------------------------------------|-------------------------------------------------------|---------------------------------------------------------------------------|----------------------|
| FFmpeg 直接 wmalossless 编码             | 沿用 libavcodec 架构                                  | 当前应用打包版本只提供 WMA Lossless 解码，没有对应编码器                  | 不可作为现阶段主路径 |
| Windows ACM 编码                         | 接口较简单                                            | 检查 ACM 编码器并不能覆盖 Media Foundation MFT；仅据 ACM 缺失下结论不充分 | 排除                 |
| Windows Media Foundation WMA Encoder MFT | 微软原生支持 WMA Lossless；官方类型与编码模式文档明确 | 仅 Windows；需要 COM/MFT 与 ASF 封装，必须实机验证输出                    | 首选                 |
| 独立外部旧版编码工具                     | 可降低首轮 COM 接口开发                               | 分发、授权、无头调用、路径、错误处理和维护较复杂                          | 仅作验证对照         |

官方依据：Windows Media Audio Encoder 支持 Standard、Professional、Lossless 三类输出 [M01]。微软规定无损枚举先设置 MFPKEY_VBRENABLED=TRUE、MFPKEY_CONSTRAIN_ENUMERATED_VBRQUALITY=TRUE、MFPKEY_DESIRED_VBRQUALITY=100，再调用 GetOutputAvailableType 并选择 0x0163 类型 [M02]。编码器输出媒体类型须保持其扩展格式字节，且编码器自身不负责重采样 [M03]。ASF 写入可通过 Media Foundation 的 ASF Profile/ContentInfo/File Sink 完成 [M04]。

# 7. 推荐实施架构

```text
本地音乐库（FLAC / WAV / ALAC / APE / WMA）
  ↓
AudioProbe（真实 codec / PCM 参数）
  ↓
设备 Profile / 同步策略
  ├─ 兼容的 WMA Lossless → 原样复制 / 安全暂存 ───────────┐
  └─ 其他无损源文件 → FFmpeg 解码为 PCM                  │
                      ↓                                 │
                 WMA Lossless Encoder MFT               │
                      ↓                                 │
                 ASF ContentInfo + File Sink            │
                      ↓                                 │
              已准备文件：.wma（0x0163）←────────────────┘
                      ↓
          输出 codec/PCM/元数据校验 + 设备容量复核
                      ↓
          SyncEngine → DeviceWorker → libzune MTP
                      ↓
              流式 SendObject + 文件大小验证
                      ↓
                   Zune 设备
```

## 7.1 转码 API 扩展

保持现有 char\* 临时路径返回值以控制改动范围，但建议通过新增上下文结果结构返回错误码、实际 codec、采样率、输出大小、是否重新编码等信息，避免仅靠 NULL 和通用 “transcode failed” 提示定位问题。

```c
// libav_transcode.h — 设计草案，非已编译补丁
#define ZUUNED_AUDIO_MP3 0
#define ZUUNED_AUDIO_WMA 1  // existing wmav2 lossy
#define ZUUNED_AUDIO_WMA_LOSSLESS 2

int zuuned_audio_encoder_available(int target);
char *zuuned_transcode_audio_ex(
    const char *input,
    const char *album_artist,
    int disc,
    int target,
    int bitrate_kbps,
    int vbr,
    zuuned_progress_cb progress,
    void *userdata
);
// target=2: bitrate_kbps / vbr ignored; never silently downgrade.
```

## 7.2 Windows Media Foundation 后端

1.  为每个工作线程建立 RAII COM/MF 生命周期管理（CoInitializeEx、MFStartup；相应 CoUninitialize、MFShutdown），并检测 Windows N/缺失媒体组件等环境问题。

2.  实例化 CLSID_CWMAEncMediaObject 或经 MFTEnumEx 枚举音频编码器；查询 IMFTransform、IPropertyStore。

3.  按官方顺序设置无损属性，再枚举 GetOutputAvailableType，核对 wFormatTag==0x0163、目标采样率、16-bit PCM、2 channels；不得自行构造或裁剪 encoder 输出扩展数据。

4.  在 SetOutputType 后枚举和设置匹配的输入 PCM 类型；注意 MFT 不做 resample，FFmpeg 端需按输入规范提供准确、对齐的 PCM 样本。

5.  循环送入 IMFSample/IMFMediaBuffer，处理 ProcessInput/ProcessOutput 反压、MFT_E_NOTACCEPTING 与 MFT_E_TRANSFORM_NEED_MORE_INPUT；终止时发出正确 drain/flush 信号。

6.  创建 ASF Profile/ContentInfo 与文件写入组件，写入完整编码格式描述、时间戳、时长、stream 编号及元数据，再 finalise 并 fsync/close 临时文件。

7.  重新打开生成的 ASF，校验仅包含期望音频流且 codec 为 WMA Lossless；可选择先只在 CLI 中启用，测试稳定后接入 SyncEngine。

封装策略注意：Media Foundation 的 ASF 写入涉及 Profile、ContentInfo、File Sink 等对象；不要默认将任意 MFT 输出裸码流直接写入 .wma 就能形成合法容器。官方范例的完整 ASF Profile 与编码器拓扑应作为实现参考。[M04][M05]

# 8. 同步调度与设备侧改造

## 8.1 AppSettings / SyncEngine 策略

```cpp
// 策略伪代码：非可直接编译替换代码
switch (settings.audioFormat) {
case "mp3":          target = MP3; break;
case "wma":          target = WMA_STANDARD; break;
case "wma_lossless": target = WMA_LOSSLESS; break;
default:             fail("unknown target");
}
if (target == WMA_LOSSLESS && !encoderAvailable(WMA_LOSSLESS))
    fail("Windows Media Foundation WMA Lossless unavailable");
if (target == WMA_LOSSLESS && nativeCopy && probe.isCompatibleWmaLossless())
    prepared = stageOriginalWma();
else if (target == WMA_LOSSLESS)
    prepared = encodeWmaLossless(source);
// NEVER change target from lossless to lossy implicitly.
```

检查点：readAudioSyncPolicy、prepareEntry、onPrepareFinished、workerSendTrack 的音频目标须保持同一值；同步队列要记录目标编码格式与实际产物 codec。已有 podcast/audiobook 固定 MP3 路径不随本次改造变更。

## 8.2 MTP 文件发送流式化

改造点：mtp_send_object_from_file 与 ptp_send_data/ptp_transaction 的文件源数据传输路径。应保留 SendObjectInfo（确定对象大小/格式）→ 同一事务 SendObject（Header + 多个 USB Bulk 块）→ Response → metadata 的原有序列。[S04][S06][S07]

- 新增 ptp_transaction_send_file(session, opcode, path, size, progress, cancel)：函数内部自行读取文件块，按当前 split-header 模式组织 header；不要在上层预先 malloc(file_size)。

- 固定内存缓冲建议 256 KiB–1 MiB；测试不同 Zune 型号时以目前的 1 MiB USB 分块行为作为性能基线，不擅自改变对齐与零长度包规则。

- 每成功写完一块才更新 byte progress；断链/超时/取消后应明确失败，标记孤儿 handle 并按既有恢复机制处理；不可在同一事务中无约束重试某个 USB 块。

- 补充发送完成的必选验证：MTP GetObjectInfo.size == prepared file size，返回对象格式为 WMA；扩展选项为读取设备文件并计算字节哈希。

## 8.3 设备能力与格式复用

保留 track.c::format_from_ext 对 .wma → MTP_OFC_WMA 的映射；在进入发送模块前保证文件后缀与真实容器/codec 一致。推荐以设备型号与已验证媒体 Profile 实施白名单：Zune 30 首批针对双声道 16-bit/44.1kHz 测试，Zune HD 与其他型号单独证实。未经测试，不得把“对象成功写入”认定为“设备可解码”。

# 9. 元数据、空间与异常策略

| **事项**   | **实施要求**                                                                                                                                                         |
|------------|----------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| ASF 元数据 | 维持 title、artist、album、albumartist、track、disc、genre、date 等字段映射；对中文以 Unicode 标准属性写入。必要时通过 MTP 的专辑/艺人对象生成逻辑维持设备显示一致。 |
| 封面       | 优先维持现有专辑图像/代表样本发送路径；单独测试 ASF 文件嵌入封面与设备 ZMDB 封面显示，避免重复写入导致错误。                                                         |
| 容量       | 编码前使用音频时长×采样率×声道×位深/8 + 容器裕量作为保守空间预算；编码后必须以实际 .wma 字节数做最终校验。                                                           |
| 磁盘与缓存 | 临时文件位于本地临时/缓存目录；使用独占生成，完成后原子提交；异常时清理自身临时对象，原始素材绝不更改。                                                              |
| 错误       | 区分 codec 不支持、MFT 不可用、ASF 写入失败、PCM 校验失败、Zune 空间不足、USB 错误、设备端格式拒绝。                                                                 |
| 自动回退   | 无损模式禁止隐式编码格式回退；允许用户显式选择另一目标后重新同步。                                                                                                   |
| 跨平台     | Linux 端仍可以原样发送已有 WMA Lossless；Windows 专有无损编码器能力仅在 Windows 显示可用，其他平台可留插槽。                                                         |

# 10. 修改文件清单与接口影响

| **文件 / 模块**                        | **拟实施变更**                                              | **验收依据**                 |
|----------------------------------------|-------------------------------------------------------------|------------------------------|
| transcode/wmalossless_mf.cpp（新）     | Windows MFT 编码器、ASF 容器封装、进度、取消和资源回收      | 有效的 0x0163 ASF + PCM 校验 |
| transcode/libav_transcode.h/.c         | 新增目标 2；统一 decode/drain；安全临时文件；传递可诊断错误 | CLI 格式选择、异常正确返回   |
| src/AppSettings.h                      | 增加 wma_lossless 设置值，保留 mp3 默认与兼容旧值           | 重启持久化与升级兼容         |
| src/sync/SyncEngine.cpp                | strict lossless 策略、codec 探测、直传与转码分支、空间门槛  | 不发生无意降质、完整上传     |
| qml/SettingsPage.qml                   | 新增 Lossless 选项、屏蔽码率控件、能力/设备提示             | 用户选择可见且含失败原因     |
| src/diagnostics/DiagnosticsService.cpp | 动态探测 MFT 与支持的媒体参数                               | 状态对照实际 Windows 环境    |
| src/DeviceWorker.cpp                   | 发送后要求大小验证，可选字节读回                            | 对象大小一致；错误上报       |
| libzune/src/mtp.c / ptp.c              | 分块文件发送、真实进度和错误取消                            | 峰值内存有界；USB 回归       |
| libzune/src/track.c                    | 保持 MTP_OFC_WMA；补强上传后验证                            | 设备内容与本地输入对应       |
| transcode/transcodetool.c、tests/      | 新增 lossless encode/probe 与异常用例                       | CI / 实机测试报告            |
| CMakeLists.txt、i18n                   | Windows mfplat/mfreadwrite/mfuuid/ole32 等链接，翻译文案    | MSVC 构建及 i18n audit 通过  |

构建链接库属于设计候选，具体还需根据实际使用的 COM CLSID、GUID、ASF 接口选择并在目标 Windows SDK/MSVC 环境验证；此处不宣称已编译通过。

# 11. 可复现测试计划与验收门槛

| **用例** | **输入/情境**                     | **验证点**                                                            | **等级** |
|----------|-----------------------------------|-----------------------------------------------------------------------|----------|
| TC-01    | 16/44.1 双声道 FLAC               | 编码结果 codec_name=wmalossless，WAVE tag 0x0163；PCM hash 和帧数一致 | 阻断     |
| TC-02    | 同规格 WAV                        | 无损编码；零样本丢失、首尾样本一致                                    | 阻断     |
| TC-03    | 已有 WMA Lossless                 | 启用原生复制；文件字节 SHA-256 不变                                   | 阻断     |
| TC-04    | 有损 WMA / 带视频 ASF             | 不得被误判为兼容 Lossless 原样复制                                    | 阻断     |
| TC-05    | 24-bit/96k、单声道、5.1ch         | 明确拒绝或匹配已验证 Profile；不静默重采样                            | 阻断     |
| TC-06    | 编码器不可用 / Windows N          | 设置诊断与错误明确，禁止回退 MP3                                      | 阻断     |
| TC-07    | 中文、日文、特殊字符文件名和标签  | 本地导入、标签、设备列表显示一致                                      | 必要     |
| TC-08    | 重复名称、两线程转码              | 临时文件互不冲突；不会覆盖或交叉发送                                  | 阻断     |
| TC-09    | 大文件（建议 0.5、1、2 GiB 档）   | 峰值内存随固定块大小变化，无整文件 malloc                             | 阻断     |
| TC-10    | 取消/拔线/发送超时                | 失败上报、恢复信息正确、无误报完成                                    | 阻断     |
| TC-11    | 设备空间不足                      | 发送前门槛与已编码实际大小校验；文件不损坏                            | 阻断     |
| TC-12    | Zune 30 实机                      | 上传成功、库识别、逐曲播放、seek、完整结束、重连                      | 阻断     |
| TC-13    | Zune HD 或其他型号                | 独立的兼容性证据，不沿用 Zune 30 结论                                 | 必要     |
| TC-14    | 现有 MP3/WMA 有损 / spoken media  | 与改造前回归输出、行为兼容                                            | 阻断     |
| TC-15    | 文件 SHA / 设备 GetObjectInfo     | 大小一致；可选设备读回哈希一致                                        | 阻断     |
| TC-16    | ASAN/UBSAN 或 Windows AppVerifier | 无 UAF / 数据竞争/越界/泄漏，运行期间可取消                           | 推荐     |

Windows 上 PCM 校验示例（与源一致的 16-bit 双声道前提）：

```powershell
ffmpeg -v error -i "source.flac" -map 0:a:0 -c:a pcm_s16le -f md5 -
ffmpeg -v error -i "output.wma" -map 0:a:0 -c:a pcm_s16le -f md5 -
ffprobe -v error -select_streams a:0 -show_entries stream=codec_name,sample_rate,channels,bits_per_raw_sample -of json "output.wma"
```

哈希相同必须同时确认使用相同的 PCM 规范、声道顺序、样本总数和解码器输出，不可仅比较压缩文件 SHA-256。若输入源本身为有损编码，转成 WMA Lossless 只能保留其解码样本，不会恢复已损失的信息。

# 12. 实施顺序与合并准入条件

| **阶段**                | **交付内容**                                                              | **门槛**                |
|-------------------------|---------------------------------------------------------------------------|-------------------------|
| 阶段 A · 修复基础正确性 | F-01 EAGAIN 丢帧、F-06 临时文件重名、F-08 标志并发；建立 CLI 音频回归基线 | 现有 MP3/WMA 测试通过   |
| 阶段 B · 独立编码器     | wmalossless_mf.cpp + Windows CMake + CLI 编码入口 + PCM 比对              | TC-01/02/05/06 通过     |
| 阶段 C · 同步策略接入   | Settings/SyncEngine/诊断/元数据 + 禁止静默回退                            | TC-03/04/07/11 通过     |
| 阶段 D · MTP 发送与验证 | 文件流读取、每块进度、大小校验及恢复语义                                  | TC-09/10/15 通过        |
| 阶段 E · 实机与回归     | Zune 30 音乐库、播放、seek、完整结束及现有多媒体路径回归                  | TC-12/14 通过后考虑合并 |

最低可合并标准：WMA Lossless 输出必须由真实 0x0163 编码器生成；源/输出 PCM 严格等价；SyncEngine 不隐式降质；设备端可实际播放；大文件 MTP 发送没有整文件内存分配；失败和重试可恢复；原 MP3/有损 WMA 路径回归通过。

# 13. 尚待验证的事项与风险登记

| **事项**                | **当前判断 / 处理方式**                                                                             |
|-------------------------|-----------------------------------------------------------------------------------------------------|
| 实际 Windows MFT 可用性 | 微软接口可行性有官方依据，但当前没有该项目的 MSVC 编码成功记录；以目标操作系统枚举和编码测试为准。  |
| ASF 封装兼容性          | MFT 输出需正确保留 WAVEFORMATEX 的扩展信息；ASF Profile 与时间戳必须经播放器/ffprobe/实机三重核对。 |
| Zune 型号差异           | 已有设备能播放某个 WMA Lossless 样本，不代表所有编码选项/固件都兼容；建立型号 × 采样参数矩阵。      |
| Windows N 与许可        | WMA 编码器可能因系统组件/版本缺失而不可用；不可将系统媒体 DLL 随意打包。                            |
| 设备端字节校验成本      | 完整读回会增加耗时；可分“默认强制 ObjectInfo size + 按需完整读回哈希”两级。                         |
| 输出缓存空间            | 对长轨道按高保守估计、失败清理，避免编码成功但目标设备无足够空间。                                  |
| 全仓代码质量            | 本报告仅对音频主链路专项审查，不覆盖全部 UI/网络/数据库/驱动代码的逐行质量结论。                    |

# 附录 A 证据与参考资料

以下链接指向公开的仓库 main 源码和微软官方文档。行号以 2026-10-28 读取版本为准；分支后续更新可能改变行号。

**S01 仓库 README：**<https://github.com/miemiekurisu/zuuned-modern>

**S02 SyncEngine：**<https://github.com/miemiekurisu/zuuned-modern/blob/main/src/sync/SyncEngine.cpp>

**S03 libav_transcode：**<https://github.com/miemiekurisu/zuuned-modern/blob/main/transcode/libav_transcode.c>

**S04 libzune track：**<https://github.com/miemiekurisu/zuuned-modern/blob/main/libzune/src/track.c>

**S05 audio_encode：**<https://github.com/miemiekurisu/zuuned-modern/blob/main/transcode/audio_encode.c>

**S06 MTP file transfer：**<https://github.com/miemiekurisu/zuuned-modern/blob/main/libzune/src/mtp.c>

**S07 PTP USB transfer：**<https://github.com/miemiekurisu/zuuned-modern/blob/main/libzune/src/ptp.c>

**S08 DeviceWorker：**<https://github.com/miemiekurisu/zuuned-modern/blob/main/src/DeviceWorker.cpp>

**S09 Settings QML：**<https://github.com/miemiekurisu/zuuned-modern/blob/main/qml/SettingsPage.qml>

**M01 Windows Media Audio Encoder：**<https://learn.microsoft.com/en-us/windows/win32/medfound/windowsmediaaudioencoder>

**M02 Configuring Lossless Encoding：**<https://learn.microsoft.com/en-us/windows/win32/medfound/configuring-standard--professional--or-lossless-audio-encoding>

**M03 Configuring Audio Encoding：**<https://learn.microsoft.com/en-us/windows/win32/medfound/configuringaudioencoding>

**M04 Adding ASF File Sink Stream：**<https://learn.microsoft.com/en-us/windows/win32/medfound/adding-stream-information-to-the-asf-file-sink>

**M05 One-pass Windows Media Encoding Tutorial：**<https://learn.microsoft.com/en-us/windows/win32/medfound/tutorial--1-pass-windows-media-encoding>

# 附录 B 交付物清单

- 可编译的 Windows WMA Lossless MFT 编码模块与 C ABI。

- 独立 WMA Lossless 同步设置、运行时能力检测及准确错误文案。

- ASF/codec/PCM 参数识别与原样复制策略。

- libzune/MTP 文件流式发送及文件大小强制校验。

- CLI 编码、PCM 哈希、元数据、取消与异常回归测试。

- Zune 30 实机验证记录（含设备固件、输出参数、文件、播放结果）；其他型号逐一登记。

交付状态（2026-10-29 更新）：本文件仍是审计分析与实施设计文档，但所列改造已在仓库工作树中落地，WMA Lossless 编码同步补丁已经可编译、可运行、可回归，不再是纸面设计。

- 可编译的 Windows WMA Lossless 编码模块与 C ABI：已完成。`transcode/wma_lossless_enc.cpp` 直接驱动 `wmvcore.dll` 内置的 Windows Media Audio 9.2 Lossless 编码器（`IWMCodecInfo3` → `IWMStreamConfig` → `IWMProfile` → `IWMWriter`），非 Windows 由 `transcode/wma_lossless_stub.c` 统一回答“不可用”；未 fork、未附带任何第三方编码器。
- 独立 WMA Lossless 同步设置、运行时能力检测与准确错误文案：已完成。`zuuned_audio_encoder_available()` 为能力来源，缺失编码器时不再静默降级为 MP3，队列会明确说明没有排入任何队列、文件也没有被转而转换。
- ASF/codec/PCM 参数识别与原样复制策略：已完成。`transcodetool container` 报告 ASF codec GUID、`WAVEFORMATEX` tag、是否无损与复制策略判定，`copycheck` 14 项断言守住判定表。
- libzune/MTP 文件流式发送及文件大小强制校验：已完成。`ptp_transaction_send_file()` + `mtp_send_object_from_file_progress()` 以 1 MB 分块从已打开的 `FILE *` 发送，拒绝 0 字节对象与短于承诺长度的源；`mtpframingtool` 逐字节比对分块后与原有缓冲路径的容器帧，`framing 1245 checks 0 failures`。
- CLI 编码、PCM 哈希、元数据、取消与异常回归测试：已完成。`transcodetool encoders/audio-wma-lossless/container/retagcheck/copycheck`、`retagcheck` 27 项、`copycheck` 14 项、`mtpframingtool` 1245 项，以及 `tests\win-compat` 201 项与 `tests\win-usability` `launches=18 checks=134 failures=0 skips=1 killed=0`。
- Zune 30 实机验证记录（含设备固件、输出参数、文件、播放结果）：未完成，仍是开放门禁。本轮没有接入任何 Zune，也没有任何人听过任何输出；能播放 WMA Lossless 的公开依据只有 Zune HD，其余型号一概未验证，删除、播客/有声书、图片/视频类别与此 Pavo 之外的所有型号同样保持未验证。
