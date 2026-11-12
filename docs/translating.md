# 翻译 DropShelf / Translating DropShelf

[English below](#english)

DropShelf 的中文和英文写在程序里（`src/DropShelf/Localization/Strings.cs`），其他语言用**语言包**：一个 JSON 文件，放进文件夹就能用，不用改程序、不用重新编译。

## 做一个语言包

1. 打开主窗口右下角的版本号，在“关于”窗口的“语言包”一栏点“导出翻译模板”。得到的 `template.json` 列出了全部界面文字：
   - `strings`：左边是原文，右边留空，等你填译文。
   - `reference`：同一条文字的另一种语言，供对照，读取时会忽略。
   - `from`：对照哪种语言翻译，`zh` 是中文，`en` 是英文。界面是中文时导出的模板对照中文，是英文时对照英文。
2. 把 `name` 填成这种语言自己的名称（比如 `日本語`），把 `strings` 里的译文填上，文件改名为语言代码，比如 `ja.json`、`zh-TW.json`。
3. 放进“语言包”一栏显示的文件夹（`%LOCALAPPDATA%\DropShelf\Languages`），或者程序所在文件夹里的 `Languages` 文件夹。
4. 重新打开主窗口，“常规”页会多出“语言包”一项，选上就生效。改了文件之后，重新打开主窗口即可看到新的译文。

```json
{
  "name": "日本語",
  "from": "zh",
  "strings": {
    "置物架": "シェルフ",
    "已添加 {0} 项": "{0} 件追加しました"
  }
}
```

## 规则

- 带参数的文字里，`{0}`、`{1}` 是参数的位置，译文里照样写，顺序可以调换；`{0:0.#}` 这样带格式的，冒号后面的格式可以保留也可以去掉。
- 要显示花括号本身时写两个：`{{files}}` 显示为 `{files}`。没有参数的文字（原文里没有 `{0}` 这样的位置）照原样写，不用加倍。
- 没填的、留空的条目显示原文；原文在新版本里改了字，对应的译文就失效，同样显示原文，重新导出模板可以看到新的原文。
- 译文里参数位置写错（比如用了不存在的 `{5}`）时这一条显示原文，不会出错。
- 对照英文翻译时，数量相关的文字里有的参数已经是拼好的英文（比如 `3 items`）。中文原文里的参数都是单纯的数字和名称，所以没有单复数变化的语言建议对照中文翻译。
- 语言包只是文字，不含代码；超过 2 MB 的文件不会读取。

做好的语言包欢迎提交到仓库的 `Languages` 文件夹。

---

<a id="english"></a>

## English

DropShelf's Chinese and English text lives in the program (`src/DropShelf/Localization/Strings.cs`). Any other language comes from a **language pack**: a JSON file you drop into a folder. No code changes, no rebuild.

### Make a language pack

1. Click the version number at the bottom right of the main window, then "Export template" in the "Language pack" row of the About window. The resulting `template.json` lists every piece of interface text:
   - `strings`: the source text on the left and an empty value on the right for your translation.
   - `reference`: the same text in the other built-in language, for comparison. It's ignored when the pack is loaded.
   - `from`: which language you're translating from, `zh` for Chinese or `en` for English. The template follows the language the interface is showing when you export it.
2. Set `name` to the language's own name (for example `Deutsch`), fill in the translations, and rename the file to the language code, such as `de.json`.
3. Put it in the folder shown in the "Language pack" row (`%LOCALAPPDATA%\DropShelf\Languages`), or in a `Languages` folder next to the program.
4. Reopen the main window. A "Language pack" setting appears on the General page; pick your pack. After editing the file, reopen the main window to see the changes.

### Rules

- In text with parameters, `{0}` and `{1}` mark where values go. Keep them in your translation; you may reorder them. In `{0:0.#}`, the format after the colon can be kept or dropped.
- To show a literal brace, double it: `{{files}}` shows as `{files}`. Text without parameters is written as is, with no doubling.
- Entries left empty show the source text. If a later version changes the source wording, the old translation no longer matches and the source text shows; export the template again to see the new wording.
- If a translation refers to a parameter that doesn't exist (say `{5}`), that entry falls back to the source text.
- When translating from English, some count-related parameters are already assembled English phrases (such as `3 items`). The Chinese source passes plain numbers and names, so languages without plural forms are best translated from Chinese.
- A language pack is text only and can't run code. Files larger than 2 MB are not loaded.

Finished packs are welcome in the repository's `Languages` folder.
