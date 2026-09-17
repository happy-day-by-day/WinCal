# 设计：整月翻滚开关与瀑布流平滑滚动翻月

日期：2026-09-17
状态：已批准（2026-09-17 修订：滑动方向按翻页方向，下月=上划、上月=下滑）

## 背景与目标

WinCal 原生日历目前切换月份（鼠标滚轮、←/→ 方向键、点击相邻月份日期）时都是
「整月瞬间替换 + 淡入动画」（`BeginTransition()`，内容透明度 0.45 → 1.0，约 290ms）。

目标：在设置中增加一个开关「整月翻滚」，提供两种翻月模式——

- 开（默认）：保持现状，整月切换 + 淡入。
- 关：翻月改为**瀑布流式平滑滚动**动画，方向按翻页方向：
  翻到下一个月内容整体向上滚（新月份自底部滑入、旧月份向顶部滑出）；
  翻到上一个月内容整体向下滚（新月份自顶部滑入、旧月份向底部滑出）。

## 决策记录

| 决策点 | 选择 | 说明 |
| --- | --- | --- |
| 动画样式 | 瀑布流式垂直滚动 | 用户指定；横向滑动、连续手势滚动方案已否决 |
| 滑动方向 | 按翻页方向 | 下一个月 = 内容向上滚（新月份自底部滑入）；上一个月 = 内容向下滚（新月份自顶部滑入）。滚动列表隐喻，用户确认「下月=上划，上月=下滑」 |
| 开关默认值 | 开（整月翻滚） | 升级后行为不变 |
| 生效范围 | 所有切换方式 | 滚轮、方向键、点击相邻月日期统一分流 |
| 实现方式 | 双份参数化渲染 | 每帧画两个月份，无位图快照，DPI/主题天然正确 |

否决方案：旧帧位图快照（需处理 DPI/主题/hover 一致性，收益小）；
连续手势滚动（与离散切换模型、outside-click 隐藏、日程面板布局耦合过深，YAGNI）。

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

### 3. 瀑布流平滑滚动动画（main.cpp）

- 新增状态：`scrollAnimating_`、`scrollProgress_`（0→1）、`scrollDir_`
  （+1 = 翻到下一个月，内容向上滚；-1 = 上一个月，内容向下滚）、
  `scrollFromYear_ / scrollFromMonth_`（滑动起点月份）。
- 时长约 260ms，ease-out cubic（`p = 1 - (1-t)^3`），复用 `kAnimationTimer`（16ms）。
- 每帧绘制两次（统一公式，`dir` 为 `scrollDir_`）：
  - 新月份：offsetY 从 `+kSlideHeight * dir` → 0。
  - 旧月份：offsetY 从 0 → `-kSlideHeight * dir`。
  - 即下一个月时新月份自底部滑入、旧月份向顶部滑出；上一个月相反。
  - `kSlideHeight` = 网格区高度（6 × kCellHeight = 294 逻辑像素）。
- 裁剪：滑动内容用 `PushAxisAlignedClip` 限制在日期网格区
  （y ∈ [kGridTop, kGridTop + 6×kCellHeight]，即 124–418 逻辑像素），
  避免滑进上方表头或下方日程面板（日程面板 y ≥ 432 不参与动画）。
- 月份标题「YYYY年M月」在原位交叉淡化：旧标题 alpha 1→0、新标题 0→1，
  与 `scrollProgress_` 同步；星期表头、分隔线、日程面板全程不动。
- 动画进行中再次翻页：先立即结算当前动画（应用目标月份），再开始新动画。
  快速滚轮表现为逐月推进。
- 开关开启时完全保持现有 `BeginTransition()` 淡入路径，不改动。

### 4. 触发点接入（main.cpp）

- `ChangeMonth(delta)`：计算新 (year, month) 后按 `settings_.monthPaging` 分流——
  关 → `BeginScrollTransition(sign(delta), fromYear, fromMonth)`；开 → 现有 `BeginTransition()`。
- `ChangeYear(delta)`：同样分流，`dir = sign(delta)`（下一年与下一个月同为向上滚）。
- 点击相邻月日期（`WM_LBUTTONUP` 中 `selected_` 与显示月不同步的分支）：同样分流，
  `dir = sign((目标年×12 + 目标月) - (显示年×12 + 显示月))`（跨年如 12 月 → 1 月为正）。
- 键盘 ↑/↓ 与滚轮分别经由 `ChangeYear` / `ChangeMonth`，自动覆盖。

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
- 根 README 功能清单补一句：翻月支持整月切换或平滑滚动（设置中切换）。

## 边界与错误处理

- 旧 settings.json 无 `MonthPaging` 键 → 默认开，无迁移步骤。
- 动画中弹窗被隐藏（outside-click / Esc）→ 现有 `HidePopup` 已 KillTimer，
  动画停止即可，月份状态已是目标值，无残留。
- DPI 变化、主题切换发生在动画中 → 每帧实时重绘两份内容，天然正确。

## 不做的事

- 不改 WPF 归档工程。
- 不做横向滑动、连续手势滚动、速度惯性。
- 不为动画新增独立线程或位图缓存。
