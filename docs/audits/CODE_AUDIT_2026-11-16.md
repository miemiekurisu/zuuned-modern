# Zuuned Modern 全仓代码审计报告

*通用静态代码审计 / 安全性 · 正确性 · 并发 · 内存安全 · 可维护性*

| 项目 | 内容 |
|------|------|
| 仓库 | `miemiekurisu/zuuned-modern`（Zune 媒体管理器的 Windows 客户端移植） |
| 分支 / HEAD | `main` · `478847c` |
| 审计日期 | 2026-11-16 |
| 代码规模 | C++ `src/` ≈ 38k 行 · QML ≈ 31k 行 · libzune C ≈ 13k 行 · transcode C ≈ 5.7k 行 · compat C ≈ 2.3k 行 · 测试 ≈ 21k 行 |
| 审计方式 | 静态源码审查 + 对关键结论的逐处取证核对；4 个只读探查代理并行覆盖，所有入库结论均由主审本人复核原始代码 |

## 审计边界声明

本报告基于对本地工作树的只读静态审查，并对每一条入库结论都回到源码逐处核对。**未执行**以下内容：MSVC 构建、运行时动态分析（ASan/TSan/fuzz）、Zune 实机验证、以及对全部 115k 行代码的逐行阅读。因此本报告聚焦于高风险链路与可取证的具体缺陷；"无发现"不等于"该文件绝对无缺陷"。严重性分级按本仓威胁模型给出（USB 设备响应、本地文件名、网络 provider 数据均视为不可信输入）。

---

## 1. 执行摘要

**总体结论：代码质量高于同类项目的平均水平。** 已审查的关键链路（MTP 流式发送、转码取消/加速标志、SQLite 迁移、诊断脱敏、路径规范化、XXE 防护、线程排空）普遍经过反复打磨且防御到位；既往 WMA Lossless 专项审计的 F-01~F-10 经抽样核对基本已闭环。本次新发现的问题集中在少数状态机分支、一处设备输入的整数溢出、以及若干工程加固项，**无远程可利用漏洞**。

共计 14 项发现：高危 2、中危 3、低危 5、工程/可维护性 4。

| 优先级 | 编号 | 结论摘要 | 状态 |
|--------|------|----------|------|
| **高 P0** | A-01 | `mtp_get_storage_ids` 对设备返回的计数做 `4 + n*4` 校验会在 `uint32` 回绕，导致堆缓冲区溢出 | 已核实 |
| **高 P0** | A-02 | 视频发送失败分支无条件 `clearInflight()`，丢失中断恢复标记（音频/照片不会）——对应固件崩溃隐患 | 已核实 |
| 中 P1 | A-03 | `SyncEngine` 析构未 join 线程池任务，且跨线程读 `QPointer`（关机期数据竞争/UB） | 已核实 |
| 中 P1 | A-04 | `refreshPodcastCleanupPreview` 循环内用 `return` 代替 `continue`，单个坏项即丢弃整份清理预览 | 已核实 |
| 中 P1 | A-05 | WPD 发送成功但 handle 解析失败（`itemId==0`）时跳过 `filepath→handle` 登记，影响播放列表成员解析 | 已核实 |
| 低 P2 | A-06 | TMDB `api_key` 位于 URL query，失败日志 `fprintf(url.left(80))` 在会话日志脱敏前打印到原始 stderr | 已核实（已被 Redactor 缓解） |
| 低 P2 | A-07 | `TmdbClient::fetch` 用 `readAll()` 无响应大小上限（与 ArtworkHttp 的 32MiB 上限不一致） | 已核实 |
| 低 P2 | A-09 | 多处通过 `system()`/cmd.exe shell-out 调用 ffmpeg/sips，Windows 下 `%VAR%` 展开未被引号中和 | 已核实 |
| 低 P3 | A-10 | 转码 flush/视频/图片路径对 `avcodec_send_packet` 返回值处理与已修复的 F-01 不一致 | 已核实 |
| 低 P3 | A-12 | `device.c` 多处 `system()` 忽略返回值，且在 Windows 上为无声空操作 | 已核实 |
| 工程 | A-11 | `wma_lossless_enc.cpp` 对定长栈缓冲使用 `sprintf`（当前有界，属潜在隐患） | 已核实 |
| 工程 | A-13 | 构建未启用 `/W4`·`/WX`·`/guard:cf`，主程序无 sanitizer 预设 | 已核实 |
| 工程 | A-14 | 存在多处超大翻译单元（170KB/147KB/120KB），复杂度与审查/测试成本偏高 | 已核实 |
| 工程 | A-15 | `Redactor` 家目录脱敏以正斜杠为主，可能漏掉反斜杠拼写的 Windows 家目录（由用户名规则部分缓解） | 已核实 |

