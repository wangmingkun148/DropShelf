# 代码签名与上架 winget

这两件事都需要项目维护者自己的账号或证书，仓库里只准备好了接入的位置：没有配置时，发布流程和原来完全一样。

安装包、便携版和提交到 winget 的都是 C 版（`native` 文件夹）：`installer/Build-Installer.ps1` 先用 `native/build.ps1` 编译出一个不依赖其他文件的 `DropShelf.exe`（运行库静态链接，不需要 .NET 或另外安装 VC++ 运行库），放到 `artifacts\win-x64`，再交给 Inno Setup。构建机要有 Visual Studio 2022 或 Build Tools 的 C++ 工具（GitHub 的 `windows-latest` 自带）。原来的 C# 版已停用，放在 `legacy/csharp`，不再发布。

## 代码签名

没有签名的程序第一次运行时会被 SmartScreen 拦一下（README 的“下载”一节有说明）。签名之后这个提示会减少；新证书通常还要积累一段时间的下载量才会完全消失。

构建脚本 `installer/Build-Installer.ps1` 留了一个签名的钩子：设置环境变量 `DROPSHELF_SIGN_COMMAND` 后，它会依次给主程序 `DropShelf.exe`、`卸载 DropShelf.exe` 和安装包签名，命令里的 `{file}` 会换成要签名的文件；没有设置时不签名。命令按 PowerShell 的写法执行。

```powershell
# 本机签名：证书在硬件令牌或证书存储里
$env:DROPSHELF_SIGN_COMMAND = 'signtool sign /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 /a "{file}"'
.\installer\Build-Installer.ps1
```

在 GitHub Actions 里签名：到仓库的 Settings → Secrets and variables → Actions，在 **Variables** 里添加 `SIGN_COMMAND`，内容就是上面那样的命令。`release.yml` 会把它交给构建脚本，另外把 `AZURE_TENANT_ID`、`AZURE_CLIENT_ID`、`AZURE_CLIENT_SECRET` 三个 **Secrets** 传给签名命令（用 Azure 的签名服务时需要，不用就不必设置）。

可以考虑的证书来源（政策和价格以各自官网为准）：

- **SignPath Foundation**：面向开源项目的免费代码签名，需要申请并通过审核。它的做法是把构建产物交给它的服务去签，不是在构建机上运行签名命令，所以接入时要按它的文档在 `release.yml` 里加上传、签名、取回的步骤，上面的钩子用不上。
- **Azure 的代码签名服务**（Trusted Signing）：按月付费，需要验证身份。可以用 `signtool` 加它的插件签名，适合上面的钩子。
- **自己购买的代码签名证书**：现在的证书都放在硬件令牌或云端的密钥库里，一般只能在本机签名，用上面本机签名的写法。

Inno Setup 自己生成的 `unins000.exe` 不经过这个钩子；要连它一起签，需要在 `DropShelf.iss` 里配置 Inno Setup 的 `SignTool`。

签名之后记得把 README“下载”一节里“DropShelf 没有代码签名”的说明改掉。

## winget

第一次提交要手动做一次：

1. 发布一个版本，拿到安装包的下载地址（`https://github.com/<仓库>/releases/download/v<版本号>/DropShelf-Setup-<版本号>.exe`）。
2. 安装 [wingetcreate](https://github.com/microsoft/winget-create)，运行 `wingetcreate new <安装包地址>`，按提示填写包的标识（形如 `发布者.DropShelf`）、名称、许可证（MIT）等。安装包是 Inno Setup 做的、装在当前用户下（不需要管理员权限），工具一般能自动识别。
3. 让它提交，会在 `microsoft/winget-pkgs` 开一个合并请求，等审核通过。

通过之后可以让以后的版本自动提交：

- **Variables** 里添加 `WINGET_PACKAGE_ID`，值是上面填的包标识。
- **Secrets** 里添加 `WINGET_TOKEN`，值是一个有 `public_repo` 权限的 GitHub 个人访问令牌（用来替你开合并请求）。

这两项都设置了，发布 `v` 开头的标签时 `release.yml` 的最后一步会运行 `wingetcreate update … --submit`。这一步失败不影响已经发布的文件，可以到 Actions 的日志里看原因后手动补交。

## Linux 版

发布标签时，`release.yml` 里的 `linux` 任务会在 Arch Linux 容器里编译，并运行 `linux/package-formats.sh` 生成
`DropShelf-<版本号>-x86_64.AppImage` 和 pacman 包 `dropshelf-<版本号>-1-x86_64.pkg.tar.zst`，附加到同一个 Release。
它只构建 x86_64；deb、rpm、便携 tar.gz 或 aarch64 版需要时在对应的机器上运行 `linux/package-formats.sh`、
`linux/package.sh`，或用 `linux/PKGBUILD` 在本机打包。
