# 版本号规则

版本号格式为 `主.次.修订`，版本号写在 `src/DropShelf/DropShelf.csproj`、`src/DropShelf/app.manifest`、`installer/DropShelf.iss`、`installer/UninstallLauncher.iss` 四处，需同步修改。

- 每次修改应用代码，修订号加一（如 2.2.0 → 2.2.1）。
- 某一位加到 10 时归零并向前一位进一（如 2.2.9 → 2.3.0，2.9.9 → 3.0.0）。
- 只改文档或测试不升版本。