---

## 2. 详细发现

### 高危

#### A-01 · `mtp_get_storage_ids` 整数回绕导致堆溢出

- **位置**：[`libzune/src/mtp.c`](libzune/src/mtp.c) 约 414–429 行
- **证据**：
  ```c
  uint32_t n = le32_unpack(data);        // 设备返回，最大 0xFFFFFFFF
  if (len < 4 + n * 4) { ... return -1; } // n*4 与 4+ 均为 uint32，会回绕
  uint32_t *result = (uint32_t *)malloc(n * sizeof(uint32_t)); // 同样回绕
  for (uint32_t i = 0; i < n; i++)
      result[i] = le32_unpack(data + 4 + i * 4);  // 越界读 data + 越界写 result
  ```
  当 `n ≈ 0x40000000` 时，`n*4` 回绕为 0，`4 + n*4` = 4，`len < 4` 为假，**校验被绕过**；`malloc(n*4)` 退化为极小/零分配，随后循环按 `n` 写入，发生堆溢出与越界读。
- **对比**：同文件的兄弟函数 `mtp_get_object_handles`（约 550 行）用除法写法正确规避：`if (n > (len - 4) / 4) return -1;`
- **影响**：`GetStorageIDs` 在每次连接设备时都会被调用，`n` 为设备可控输入。恶意/伪装的 USB 设备或损坏的响应可触发堆破坏（内存安全）。实际利用需设备侧配合，但 USB 输入按不可信处理是本仓一贯的威胁模型。
- **建议**：改用兄弟函数的除法式上界校验：`if (n > (len - 4) / 4) { ...; return -1; }`，并在 `malloc` 前对 `n` 做合理上限（如存储器数量上限）。

#### A-02 · 视频发送失败时无条件清除中断恢复标记

- **位置**：[`src/sync/SyncEngine.cpp`](src/sync/SyncEngine.cpp) `onVideoSendDone`（约 2635–2683 行）
- **证据**：视频发送前已在 2502 行 `markInflightVideo(...)` 布防 B2 恢复标记；但完成回调开头**无条件**执行：
  ```cpp
  void SyncEngine::onVideoSendDone(const QString &entryId, bool ok, ...) {
      clearInflight();   // B2: the wire object is complete  ← 在失败/陈旧判断之前
      if (entryId != m_currentEntryId || ... ) return;
      ...
      } else {   // ok == false
          m_lastError = autopsyName;
          markEntryTerminal(entryId, "failed", humanAutopsy(autopsyName)); // 无 retainFailedInflight
      }
  ```
- **对比**：音频 `onSendDone`（2167–2168）与照片路径（2352–2353）的正确写法为
  `if (ok) clearInflight(); else { retainFailedInflight(itemId); m_cancelled = true; }`
- **影响**：视频恰是大文件主链路。失败（可能中途中断、留下截断对象）时标记被清除，下次启动的恢复流程将**无法识别并删除**这个截断对象。AGENTS.md 明确记录：截断的媒体对象在播放时会**导致 Zune 固件崩溃/重启**。此外，开头无条件 `clearInflight()` 也会被**陈旧/重复信号**触发，抹掉另一在途条目的有效标记。
- **建议**：将 `clearInflight()` 移入 `ok` 分支（并置于陈旧判断之后），在 `else` 分支补上 `retainFailedInflight(itemId)`，与音频/照片路径保持一致。

### 中危

