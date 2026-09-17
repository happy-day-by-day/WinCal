# 整月翻滚开关 + 逐周平滑滚动 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 设置中新增「整月翻滚」开关；关闭后翻月动画从「整月替换 + 淡入」变为按翻页方向的逐周连续平滑滚动（下月向上滚、上月向下滚），年切换保留淡入。

**Architecture:** 设置模型加 `monthPaging` bool；设置窗口「常规」页复用现有 owner-draw toggle；日历渲染抽取 `DrawCalendarCell / DrawMonthGrid` 参数化单元格绘制，新增 `DrawWeekRows` 以「周锚点 + 7 天步进」渲染虚拟周行列表并裁剪在网格区；`WM_TIMER` 同时驱动淡入与滚动两种动画。

**Tech Stack:** Win32 + Direct2D/DirectWrite（软件渲染），C++20，MSVC，CMake；测试为冒烟测试（smoke.exe）与设置页离屏渲染（WinCalSettingsPreview）。

**设计文档:** `docs/superpowers/specs/2026-09-17-month-scroll-toggle-design.md`

## Global Constraints

- 只改 `native/` 与根 `README.md`；`archive/wpf/` 不动。
- 设置 JSON 键名固定为 `MonthPaging`，旧配置缺键回落默认 `true`。
- 滚动动画：16ms 定时器（复用 `kAnimationTimer`），步进 `0.058`（≈280ms），ease-out cubic `p = 1-(1-t)^3`。
- 周滚动距离 `D = |WeekRowStart(目标月) - WeekRowStart(源月)| / 7`，钳制 `[1, 6]`（实际恒为 3~5）。
- 裁剪区域固定：逻辑像素 y ∈ [kGridTop=124, kGridTop + 6×kCellHeight=418]，即 `D2D1::RectF(0, Scale(124), Scale(430), Scale(418))`。
- 方向映射（用户确认）：下一个月 = 内容向上滚（`scrollDir_ = +1`），上一个月 = 内容向下滚（`scrollDir_ = -1`）。
- 年切换（↑/↓）与跨年点击在两种模式下都走现有淡入（`BeginTransition()`），不改动。
- 构建一律在仓库根执行：`powershell -Command ".\native\build.ps1 -Configuration Release"`；增量构建测试目标用 `cmake --build native/build --config Release --target <目标>`。
- 工作区中 `native/src/main.cpp` 有一处用户未提交的「日程空状态文案」简化改动，Task 0 先单独提交它，后续提交才不会混入无关内容。
- 每个任务结束提交一次（git add 指定文件，不用 `git add -A`），不 push。

---

### Task 0: 提交工作区遗留改动

**Files:**
- Modify（已改，待提交）: `native/src/main.cpp`

**Interfaces:**
- Consumes: 无
- Produces: 干净的工作区，后续任务的提交只包含自己的改动

- [ ] **Step 1: 确认遗留改动内容**

Run: `git -C "D:\code\my\WinCal" diff native/src/main.cpp`
Expected: 唯一改动为日程区空状态文案简化（删除 `EventCount`/`IcsEventCount`/`SystemEventCount` 相关的状态拼串，改为三个固定文案常量）。若出现其他改动，停止并向用户确认。

- [ ] **Step 2: 单独提交**

```bash
cd "D:\code\my\WinCal"
git add native/src/main.cpp
git commit -m "refactor: 简化日程空状态文案"
```

Expected: commit 成功，`git status --short` 无输出。

---

### Task 1: 设置模型 monthPaging

**Files:**
- Modify: `native/src/app_settings.h:19`（`weekStartDay` 之后）
- Modify: `native/src/app_settings.cpp:238`（`Load`）、`native/src/app_settings.cpp:258`（`Save`）
- Test: `native/tests/smoke.cpp`（settings 检查之后）

**Interfaces:**
- Consumes: 现有 `SettingsStore::Load/Save`、`BoolValue` 辅助
- Produces: `wincal::AppSettings::monthPaging`（bool，默认 `true`）；JSON 键 `MonthPaging`。Task 2、Task 4 依赖该字段。

- [ ] **Step 1: 写失败的测试（smoke 断言默认值）**

