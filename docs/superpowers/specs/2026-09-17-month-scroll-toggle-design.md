# 设计：整月翻滚开关与逐周平滑滚动翻月

日期：2026-09-17
状态：已批准（修订历程：垂直滑动 → 瀑布流方向 → 下月=上划/上月=下滑 → 逐周连续滚动）

## 背景与目标

WinCal 原生日历目前切换月份（鼠标滚轮、←/→ 方向键、点击相邻月份日期）时都是
「整月瞬间替换 + 淡入动画」（`BeginTransition()`，内容透明度 0.45 → 1.0，约 290ms）。

目标：在设置中增加一个开关「整月翻滚」，提供两种翻月模式——

- 开（默认）：保持现状，整月切换 + 淡入。
- 关：翻月改为**逐周连续滚动**动画：月份网格按一个连续的周行列表滚动，
  翻月时能看到中间真实的周依次滚过（不是把整月网格当作一整块平移）。
  方向按翻页方向：翻到下一个月内容向上滚，翻到上一个月内容向下滚。
  交互不变——滚轮、方向键、点击日期仍是「翻月」一步到位，只有动画不同。

## 决策记录

| 决策点 | 选择 | 说明 |
| --- | --- | --- |
| 动画样式 | 逐周连续滚动 | 用户指定「理解为逐周」：滚动的是连续周行列表，中间周真实滚过；横向滑动、整块月网格平移、连续手势滚动方案已否决 |
| 滚动方向 | 按翻页方向 | 下一个月 = 内容向上滚；上一个月 = 内容向下滚。用户确认「下月=上划，上月=下滑」 |
| 年切换 | 保留淡入 | 跨 52 周无滚动意义，平滑模式下 ↑/↓ 年切换仍走现有淡入动画 |
| 开关默认值 | 开（整月翻滚） | 升级后行为不变 |
| 生效范围 | 所有翻月方式 | 滚轮、←/→ 方向键、点击相邻月日期统一走逐周滚动 |
| 实现方式 | 虚拟周行列表渲染 | 滚动中每行都是真实日期（锚点 + 7 天步进），无位图快照，DPI/主题天然正确 |

否决方案：旧帧位图快照（需处理 DPI/主题/hover 一致性，收益小）；
连续手势滚动（与离散切换模型、outside-click 隐藏、日程面板布局耦合过深，YAGNI）。

## 修订（2026-09-18）：开关两态均带动画

用户复述期望后调整语义——关闭态保留逐周滚动，**开启态从“无动画整月切换”升级为
“整月翻滚”本义：新既单月整页在同一竖直轨道上滑动（类似 Vue 虚拟滚动的整页轨道）**。

| 决策点 | 选择 | 说明 |
| --- | --- | --- |
| 开启态动画 | 整月整页滑动 | `BeginMonthSlide`：新/旧 42 格单月网格按 `-dir·gridH·ease(p)` 同轨道平移，方向沿用「下月=上划，上月=下滑」 |
| 预加载相邻月 | 按需即时生成 | 日期由 `DateForCell`（天数序数）即时推算，无 IO/缓存，天然等价于虚拟滚动的按需渲染 |
| 关闭态 | 维持逐周滚动 | 既有 `BeginScrollTransition` 不变 |
| 年切换 | 仍为淡入 | 两态下 ↑/↓ 均不变；非相邻月点击也回落淡入 |
| 触发与时长 | 与逐周模式一致 | 滚轮、←/→、相邻月点击；16ms 定时器、`EaseOutCubic`、步进 0.058 |
| 标题切换 | 交叉淡化 | 与逐周模式共用 `scrollFromYear_/Month_` 状态 |
| 测试 | 确定性 CPU 回读 | 预览工具改为 WARP `ID2D1DeviceContext` + `TARGET` 位图绘制、`CopyFromRenderTarget` 到 `CANNOT_DRAW|CPU_READ` 位图后 `Map` 读回；不走窗口合成，锁屏/显示器休眠也稳定（本机 `TARGET|CPU_READ` 组合被 E_INVALIDARG 拒绝，须双位图） |

## 详细设计

### 1. 设置项（app_settings.h / app_settings.cpp）

- `AppSettings` 新增 `bool monthPaging{true};`（整月翻滚，默认开）。
- JSON 键 `MonthPaging`：
  - `Load()`：`BoolValue(content, "MonthPaging", true)`，旧配置缺键时回落默认，向后兼容。
  - `Save()`：追加一行 `"MonthPaging": true/false`。
- 存取模式与现有 `AutoStartup` 一致。

### 2. 日历渲染参数化（main.cpp）

- 新增 `DrawCalendarContent(int year, int month, float offsetX, float offsetY, float alpha)`：
  绘制「月份标题 + 42 格日期网格」。
  - `DateForCell(index)` 参数化为 `DateForCell(year, month, index)`，
    消除对 `displayYear_ / displayMonth_` 的隐式依赖。
  - 现有淡入动画（`transition_`）走同一入口；但淡入模式下标题保持不透明
    （现状标题不参与淡化），仅网格应用 alpha，行为与现状一致。
- 无动画时行为与现状完全一致（单次绘制、offset 0、alpha 1）。
- 日期序数工具 `DaysFromCivil` 与 `CivilFromDays` 均已存在（main.cpp 顶部），
  周行锚点直接用它们按 7 天步进推算真实日期，无需新增。

### 3. 逐周连续滚动动画（main.cpp）