#### A-03 · `SyncEngine` 析构未 join 线程池任务（关机期竞争/UB）

- **位置**：[`src/sync/SyncEngine.cpp`](src/sync/SyncEngine.cpp) 析构 443–446 行；任务见 1735 行（`QtConcurrent::run`，同类模式另见 ~2531）
- **证据**：
  ```cpp
  SyncEngine::~SyncEngine() {
      m_bookPreparationCancel->store(true);   // 只置原子取消标志
      cleanupCurrentTemp(QString());          // 不等待在途任务
  }
  // 任务体捕获 QPointer<SyncEngine> self，并在 copyCancelled 检查之后仍可能读 self
  ```
- **影响**：有声书/音频准备任务运行在全局 `QThreadPool` 上，析构仅置原子标志而不等待。若对象在关机期先于线程池被销毁，任务可能在越过取消检查点后仍跨线程读取 `QPointer`（Qt 文档明示 `QPointer` 非线程安全）→ 数据竞争/UB。`sourceCopy` 以 `shared_ptr` 值捕获，暂无对象释放后使用；主要风险是跨线程 `QPointer` 读与未等待任务。
- **建议**：用 `QFutureSynchronizer` 或保存 `QList<QFuture<void>>`，在析构里置取消标志后 `waitForFinished()` 再返回。

#### A-04 · 播客清理预览用 `return` 代替 `continue`，单个坏项丢弃整份预览

- **位置**：[`src/sync/SyncEngine.cpp`](src/sync/SyncEngine.cpp) `refreshPodcastCleanupPreview`（约 1190–1216 行）
- **证据**：
  ```cpp
  for (const auto &value : m_device->podcastEpisodesList()) {
      ...
      if (!id || id == 0xffffffffu || ids.contains(id)) return;          // 应为 continue
      ...
      if (key.first.isEmpty() || key.second.isEmpty() || sources.contains(key)) return; // 应为 continue
  }
  ...
  if (matched.contains(id) || candidate.value("spokenId").toString().isEmpty()) return; // 应为 continue
  ```
- **影响**：只要出现一个重复的设备对象 ID、一条空 feed/source 键，或一个已匹配项，整个函数即提前返回，**静默丢弃其余所有有效清理候选**。用户看到的删除预览会比实际少，可能误以为清理已完整（对照 `onPodcastCleanupInspected` 对同类条件采用逐项跳过/报错）。实际删除会在 Sync 时重新校验，故不构成数据丢失，但预览正确性受损。
- **建议**：将上述三处 `return;` 改为 `continue;`。

#### A-05 · WPD 发送成功但句柄解析失败时漏登记（影响播放列表成员解析）

- **位置**：[`src/DeviceWorker.cpp`](src/DeviceWorker.cpp) 1218–1222 行；消费端 [`src/sync/SyncEngine.cpp`](src/sync/SyncEngine.cpp) 2186 行
- **证据**：
  ```cpp
  // DeviceWorker：upload 成功后
  quint32 handle = 0;
  if (!wpdHandleOf(objectId, &handle)) handle = 0;   // 解析失败 → 0
  emit sendDone(entryId, true, handle, QString());    // ok=true 但 itemId 可能为 0
  // SyncEngine::onSendDone
  if (itemId != 0) { m_sentTrackIds.insert(...); m_sentTrackIdentities.insert(...); }
  ```
- **影响**：WPD 后端若无法从 objectId 推导句柄，成功发送会以 `itemId==0` 上报，`filepath→itemId` 映射被跳过，随后同会话内的**播放列表成员解析**可能找不到该曲目。**注意**：紧随其后的 `m_device->noteSyncedTrack(...)` 是无条件调用，基于标题/艺术家/专辑的**去重仍然有效**，因此不会重复发送整轨——影响范围窄于初判。
- **建议**：当 `ok && itemId==0` 时，按 filepath 走一条回退登记路径（或从 `noteSyncedTrack` 的身份键反查），不要仅以 `itemId != 0` 作为登记门槛。

### 低危

#### A-06 · TMDB `api_key` 进入 URL query 且失败时打印到原始 stderr