在 `native/tests/smoke.cpp` 中，ICS alias 检查之后、`std::cout` 输出之前插入：

```cpp
    wincal::AppSettings defaults;
    if (!defaults.monthPaging)
    {
        std::wcerr << L"MonthPaging default should be true.\n";
        return 4;
    }
```

- [ ] **Step 2: 运行测试确认编译失败**

Run: `cmake --build native/build --config Release --target WinCalNativeSmoke`
Expected: FAIL，`error C2039: "monthPaging": 不是 "wincal::AppSettings" 的成员`（或等价错误）。

- [ ] **Step 3: 最小实现**

`native/src/app_settings.h` 中 `weekStartDay` 行后加：

```cpp
    bool monthPaging{true};
```

`native/src/app_settings.cpp` 的 `Load()` 中 `weekStartDay` 行后加：

```cpp
    settings.monthPaging = BoolValue(content, "MonthPaging", true);
```

`Save()` 中把最后一行改为两行（WeekStartDay 行加逗号，新增 MonthPaging 为末项）：

```cpp
    json += "  \"WeekStartDay\": \"" + EscapeJson(settings.weekStartDay) + "\",\n";
    json += "  \"MonthPaging\": " + std::string(settings.monthPaging ? "true" : "false") + "\n}\n";
```

- [ ] **Step 4: 运行测试确认通过**

Run: `cmake --build native/build --config Release --target WinCalNativeSmoke && ./native/build/Release/WinCalNativeSmoke.exe`
Expected: PASS，输出 `lunarLength=...; sourceLength=...`，退出码 0。

- [ ] **Step 5: 提交**

```bash
cd "D:\code\my\WinCal"
git add native/src/app_settings.h native/src/app_settings.cpp native/tests/smoke.cpp
git commit -m "feat: 设置模型新增整月翻滚开关"
```

---

### Task 2: 设置窗口「整月翻滚」开关

**Files:**
- Modify: `native/src/main.cpp`（控件 ID 常量、`CreateSettingsControls`、常规页绘制、`PopulateSettingsControls`、`SaveSettingsFromControls`、WM_COMMAND toggle 分支、`DrawSettingsButton` toggle 分支）

**Interfaces:**
- Consumes: Task 1 的 `AppSettings::monthPaging`；现有 `button()` lambda、`SetSettingSelection/SettingSelection`、`SettingsText`
- Produces: 控件 ID `kSettingMonthPaging = 2008`；保存流程写入 `settingsDraft_.monthPaging`。Task 4 依赖保存后的 `settings_.monthPaging`。

- [ ] **Step 1: 新增控件 ID**

`constexpr int kSettingWeekStart = 2007;` 之后加：

```cpp
constexpr int kSettingMonthPaging = 2008;
```

- [ ] **Step 2: 创建控件**

`CreateSettingsControls` 中 `button(L"", kSettingWeekStart, 266, 270, 200, 38, 2);` 之后加：

```cpp
        button(L"整月翻滚", kSettingMonthPaging, 410, 352, 56, 30, 2);
```

- [ ] **Step 3: 常规页文字标签**

常规页绘制分支中（`SettingsText(dc, L"选择日历每一行的第一天", 48, 294, 210, 20, settingsCaptionFont_, p.secondary);` 之后）加：

```cpp
            SettingsText(dc, L"整月翻滚", 48, 352, 320, 24, settingsBodyFont_, p.primary);
            SettingsText(dc, L"关闭后翻月时逐周平滑滚动", 48, 378, 338, 20, settingsCaptionFont_, p.secondary);
```

（布局核对：与上方「每周开始」下拉框 y270–308 无重叠，与下方 y446 的底部说明无重叠；toggle x=410 与「开机启动」对齐。）

- [ ] **Step 4: 回填与保存**

`PopulateSettingsControls` 中 `SetSettingSelection(window, kSettingWeekStart, ...)` 行后加：

```cpp
        SetSettingSelection(window, kSettingMonthPaging, settingsDraft_.monthPaging ? 1 : 0);
```

`SaveSettingsFromControls` 中 `settingsDraft_.weekStartDay = ...` 行后加：

