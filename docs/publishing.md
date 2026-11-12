# 代码签名与上架 winget、Scoop

这三件事都需要项目维护者自己的账号或证书，仓库里只准备好了接入的位置：没有配置时，发布流程和原来完全一样。

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

## Scoop

发布 `v` 开头的标签时，`release.yml` 会生成一份 Scoop 清单 `dropshelf.json` 附在 Release 上，指向这一版的便携版并带有校验值。不需要任何配置，用下面的命令就能安装：

```powershell
scoop install https://github.com/wangmingkun148/dropshelf/releases/latest/download/dropshelf.json
```

想让用户用 `scoop install dropshelf` 这样的短名字安装，需要把清单放进一个 bucket：可以自己建一个 bucket 仓库放这份清单（清单里的 `checkver` 和 `autoupdate` 已经写好，bucket 的自动更新能跟上新版本），或者按 Scoop 官方 bucket 的收录要求提交到 `extras`。

上架之后可以在 README 的“下载”一节补上对应的安装命令。