- **周锚点**：`WeekRowStart(year, month)` = 配置的周起始日（周日或周一）
  在当月 1 日当天或之前最近的日期，即源月份网格第一行的真实日期。
- **滚动距离**：`D = (WeekRowStart(目标月) - WeekRowStart(源月)) / 7` 天差换算为周数，
  期望范围 3~5 行，代码钳制到 `[1, 6]` 兜底。滚动距离随月份和「每周开始」设置浮动。
- **动画状态**：`scrollAnimating_`、`scrollProgress_`（0→1）、`scrollDir_`
  （+1 = 下一个月内容向上滚；-1 = 上一个月内容向下滚）、
  `scrollFromYear_ / scrollFromMonth_`（源月份，锚点由它派生）、`scrollRows_`（距离 D）。
- **渲染**：滚动位置 `s`（行，浮点）从 0 → `scrollDir_ * D`，ease-out cubic
  （`p = 1 - (1-t)^3`），时长约 280ms，复用 `kAnimationTimer`（16ms）。
  - 行号 `r` 覆盖 `⌊s⌋ … ⌊s⌋+7`，行顶部 y = `kGridTop + (r - s) × kCellHeight`；
  - 每行 7 个日期 = `WeekRowStart(源月) + r × 7 天 + 列偏移`，均为真实日期；
  - 仅目标月份的日期用主色，其余按现状灰色（与最终静态帧一致）；
  - 月份标题「YYYY年M月」原位交叉淡化（旧标题 1→0、新标题 0→1，与进度同步）；
  - 滚动内容用 `PushAxisAlignedClip` 裁剪在网格区
    （y ∈ [kGridTop, kGridTop + 6×kCellHeight]，即 124–418 逻辑像素），
    星期表头、分隔线、日程面板全程不动。
- **落位一致性**：动画结束 `s = scrollDir_ × D` 时顶部行恰好是
  `WeekRowStart(目标月)`，与静态渲染的网格逐格相同（由锚点定义保证），
  随后切回常规渲染路径。
- **动画进行中再次翻页**：先立即结算当前动画（应用目标月份），再以它为源月开始
  新动画；快速滚轮表现为逐月推进。
- 开关开启时完全保持现有 `BeginTransition()` 淡入路径，不改动。

### 4. 触发点接入（main.cpp）

- `ChangeMonth(delta)`：计算新 (year, month) 后按 `settings_.monthPaging` 分流——
  关 → `BeginScrollTransition(sign(delta), fromYear, fromMonth)`（逐周滚动）；
  开 → 现有 `BeginTransition()`（淡入）。
- `ChangeYear(delta)`：两种模式都走现有淡入动画（年切换不逐周滚动）。
- 点击相邻月日期（`WM_LBUTTONUP` 中 `selected_` 与显示月不同步的分支）：
  年份也相同或仅差一月跨年（如 12 月 → 1 月）时按月份差走逐周滚动，
  `dir = sign((目标年×12 + 目标月) - (显示年×12 + 显示月))`；
  跨年（年差 ≥ 1）走淡入。
- 键盘 ↑/↓ 走 `ChangeYear`（淡入），滚轮与 ←/→ 经由 `ChangeMonth`，自动覆盖。

### 5. 设置窗口（main.cpp）

- 「常规」页「每周开始」下方新增一行：
  - 标签「整月翻滚」（y ≈ 352）+ 说明「关闭后翻月时平滑滚动」；
  - 右侧 toggle（与「开机启动」开关同款，坐标 410 对齐）。
- 新控件 ID `kSettingMonthPaging`（2008，现有最大 2007）。
- `DrawSettingsButton` 的 `toggle` 分支扩展为两个 ID；
  `PopulateSettingsControls`、`SaveSettingsFromControls`、WM_COMMAND 翻转处理同步接入。
- 「恢复默认」：`settingsDraft_ = {}` 经成员初始化回到 `monthPaging = true`。
- 翻页方式不影响外观，不接入实时预览（`ApplyLiveSettingsPreview`）；
  保存后经既有 `kSettingsChangedMessage` 流程即时生效。

### 6. 测试与文档

- `WinCalSettingsPreview`（离屏渲染常规页）自然覆盖新控件绘制，无需改测试代码。
- `smoke.cpp` 增加一行 `monthPaging` 读取断言。
- 根 README 功能清单补一句：翻月支持整月切换或逐周平滑滚动（设置中切换）。

## 边界与错误处理

- 旧 settings.json 无 `MonthPaging` 键 → 默认开，无迁移步骤。
- 周距离 `D` 理论范围 3~5 行（受月份天数与「每周开始」影响），代码钳制 `[1, 6]` 兜底，
  非法值不致越界或除零。
- 滚动行号按 `⌊s⌋…⌊s⌋+7` 渲染并钳制在网格区内，日期推算基于天数序数，
  年份边界（如 1899→1900）由 `DaysFromCivil / CivilFromDays` 的序数运算天然处理。
- 动画中弹窗被隐藏（outside-click / Esc）→ 现有 `HidePopup` 已 KillTimer，
  动画停止即可，月份状态已是目标值，无残留。
- DPI 变化、主题切换发生在动画中 → 每帧实时重绘周行内容，天然正确。

## 不做的事

- 不改 WPF 归档工程。
- 不做横向滑动、连续手势滚动、速度惯性。
- 不为动画新增独立线程或位图缓存。