```cpp
        settingsDraft_.monthPaging = SettingSelection(window, kSettingMonthPaging) != 0;
```

- [ ] **Step 5: 点击翻转与开关绘制**

WM_COMMAND 中把 `if (id == kSettingAutoStartup)` 改为：

```cpp
                if (id == kSettingAutoStartup || id == kSettingMonthPaging)
```

`DrawSettingsButton` 中把 toggle 判定改为：

```cpp
        const bool toggle = id == kSettingAutoStartup || id == kSettingMonthPaging;
```

（「恢复默认」无需改动：`settingsDraft_ = {}` 经成员初始化回到 `monthPaging = true`。）

- [ ] **Step 6: 离屏渲染验证**

Run:
```
cmake --build native/build --config Release --target WinCalSettingsPreview
./native/build/Release/WinCalSettingsPreview.exe 120 2 0 7 native/out/settings-general-light.bmp
./native/build/Release/WinCalSettingsPreview.exe 120 2 1 7 native/out/settings-general-dark.bmp
```
Expected: 两条命令退出码均为 0，生成两个 BMP；用图像查看确认常规页出现「整月翻滚」行与右侧开关、深浅色下均无重叠或截断。

- [ ] **Step 7: 提交**

```bash
cd "D:\code\my\WinCal"
git add native/src/main.cpp
git commit -m "feat: 设置常规页新增整月翻滚开关"
```

---

### Task 3: 渲染参数化重构（行为不变）

**Files:**
- Modify: `native/src/main.cpp`（`DateForCell`（约 2253 行）、`Paint()` 网格循环（约 2417–2498 行）、`WM_LBUTTONUP` 中的 `DateForCell(hit)`（约 2656 行））

**Interfaces:**
- Consumes: 现有 `DaysFromCivil/CivilFromDays`、`Brush`、`DrawText`、`hoveredCell_`、`selected_`
- Produces（Task 4 依赖，签名必须一致）:
  - `Date DateForCell(int year, int month, int index) const`
  - `void DrawCalendarCell(const Theme& theme, const Date& today, const Date& date, bool inMonth, int column, float topLogical, bool hovered, float alpha)`
  - `void DrawMonthGrid(const Theme& theme, const Date& today, int year, int month, float alpha)`

- [ ] **Step 1: 参数化 DateForCell**

把现有 `Date DateForCell(int index) const` 整体替换为：

```cpp
    Date DateForCell(int year, int month, int index) const
    {
        const long long first = DaysFromCivil(year, static_cast<unsigned>(month), 1);
        const int mondayBasedWeekday = static_cast<int>((first + 3) % 7 + 7) % 7;
        const int firstColumn = weekStartsMonday_ ? mondayBasedWeekday : (mondayBasedWeekday + 1) % 7;
        return CivilFromDays(first - firstColumn + index);
    }
```

`WM_LBUTTONUP` 中 `selected_ = DateForCell(hit);` 改为：

```cpp
                selected_ = DateForCell(displayYear_, displayMonth_, hit);
```

- [ ] **Step 2: 抽取单元格绘制**

新增成员函数（放在 `DateForCell` 之后），函数体来自现有 42 格循环体，把 `contentOpacity` 换成形参 `alpha`、`index == hoveredCell_` 换成形参 `hovered`、`displayMonth_` 换成形参 `month` 语义（由调用方传 `inMonth`）、行 top 换成形参 `topLogical`：

