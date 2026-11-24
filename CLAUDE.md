# 版本号规则

版本号格式为 `主.次.修订`，版本号写在 `src/DropShelf/DropShelf.csproj`、`src/DropShelf/app.manifest`、`installer/DropShelf.iss`、`installer/UninstallLauncher.iss` 四处，需同步修改。

- 每次修改应用代码，修订号加一（如 2.2.0 → 2.2.1）。
- 某一位加到 10 时归零并向前一位进一（如 2.2.9 → 2.3.0，2.9.9 → 3.0.0）。
- 只改文档或测试不升版本。
- 简洁和纸牌两种风格都在 main 里，只发一种安装包和一个 `v` 开头的标签；style 分支（claude/deck-style）不再单独发布。

# 两种风格

- 改界面时两种风格都要照顾到：差别大的几个文件（`native/src/shelf_window.c`、`shelf_input.c`、`main_pages.c`）用 `#ifdef LOOK_VARIANT_CARDS` 分出纸牌的部分，各编译一遍；置物架动画分成 `shelf_motion.c`（简洁）和 `shelf_motion_cards.c`（纸牌）；其余地方用 `theme_is_cards()` 分支。机制见 `native/src/look_variant.h`。
- 这几个文件里新增对外的函数时，在 `look_variant.h` 里加上改名；别的文件要调用它时，在 `look_dispatch.c` 里加一个按风格转发的函数。

# 沟通

- 遇到任何不确定的事情，直接向用户提问来确定功能和设计改进,用户很喜欢被提问。
