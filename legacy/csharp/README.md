# DropShelf C# 版（已停用）

**这里的代码已不再使用、不再维护，也不会发布。** 从 3.0.0 起，DropShelf 发布的是仓库根目录 `native` 文件夹里的 C 版；新功能和问题修复都只在 C 版里做。

这是 2.x 的 C# / WPF 版，最后一个发布的版本是 v2.8.2。C 版大多数源文件开头写着对应这里的哪一部分，这份代码只留作对照参考。项目文件里的版本号停在搬到这里时的数字，不代表发布的版本。

如果仍想编译（需要 .NET 10 SDK），在本文件夹里运行：

```powershell
dotnet build .\DropShelf.sln -c Release
dotnet run --project .\tests\DropShelf.Tests\DropShelf.Tests.csproj
```

---

**Deprecated.** This is the original C# / WPF version (2.x), last released as v2.8.2. It is no longer used, maintained or released; since 3.0.0 DropShelf ships the C version in the `native` folder at the repository root. The code is kept for reference only.