```cpp
    void DrawCalendarCell(const Theme& theme, const Date& today, const Date& date,
                          bool inMonth, int column, float topLogical, bool hovered, float alpha)
    {
        const float left = Scale(static_cast<float>(kGridLeft + column * kCellWidth));
        const float top = Scale(topLogical);
        const auto rect = D2D1::RectF(
            left + Scale(3), top + Scale(3), left + Scale(kCellWidth - 3), top + Scale(kCellHeight - 3));
        const bool isToday = date == today;
        const bool selected = date == selected_;
        const bool hasEvents = calendarData_.HasEvents(date.year, date.month, date.day);
        const auto schedule = calendarData_.ScheduleForDate(date.year, date.month, date.day);

        if (hovered)
        {
            const auto hover = Brush(theme.hover, alpha);
            renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(rect, Scale(9), Scale(9)), hover.Get());
        }
        if (selected)
        {
            const auto accent = Brush(theme.accent, alpha);
            renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(rect, Scale(9), Scale(9)), accent.Get());
        }
        else if (isToday)
        {
            const auto accent = Brush(theme.accent, alpha);
            renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(rect, Scale(9), Scale(9)), accent.Get(), Scale(1.5f));
        }

        wchar_t number[4]{};
        swprintf_s(number, L"%d", date.day);
        DrawText(
            number,
            dayFormat_.Get(),
            D2D1::RectF(rect.left, rect.top + Scale(1), rect.right, rect.top + Scale(25)),
            selected ? D2D1::ColorF(0xFFFFFF) : (inMonth ? theme.primary : theme.muted),
            alpha);

        const auto lunar = lunarCalendar_.TextForDate(date.year, date.month, date.day);
        if (!lunar.empty())
        {
            DrawText(
                lunar.c_str(),
                lunarFormat_.Get(),
                D2D1::RectF(rect.left + Scale(1), rect.top + Scale(22), rect.right - Scale(1), rect.bottom - Scale(7)),
                selected ? D2D1::ColorF(0xFFFFFF) : (inMonth ? theme.secondary : theme.muted),
                alpha);
        }

        if (schedule != wincal::ScheduleLabel::None)
        {
            const bool isRest = schedule == wincal::ScheduleLabel::Rest;
            const auto badgeRect = D2D1::RectF(
                rect.right - Scale(17), rect.top + Scale(1),
                rect.right - Scale(1), rect.top + Scale(17));
            const auto badgeBackground = Brush(
                isRest ? theme.restBadgeBackground : theme.workBadgeBackground,
                alpha);
            renderTarget_->FillRoundedRectangle(
                D2D1::RoundedRect(badgeRect, Scale(4), Scale(4)), badgeBackground.Get());
            DrawText(
                isRest ? L"休" : L"班",
                badgeFormat_.Get(), badgeRect,
                isRest ? theme.restBadgeForeground : theme.workBadgeForeground,
                alpha);
        }

        if (hasEvents)
        {
            const auto dot = Brush(
                selected ? D2D1::ColorF(0xFFFFFF) : theme.eventDot,
                alpha);
            renderTarget_->FillEllipse(
                D2D1::Ellipse(
                    D2D1::Point2F((rect.left + rect.right) / 2, rect.bottom - Scale(3)),
                    Scale(1.75f), Scale(1.75f)),
                dot.Get());
        }
    }
```

- [ ] **Step 3: 抽取整月网格绘制**

```cpp
    void DrawMonthGrid(const Theme& theme, const Date& today, int year, int month, float alpha)
    {
        for (int index = 0; index < 42; ++index)
        {
            const int row = index / 7;
            const int column = index % 7;
            const auto date = DateForCell(year, month, index);
            DrawCalendarCell(
                theme, today, date, date.month == month, column,
                static_cast<float>(kGridTop + row * kCellHeight), index == hoveredCell_, alpha);
        }
    }
```

- [ ] **Step 4: Paint 接入**

`Paint()` 中删除 `const float contentOpacity = 0.45f + transition_ * 0.55f;` 与整个 42 格 `for` 循环（原 2417–2498 行），原位置替换为：

```cpp
        DrawMonthGrid(theme, today, displayYear_, displayMonth_, 0.45f + transition_ * 0.55f);
```

（`today` 变量在 `Paint()` 前部已有：`const auto today = Today();`，直接传用。）

- [ ] **Step 5: 行为不变验证**

Run:
```
cmake --build native/build --config Release --target WinCalNative
cmake --build native/build --config Release --target WinCalSettingsPreview
./native/build/Release/WinCalSettingsPreview.exe 120 2 0 7 native/out/refactor-page2.bmp
```
Expected: 编译 0 error；运行 `./native/out/WinCal.exe`，滚轮/方向键翻月仍为整月替换 + 淡入，今日描边、选中、悬停、休班角标、事件圆点、农历均正常。