- **位置**：[`src/library/TmdbClient.cpp`](src/library/TmdbClient.cpp) 131–132 行（日志）；URL 构造见 141/160/172/199/225/253/283/292 行（`?api_key=%2`）
- **证据**：`fprintf(stderr, "[tmdb] fetch failed (%d/%s): %s\n", ..., qUtf8Printable(url.left(80)));`——`url.left(80)` 会跨过 `api_key=` 截取到密钥大部分字符。
- **缓解**：会话日志在写盘与回显**之前**对每行做脱敏（[`src/diagnostics/SessionLog.cpp`](src/diagnostics/SessionLog.cpp) 207–208），`Redactor` 的 URL 规则会把 query 整段替换为 `?[redacted]`，截断后的 URL 仍能命中。因此在日志捕获生效时**不会持久化泄露**。残余风险仅限于脱离会话捕获运行（如开发者直接终端）时实时 stderr 可见，且多数情况下用的是随二进制分发的内置 key。
- **建议**：日志里改用 `QUrl(url).toString(QUrl::RemoveQuery)`；并优先像 `fetch` 的 `api-key` 头那样以请求头而非 query 传递密钥。

#### A-07 · `TmdbClient::fetch` 无响应大小上限

- **位置**：[`src/library/TmdbClient.cpp`](src/library/TmdbClient.cpp) 125 行 `data = reply->readAll();`
- **影响**：相较 `ArtworkHttp`（32 MiB 阈值中止）与 `SpokenMediaService`（分段上限），TMDB 读取整包无上限。被劫持/异常的端点或中间人可诱导大额内存分配。
- **建议**：在 `downloadProgress` 中对累计字节设上限并 `reply->abort()`。

#### A-09 · shell-out 调用 ffmpeg/sips（Windows `%VAR%` 展开未中和）

- **位置**：[`libzune/src/photo.c`](libzune/src/photo.c) 466 行 `system(cmd)`（命令拼接见 325–345 的 `shell_quote_dup` 与 ~440 的 `snprintf`）；另见 [`libzune/src/thumbnail.c`](libzune/src/thumbnail.c) 192 行、[`libzune/src/util.c`](libzune/src/util.c) 384 行
- **现状评估**：`shell_quote_dup` 已对路径做平台化引号包裹并在命令行超长时**拒绝而非截断**，注入面有限（输入多为用户本地文件）。但 Windows `cmd.exe` 即便在双引号内仍会展开 `%VAR%`，而引号函数只处理 `"`，未处理 `%`——一个名为 `%APPDATA%` 之类的文件名会被环境变量展开，造成处理错路径/失败（非 RCE，但属正确性与加固缺口）。
- **建议**：根除此类问题的方式是放弃 shell，改用 argv 数组进程启动（Windows `CreateProcessW` / POSIX `posix_spawn`），彻底消除引号/展开问题；若保留 `system()`，至少对 `%` 做转义处理。

#### A-10 · `avcodec_send_packet` 返回值处理不一致

- **位置**：[`transcode/libav_transcode.c`](transcode/libav_transcode.c) 主解码循环已正确（493–501 对 `EAGAIN` 先排空再重试，即既往 F-01 的修复）；但 flush（542）、视频重封装（1645）、音频重封装（1857）、封面图（2258）等处忽略 `send_packet` 返回值；flush 分支 545–546 在 `malloc` 失败时 `continue` 静默丢帧。
- **影响**：这些路径多为逐帧排空，解码器被填满概率低，但与已修复的 F-01 不一致，存在同类（静默丢音/丢帧）隐患。
- **建议**：在上述位置统一采用"`EAGAIN` 则排空后重试同一包"的写法，并对 `malloc` 失败走显式错误而非静默跳过。

#### A-12 · `device.c` 忽略 `system()` 返回值且跨平台空操作

- **位置**：[`libzune/src/device.c`](libzune/src/device.c) 31–46 行（`killall` / `systemctl` / `pkill`）
- **影响**：命令为硬编码（无注入），但返回值被忽略；在 Windows 上 `system()` 跑的是 `cmd.exe`，这些 POSIX 命令为无声空操作，"阻止其他 MTP 代理干扰"的意图在 Windows 上静默失效。
- **建议**：检查返回值并按平台条件编译；Windows 侧若需同类效果应改用原生 API。

