# DropShelf for Windows

[简体中文](README.md) | English

DropShelf is a temporary shelf for files on Windows. While dragging files, shake the mouse or pause at a screen edge, and a shelf appears. Drop your files on it, switch to the window you need, then drag them all to File Explorer, a chat app or a browser in one go.

**Highlights**

- Appears only while you drag, without slowing the mouse or popping up by mistake.
- Takes files, folders, images, text, links and attachments from browsers, mail and chat apps.
- Built-in quick actions such as compress, extract and move to, plus your own custom actions.
- Shelves closed by mistake can be reopened.
- Smooth animations: cards and pages follow your mouse as you drag.
- No admin rights needed, never reads file contents, never goes online.

There are already plenty of apps like this, and DropShelf may look like reinventing the wheel, but it tries to improve on some of their shortcomings. Inspired by [Dropover](https://dropoverapp.com) on macOS. DropShelf is an independent project and is not affiliated with Dropover.

**Notes**

- The code of this project was written by generative AI.
- The Chinese interface text has been proofread by hand; the English text was translated by AI.
- Developed and optimized for Windows 11. Windows 10 and earlier have not been tested.

## Download

Get the latest version from [Releases](https://github.com/wangmingkun148/dropshelf/releases/latest):

- `DropShelf-Setup-<version>.exe`: installer (in Simplified Chinese).
- `DropShelf-<version>-portable.zip`: portable version. Unzip it and run `DropShelf.exe`.

DropShelf is not code-signed, so Windows may show "Windows protected your PC" the first time you run it. SmartScreen shows this for new programs that haven't been downloaded much yet; it doesn't mean a virus was found. Click "More info", then "Run anyway". If your browser says the file isn't commonly downloaded, choose "Keep".

## Getting started

1. Install and run it. After you close the main window, DropShelf keeps running in the notification area.
2. Start dragging files and quickly shake the mouse back and forth: a shelf appears at the cursor. You can also drag to the left, right or top edge of the screen and pause.
3. Drop the files on the shelf. Later, drag the thumbnail stack to take all files at once, or click the list button at the top to open the list and drag out one or more items.
4. Press `Ctrl+Shift+Space` at any time to open an empty shelf, then press `Ctrl+V` to paste files, images or text.

## Features

### Opening a shelf
- Open shelves by Shake, Screen edge or Both.
- Shakes can be horizontal, vertical or diagonal. A slider sets the sensitivity anywhere from Steady to Responsive, and settings show how far and how many times you need to shake.
- Add excluded apps: while one of them is in the foreground or you drag over its windows, no shelf opens and the keyboard shortcut is left to the app. Handy for games and drawing apps.
- Left, right and top screen edges are supported, with a pause of 0.3, 0.6 or 1 s; edges with the taskbar and edges shared between monitors don't trigger.

### The shelf
- When collapsed, it shows a thumbnail stack, the item count and the total size. If it holds both files and text, only the files are dragged out.
- Dropped files fly from where you let go into the stack, or to the start of their own row when the list is open; when you drop several, up to 3 land one after another.
- The list button in the title bar (also in the "⋯" menu) switches between the thumbnail stack and the list; the window resizes smoothly and each card flies to its own row. In the list you can multi-select, drag out the selection, double-click to open or right-click for more.
- Dragging out moves files by default. Switch with settings or the Move/Copy button at the top of the shelf, or hold Ctrl or Shift to change it temporarily; chat apps and browsers get a copy automatically.
- While dragging in, move to the bottom button bar to reveal quick actions and drop directly on one.
- "Close empty shelves" is on by default: dragged-out files leave the shelf and an empty shelf closes. With it off, copied files stay on the shelf. A pinned shelf never closes when empty.
- Chat app support: temporary images and files deleted right after the drop are saved; PNG, JPG, GIF and DIB image data and older QQ versions are supported; temporary attachments dragged or copied to chat apps are kept for 10 more minutes.
- Shelves that still hold files when closed are kept under "Recently closed shelves" (up to 10) and can be reopened from the Overview page in settings, the tray menu or the shelf's "⋯" menu. You can also drag a card out of the Overview page to reopen the shelf where you let go, or drop it back anywhere in the stack, where the other cards make room for it.
- Several shelves can be open at once, in Small, Medium or Large.
- Shortcuts: `Esc` collapses or closes, `Delete` removes the selection, `Ctrl+A` selects all, `Ctrl+C` copies, `Ctrl+V` pastes, `Enter` opens.

### Quick actions
- The bottom of the shelf shows 0, 2 or 4 quick actions; drag cards in settings to replace or swap them.
- Built-in actions: compress/extract (extracts when the only item is a ZIP or 7z), copy path, move to, properties, show in File Explorer.
- Custom actions:
  - Send to folder: move or copy to a fixed folder with one click.
  - Compress image: compress to a JPG no larger than a given size (e.g. 200 KB), replacing the image on the shelf while the original file stays unchanged.
  - Open with app: hand files to a chosen program; use `{files}` to place them in the arguments.
  - To delete one, drag it to the red area at the bottom of the settings window and confirm.
- Click a button to process the items on the shelf, or only the selected ones when the list is expanded.
- Created ZIP files and extracted folders go to a new shelf; existing files with the same name are never overwritten.

### Settings and appearance
- Light (ivory background), Dark or System theme; the accent color follows Windows or can be one of eight colors.
- English and Simplified Chinese interface, following the system language by default.
- Settings apply immediately and are saved automatically.
- A tutorial is shown on first run and can be viewed again from General.

### System integration
- Tray icon: left-click opens settings; right-click to create a shelf, reopen a recently closed one or quit.
- Optional start with Windows.
- The installer can add DropShelf to the "Send to" menu. Dropping files on the program icon or running `DropShelf.exe --shelf files...` also opens a shelf.

## Privacy and data safety

- Files are recorded by path only and their contents are never read; removing items or clearing a shelf doesn't affect files on disk.
- Temporary attachments and dropped images are cached in the system temp folder and cleaned up after the shelf closes.
- "Recently closed shelves" records only paths and names of files and folders, and can be cleared in settings.
- Mouse positions are only used to detect gestures and are never saved.

## Install and uninstall

DropShelf installs to `%LOCALAPPDATA%\Programs\DropShelf` without admin rights; install over an existing copy to upgrade. Uninstall from Settings > Apps > Installed apps or the Start menu; your settings and logs are kept by default. For the portable version, quit it and delete the folder.

## Data locations

- Settings: `%LOCALAPPDATA%\DropShelf\settings.json`
- Recently closed shelves: `%LOCALAPPDATA%\DropShelf\history.json`
- Logs: `%LOCALAPPDATA%\DropShelf\Logs` (kept for 14 days)

## System requirements

- Windows 11, 64-bit
- No separate .NET installation needed

## Building from source

Requires the .NET 10 SDK.

```powershell
dotnet build .\DropShelf.sln -c Release
dotnet run --project .\src\DropShelf\DropShelf.csproj
```

Run the tests:

```powershell
dotnet run --project .\tests\DropShelf.Tests\DropShelf.Tests.csproj
```

Build the portable version and installer (requires Inno Setup 6 or 7) into the `artifacts` folder:

```powershell
.\installer\Build-Installer.ps1
```

For debugging, set `DROPSHELF_INSTANCE` to run a separate instance, and `DROPSHELF_SETTINGS_PATH` and `DROPSHELF_LOG_DIRECTORY` to choose where settings and logs go.

## Project structure

- `src/DropShelf/AppController.cs`: app controller.
- `src/DropShelf/Services/Gestures`: shake detection and screen edge dwell.
- `src/DropShelf/Views/ProbeWindow.cs`: probe window that confirms what is being dragged.
- `src/DropShelf/Shelves/ShelfManager.cs`: creating, reusing and closing shelves.
- `src/DropShelf/Views`: shelf, settings, tutorial and other windows. The main window and shelf code is split by feature into `MainWindow.*.cs` and `ShelfWindow.*.cs`, such as flipping, the side panel, dropping in and dragging out.
- `src/DropShelf/Services`: drag in and out, quick actions, archives, history, clipboard, theme, tray, hotkeys and more.
- `src/DropShelf/Localization`: English and Chinese UI text.
- `src/DropShelf/Themes`: colors, icons and control styles.
- `src/DropShelf/Interop`: Win32, DWM and Shell interfaces.
- `tests/DropShelf.Tests`: automated tests.

## Releasing a new version

1. Update the version in `DropShelf.csproj`, `app.manifest` and both `.iss` files under `installer`, then merge into `main`.
2. On the GitHub Releases page, click "Draft a new release", create the tag `v<version>` (e.g. `v2.1.9`, matching the version) and publish.
3. GitHub Actions builds the installer and portable version and attaches them to the release a few minutes later.

## License

[MIT](LICENSE)