- [ ] **Step 6: 提交**

```bash
cd "D:\code\my\WinCal"
git add native/src/main.cpp
git commit -m "refactor: 参数化日历单元格与整月网格绘制"
```

---

### Task 4: 逐周平滑滚动动画与触发点

**Files:**
- Modify: `native/src/main.cpp`（自由函数区（`CivilFromDays` 后，约 165 行）、成员变量区（约 2830 行 `transition_` 后）、`BeginTransition` 后新增 `BeginScrollTransition`、`DrawWeekRows`、`Paint()` 标题与网格分支、`WM_TIMER`、`ChangeMonth`、`WM_LBUTTONUP` 相邻月分支、`ShowPopup`/`HidePopup` 动画收尾）

**Interfaces:**
- Consumes: Task 3 的 `DrawCalendarCell`；Task 1/2 的 `settings_.monthPaging`；现有 `kAnimationTimer`、`CivilFromDays/DaysFromCivil`
- Produces: `WeekRowStartDays(int year, int month)`、`BeginScrollTransition(int dir, int fromYear, int fromMonth)`、`DrawWeekRows(const Theme&, const Date&)`、成员 `scrollAnimating_/scrollProgress_/scrollDir_/scrollFromYear_/scrollFromMonth_/scrollRows_`

- [ ] **Step 1: 缓动函数**

`CivilFromDays` 函数之后（自由函数区）加：

```cpp
float EaseOutCubic(float t)
{
    const float inverse = 1.0f - t;
    return 1.0f - inverse * inverse * inverse;
}
```

- [ ] **Step 2: 周锚点与动画状态**

`App` 类内（`DateForCell` 附近）加成员函数：

```cpp
    long long WeekRowStartDays(int year, int month) const
    {
        const long long first = DaysFromCivil(year, static_cast<unsigned>(month), 1);
        const int mondayBasedWeekday = static_cast<int>((first + 3) % 7 + 7) % 7;
        const int backToWeekStart = weekStartsMonday_ ? mondayBasedWeekday : (mondayBasedWeekday + 1) % 7;
        return first - backToWeekStart;
    }
```

成员变量区 `float transition_{1.0f};` 之后加：

```cpp
    bool scrollAnimating_{};
    float scrollProgress_{};
    int scrollDir_{1};
    int scrollFromYear_{};
    int scrollFromMonth_{};
    int scrollRows_{};
```

- [ ] **Step 3: 启动滚动动画**

`BeginTransition()` 之后加：

```cpp
    void BeginScrollTransition(int dir, int fromYear, int fromMonth)
    {
        scrollDir_ = dir;
        scrollFromYear_ = fromYear;
        scrollFromMonth_ = fromMonth;
        const long long delta =
            WeekRowStartDays(displayYear_, displayMonth_) - WeekRowStartDays(fromYear, fromMonth);
        const long long rows = (delta < 0 ? -delta : delta) / 7;
        scrollRows_ = static_cast<int>(std::clamp(rows, 1LL, 6LL));
        scrollProgress_ = 0.0f;
        scrollAnimating_ = true;
        transition_ = 1.0f;
        hoveredCell_ = -1;
        SetTimer(window_, kAnimationTimer, 16, nullptr);
        InvalidateRect(window_, nullptr, FALSE);
    }
```

- [ ] **Step 4: 周行渲染**

`DrawMonthGrid` 之后加（裁剪矩形遵循 Global Constraints）：

```cpp
    void DrawWeekRows(const Theme& theme, const Date& today)
    {
        const float position = scrollDir_ * scrollRows_ * EaseOutCubic(scrollProgress_);
        const long long anchor = WeekRowStartDays(scrollFromYear_, scrollFromMonth_);
        const int firstRow = static_cast<int>(std::floor(position));
        const float gridBottom = static_cast<float>(kGridTop + 6 * kCellHeight);
        const auto clip = D2D1::RectF(0, Scale(kGridTop), Scale(kLogicalWidth), Scale(gridBottom));
        renderTarget_->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
        for (int row = firstRow; row <= firstRow + 7; ++row)
        {
            const float top = kGridTop + (row - position) * kCellHeight;
            if (top >= gridBottom || top + kCellHeight <= kGridTop)
                continue;
            for (int column = 0; column < 7; ++column)
            {
                const Date date = CivilFromDays(anchor + row * 7 + column);
                DrawCalendarCell(theme, today, date, date.month == displayMonth_,
                                 column, top, false, 1.0f);
            }
        }
        renderTarget_->PopAxisAlignedClip();
    }
```