### 工程 / 可维护性

#### A-11 · 定长栈缓冲上的 `sprintf`（潜在）

- **位置**：[`transcode/wma_lossless_enc.cpp`](transcode/wma_lossless_enc.cpp) 549–550 行 `char disc[16]; sprintf(disc, "%d", discNumber);`
- **评估**：`discNumber > 0` 守卫下 `int` 十进制最多 10 位，当前不会溢出 16 字节。但对定长缓冲使用无界 `sprintf` 是一旦守卫变更即复发的潜在隐患。
- **建议**：统一改为 `snprintf(disc, sizeof(disc), "%d", discNumber);`。

#### A-13 · 构建加固开关缺失

- **位置**：[`CMakeLists.txt`](CMakeLists.txt)
- **现状**：各目标 `target_compile_options` 基本仅 `/utf-8` 与强制包含 shim；**未**启用 `/W4`、`/WX`、`/guard:cf`（控制流防护），主程序也**无** sanitizer 预设（AddressSanitizer 等）。MSVC 默认的 `/GS`、`/DYNAMICBASE`、`/NXCOMPAT` 仍在。
- **影响**：较低警告级别会放过类似 A-01 的整数回绕、未使用返回值等问题；缺少 `/guard:cf` 降低了纵深防御。
- **建议**：对本仓自有目标启用 `/W4`（视情况 `/WX`）与 `/guard:cf`；提供一个可选的 ASan/UBSan 构建预设用于 CI（测试套件已具备可运行基础）。

#### A-14 · 超大翻译单元

- **位置**：`LibraryService.cpp`（≈170KB）、`DeviceWorker.cpp`（≈147KB）、`SyncEngine.cpp`（≈147KB）、`WpdBackend.cpp`（≈120KB）、`librarytool.cpp`（≈110KB）
- **影响**：单文件承载过多职责与状态机分支，正是 A-02/A-04 这类"分支不一致"缺陷的温床，且增量编译与评审成本高。
- **建议**：按职责（例如 SyncEngine 的音频/视频/照片/播客各阶段）拆分为更小的编译单元，便于针对状态机分支补充单元测试。

#### A-15 · `Redactor` 家目录脱敏偏向正斜杠