- [ ] **Step 5: 标题交叉淡化与网格分支**

`Paint()` 中把标题绘制（原 `DrawText(title, titleFormat_.Get(), D2D1::RectF(Scale(22), Scale(kHeaderTop), Scale(260), Scale(62)), theme.primary);`）替换为：

```cpp
        const auto titleRect = D2D1::RectF(Scale(22), Scale(kHeaderTop), Scale(260), Scale(62));
        if (scrollAnimating_)
        {
            const float progress = EaseOutCubic(scrollProgress_);
            wchar_t fromTitle[64]{};
            swprintf_s(fromTitle, L"%d年%d月", scrollFromYear_, scrollFromMonth_);
            if (progress < 1.0f)
                DrawText(fromTitle, titleFormat_.Get(), titleRect, theme.primary, 1.0f - progress);
            if (progress > 0.0f)
                DrawText(title, titleFormat_.Get(), titleRect, theme.primary, progress);
        }
        else
        {
            DrawText(title, titleFormat_.Get(), titleRect, theme.primary);
        }
```

Task 3 放入的 `DrawMonthGrid(...)` 调用替换为：

```cpp
        if (scrollAnimating_)
            DrawWeekRows(theme, today);
        else
            DrawMonthGrid(theme, today, displayYear_, displayMonth_, 0.45f + transition_ * 0.55f);
```

- [ ] **Step 6: 定时器推进**

`WM_TIMER` 的 `kAnimationTimer` 分支替换为：

```cpp
            if (wParam == kAnimationTimer)
            {
                if (scrollAnimating_)
                {
                    scrollProgress_ = std::min(1.0f, scrollProgress_ + 0.058f);
                    if (scrollProgress_ >= 1.0f)
                    {
                        scrollAnimating_ = false;
                        KillTimer(window_, kAnimationTimer);
                    }
                }
                else
                {
                    transition_ = std::min(1.0f, transition_ + 0.055f);
                    if (transition_ >= 1.0f)
                        KillTimer(window_, kAnimationTimer);
                }
                InvalidateRect(window_, nullptr, FALSE);
                return 0;
            }
```

- [ ] **Step 7: 触发点分流**

`ChangeMonth` 整体替换为：

```cpp
    void ChangeMonth(int delta)
    {
        const int fromYear = displayYear_;
        const int fromMonth = displayMonth_;
        int value = displayYear_ * 12 + displayMonth_ - 1 + delta;
        displayYear_ = value / 12;
        displayMonth_ = value % 12 + 1;
        if (displayMonth_ <= 0)
        {
            displayMonth_ += 12;
            --displayYear_;
        }
        RequestSystemCalendarMonth(displayYear_, displayMonth_);
        if (!settings_.monthPaging)
            BeginScrollTransition(delta > 0 ? 1 : -1, fromYear, fromMonth);
        else
            BeginTransition();
    }
```

`WM_LBUTTONUP` 网格命中分支（`CloseDetailWindow()` 到 `InvalidateRect` 之间）替换为：

```cpp
            if (hit >= 0)
            {
                CloseDetailWindow();
                selected_ = DateForCell(displayYear_, displayMonth_, hit);
                selectedEventScroll_ = 0;
                if (selected_.year != displayYear_ || selected_.month != displayMonth_)
                {
                    const int fromYear = displayYear_;
                    const int fromMonth = displayMonth_;
                    displayYear_ = selected_.year;
                    displayMonth_ = selected_.month;
                    const int monthDelta =
                        (selected_.year * 12 + selected_.month) - (fromYear * 12 + fromMonth);
                    if (!settings_.monthPaging && (monthDelta == 1 || monthDelta == -1))
                        BeginScrollTransition(monthDelta > 0 ? 1 : -1, fromYear, fromMonth);
                    else
                        BeginTransition();
                }
                InvalidateRect(window_, nullptr, FALSE);
            }
```

（`ChangeYear` 不改：年切换在两种模式下都走淡入。）

- [ ] **Step 8: 弹窗收起时的动画收尾**

`ShowPopup` 中 `transition_ = 0.0f;` 之前加：

```cpp
        scrollAnimating_ = false;
```

`HidePopup` 中 `KillTimer(window_, kAnimationTimer);` 之后加：

```cpp
        scrollAnimating_ = false;
```

（防止隐藏打断动画后残留 `scrollAnimating_`，下次显示卡在中间帧。）

- [ ] **Step 9: 构建与手工验证**

Run:
```
cmake --build native/build --config Release --target WinCalNative
./native/out/WinCal.exe
```
手工清单（设置中先关闭「整月翻滚」并保存）：
1. 滚轮向下翻（→下月）：网格连续向上滚，中间周真实滚过，标题交叉淡化，星期表头与日程面板不动；
2. 滚轮向上翻（→上月）：内容向下滚；
3. ←/→ 方向键与滚轮一致；↑/↓ 年切换为淡入；
4. 点击相邻月日期：按月份差方向逐周滚动；点击跨年日期（如 2026-12-31 视图点 2027-01-01）：淡入；
5. 连续快速滚轮：逐月推进不卡帧、不错位；
6. 动画中按 Esc 隐藏再唤出：正常回到当月，无残影；
7. 打开「整月翻滚」保存后：恢复整月切换 + 淡入。
另外核对边界月份：2027-01→2027-02（5 行距离）、2027-02→2027-03（4 行）、2026-12→2027-01（跨年相邻月，4~5 行），滚动结束帧与静态网格逐格对齐。

- [ ] **Step 10: 提交**

```bash
cd "D:\code\my\WinCal"
git add native/src/main.cpp
git commit -m "feat: 翻月支持逐周平滑滚动动画"
```

---

### Task 5: 文档与全量验证

**Files:**
- Modify: `README.md:8`、`README.md:12`、`native/README.md:16`、`native/README.md:22`

**Interfaces:**
- Consumes: 全部前置任务
- Produces: 与实现一致的文档

- [ ] **Step 1: 更新根 README 功能清单**

`README.md` 第 8 行改为：

```markdown
- 月份、年份键盘切换，鼠标滚轮切换月份（可在设置中切换整月切换或逐周平滑滚动），打开时回到当前月份
```

第 12 行改为：

```markdown
- 原生设置窗口：主题、字号、数据源、ICS 链接、刷新频率、周起始日和整月翻滚
```

- [ ] **Step 2: 更新 native/README**

第 16 行改为：

```markdown
- 方向键切换月份/年份，鼠标滚轮切换月份；关闭「整月翻滚」后按翻页方向逐周平滑滚动（年切换仍为淡入）
```

第 22 行改为：

```markdown
- 原生设置窗口：主题、实时文字大小预览、数据源、ICS 链接与刷新频率、周起始日、整月翻滚
```

- [ ] **Step 3: 全量验证**

Run（仓库根）:
```
powershell -Command ".\native\build.ps1 -Configuration Release"
cmake --build native/build --config Release --target WinCalNativeSmoke
./native/build/Release/WinCalNativeSmoke.exe
cmake --build native/build --config Release --target WinCalSettingsPreview
./native/build/Release/WinCalSettingsPreview.exe 120 0 0 7 native/out/final-appearance.bmp
./native/build/Release/WinCalSettingsPreview.exe 120 1 0 7 native/out/final-source.bmp
./native/build/Release/WinCalSettingsPreview.exe 120 2 1 7 native/out/final-general.bmp
```
Expected: 全部退出码 0；smoke 输出正常；`git status --short` 仅剩本次文档改动。

- [ ] **Step 4: 提交**

```bash
cd "D:\code\my\WinCal"
git add README.md native/README.md
git commit -m "docs: 说明整月翻滚开关与逐周平滑滚动"
```