- **位置**：[`src/diagnostics/Redactor.cpp`](src/diagnostics/Redactor.cpp)（`homes` 正则 `"/(?:home|Users)/..."`，以及对 `homePath` 的直接替换）
- **影响**：本仓库库路径以正斜杠规范存储，但原生显示路径（`PathText::display`）为反斜杠。若日志出现反斜杠拼写的 `C:\Users\<名>\...`，`homes` 正则不匹配、`homePath` 直接替换也可能错过；目前靠独立的用户名规则兜底（账户名会被替换为 `[user]`，但其余路径片段保留）。
- **建议**：为 Windows 增加反斜杠形态的家目录/`\Users\` 匹配，令家目录脱敏与路径分隔符无关。

---

## 3. 已确认的稳健实践（正面证据）

审计同时核实了大量**到位的**工程实践，建议在后续改动中保持：

- **MTP 流式发送**（[`libzune/src/ptp.c`](libzune/src/ptp.c) 203–320、1050–1096）：短读即拒、分块上限、`malloc` 检查齐备；与 `mtpframingtool`（1245 项）帧一致性门禁对应。
- **跨线程标志原子化**（[`transcode/libav_transcode.c`](transcode/libav_transcode.c) 70–100）：MSVC 用 `_Interlocked*`、其余 `__atomic_*` SEQ_CST——既往 F-08 已闭环。
- **解码 `EAGAIN` 排空重试**（同文件 493–501）：既往 F-01 已闭环。
- **SQLite**：全程绑定参数（`?`），无字符串拼接注入；迁移经 `sqlite3_backup` 快照、版本门禁、`BEGIN/COMMIT/ROLLBACK`（[`src/library/LibraryDb.cpp`](src/library/LibraryDb.cpp) 393–442）；拼接进 `IN(...)` 的类型集合来自内部常量而非用户输入。
- **诊断脱敏**（[`src/diagnostics/Redactor.cpp`](src/diagnostics/Redactor.cpp) + [`SessionLog.cpp`](src/diagnostics/SessionLog.cpp) 199–208）：每行在**回显与写盘之前**脱敏，覆盖凭据、Bearer/Basic、私钥材料、PnP 序列号、URL query/userinfo、家目录等。
- **传输安全**：全仓无 `ignoreSslErrors`/`VerifyNone`/`setPeerVerifyMode`；重定向策略为 `NoLessSafeRedirectPolicy`/`ManualRedirectPolicy`，设置了传输超时，拒绝 https→http 降级。
- **RSS/XXE 防护**（探查确认，`PodcastFeed.cpp`）：拒绝 DTD/实体引用，限定 feed 大小、文本长度、嵌套深度与条目数；`mediaUrl` 仅允许 http/https 并剥离 userinfo。
- **路径安全**（[`src/library/LibraryPaths.h`](src/library/LibraryPaths.h)）：`safeComponent` 处理非法字符、尾部点/空格、DOS 保留名、`.`/`..`；`canonicalRoot` 修复 `/E:/` 伤疤并折叠盘符大小写；`caseKey` 平台化比较。
- **线程排空**：自有工作线程均经 `drainThread` 等待后再析构（`DeviceService`、`LibraryService`、`SpokenMediaService`），规避 Qt "销毁运行中 QThread" 的致命错误。
- **仓库卫生**：`_tmp/` 已被 gitignore，无入库构建产物（`.o/.a/.obj/.lib` 均为 0），MTPZ 密钥与 `.mtpz-data`/`*.pem`/`*.key` 均在忽略清单，另有独立隐私门禁。

### 既往 WMA Lossless 专项审计（F-01~F-10）闭环抽样

对 [`Zuuned_Modern_代码审计及_WMA_Lossless_编码同步实施方案.md`](Zuuned_Modern_代码审计及_WMA_Lossless_编码同步实施方案.md) 的若干高优先项做了抽样核对：F-01（解码丢帧）、F-03（整文件内存→流式发送）、F-08（volatile→原子）均已在源码落地；SQL 绑定/迁移姿态与 F-05/F-07 的方向一致。可认为该专项的核心结论已基本处理（本次未逐条复验全部 10 项）。

---

## 4. 修复优先级建议

1. **立即（高危）**
   - A-01：用除法式上界校验修正 `mtp_get_storage_ids`（一行改动，彻底消除堆溢出）。
   - A-02：对齐音频/照片路径，失败分支保留恢复标记——这关系到设备固件安全。
2. **近期（中危）**
   - A-03：析构 join 线程池任务；A-04：三处 `return→continue`；A-05：WPD 句柄为 0 时的回退登记。
3. **排期（低危 / 工程）**
   - A-06/A-07/A-09/A-10/A-12 的日志与健壮性收尾；A-13 开启 `/W4`+`/guard:cf` 并提供 ASan CI 预设（**可借此自动暴露 A-01 一类问题**）；A-14 拆分超大文件时补状态机单测；A-11、A-15 顺手修复。

**建议新增针对性测试**：A-01（构造异常 `GetStorageIDs` 计数的 mock 响应）与 A-02（中断视频发送后验证恢复标记保留）最值得补自动化用例，二者都可在不接实机的前提下验证。

---

## 附录 · 方法与证据

- **工具**：ripgrep/只读源码阅读 + 4 个只读探查代理（native 内存安全、C++ 并发/资源、路径/SQL/网络/输入、设备/同步/QML）并行取证；所有入库结论由主审复核原始代码行。
- **未纳入**：实机行为、构建产物、运行时竞态检测、模糊测试、编码性能基准。
- **代理初判但经复核后下调/剔除**：provider 返回的 MBID 拼入 URL 一项，经核发现 `MusicIdentityClient::albumDetails` 等入口已有 `validMbid()` 前置校验，判定为已缓解，未入库。
