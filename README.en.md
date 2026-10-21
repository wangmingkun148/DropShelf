# DropShelf for Windows

[简体中文](README.md) | English

DropShelf is a temporary shelf for files on Windows. While dragging files, give the mouse a quick shake or pause at a screen edge, and a shelf appears right where you are. Drop your files on it, switch windows or open the destination folder, then drag everything to File Explorer, a chat app or a browser in one go, with no need to arrange two windows side by side.

**Highlights**

- Appears only while you drag: shake detection runs on its own thread and never slows the mouse, and shaking without dragging anything won't open a shelf.
- Drags in and out just like File Explorer: files, folders, images, text, links, and attachments from browsers, mail and chat apps that haven't been saved to disk yet.
- Built-in quick actions: compress/extract, copy path and move to, plus custom actions to send files to a folder, compress images to a target size, or open them with an app.
- Closed by mistake? The last 10 closed shelves can be reopened.
- Lightweight and safe: runs without admin rights, never reads file contents and never goes online.
- Windows 11 style interface with light and dark themes, in English and Simplified Chinese.

**Why another one?**

There are already plenty of apps like this, and DropShelf may look like reinventing the wheel. Still, it tries hard to fix some problems found in similar apps:

- Gesture detection based on mouse hooks makes the cursor stutter whenever the app is busy. DropShelf uses Raw Input on a separate thread instead.
- Shaking the mouse without dragging anything opens a shelf. DropShelf first confirms that something is actually being dragged.
- Images dragged out of chat apps are deleted right after the drop, leaving a broken item on the shelf. DropShelf saves a copy immediately.
- After File Explorer moves a file within the same drive, the shelf still shows the item that has gone. DropShelf detects this and removes it.
- Chat apps and browsers refuse the drop. DropShelf switches to copying automatically.

Inspired by [Dropover](https://dropoverapp.com) on macOS. DropShelf is an independent project and is not affiliated with Dropover.

**Notes**

- The code of this project was written by generative AI.
- The Chinese interface text has been proofread by hand; the English text was translated by AI.
- Developed and optimized for Windows 11. Windows 10 and earlier versions have not been tested and may not work.

## Download

Get the latest version from [Releases](https://github.com/wangmingkun148/dropshelf/releases/latest):

- `DropShelf-Setup-<version>.exe`: installer (recommended). The installer itself is in Simplified Chinese.
- `DropShelf-<version>-portable.zip`: portable version. Unzip it and run `DropShelf.exe`.

## Getting started

1. Run the installer, or unzip the portable version and run `DropShelf.exe`.
2. After you close the main window, DropShelf keeps running in the notification area of the taskbar.
3. Start dragging files in File Explorer, on the desktop, in a browser or in a chat app, then quickly shake the mouse back and forth: a shelf appears at the cursor. You can also drag to the left, right or top edge of the screen and pause; the shelf slides in from that edge.
4. Drop the files on the shelf. Later, drag the thumbnail stack to take all files at once, or choose "Expand list" from the "⋯" menu at the top right to drag out single or multiple items.
5. You can also press `Ctrl+Shift+Space` (changeable in settings) at any time to open an empty shelf, then press `Ctrl+V` to paste files, images or text.

## Features

### Opening a shelf
- Shakes are detected with Raw Input on a separate thread. Windows never has to wait for DropShelf before moving the cursor, so the mouse doesn't stutter even when the app is busy.
- Once a shake is detected, an invisible probe window is placed under the cursor, and the shelf only opens after the system's drag-and-drop confirms that files, attachments, images or text are really being dragged. Shaking with the button held but nothing dragged won't open a shelf.
- Horizontal, vertical and diagonal shakes are supported, with three sensitivity levels: Responsive, Balanced and Steady.
- Pause at the left, right or top edge of the screen to open a shelf, after 0.3 s, 0.6 s or 1 s. Edges where the taskbar sits and edges shared between monitors don't trigger.
- Choose Shake, Screen edge or Both. DropShelf always responds while it's running; quit it when you don't need it.
- High polling rate mice are resampled automatically, and small sideways movements when changing direction don't interrupt detection.
- The window slides in from where the drag came from and its content unfolds from the cursor; if animations are turned off in Windows, it simply appears.

### The shelf
- When collapsed, it shows a thumbnail stack with a count badge and the total size; drag the stack to take everything. If the shelf holds both files and text, only the files are dragged out and the text and links stay.
- Expand or collapse the list from the "⋯" menu. The list shows names, sizes and Windows file types, and supports Ctrl/Shift multi-select, dragging the selection, double-click to open and a context menu.
- Dragging out uses the same data format and drag image (thumbnail stack + count) as File Explorer. Optimized same-drive moves by File Explorer are detected correctly, and moved items are removed from the shelf.
- Dragging out moves files by default (across drives too). Switch with "Drag-out action" in settings or the Move/Copy button at the top of the shelf; holding Ctrl or Shift still changes it temporarily. Apps that only accept copies (chat apps, browsers) get a copy automatically.
- While dragging in, the source app's drag image and hints such as "Add to shelf" are shown; unsupported content is reported right away. Quick actions only unfold when you reach the bottom button bar: they line up with the buttons below and grow upward from the one under the cursor, and fold away when you move back up.
- "Close empty shelves" is on by default: files you drag out leave the shelf, and an empty shelf closes by itself. With it off, copied files stay on the shelf so you can drag them somewhere else too. A pinned shelf stays open even when empty.
- Supports files, folders, attachments not yet saved to disk (browsers, mail, chat apps), web links, images and text.
- Chat app support: images and files dragged from temporary folders that get deleted right after the drop are saved immediately; PNG, JPG, GIF and DIB/DIBV5 image data are supported, as well as older QQ versions and HTML fragments that contain only an image. Temporary attachments dragged or copied to chat apps are kept for 10 more minutes so they aren't cleaned up before the other app reads them.
- Closed by mistake: shelves that still hold files when closed (including when quitting) are kept under "Recently closed shelves", up to 10, recording only paths and names. Reopen them from the Overview page in settings (opens next to the main window), the tray menu, or "Reopen last shelf" in the shelf's "⋯" menu.
- Several shelves can be open at once, and a shelf can be pinned so it stays open when emptied.
- Shortcuts: `Esc` collapses or closes, `Delete` removes the selection, `Ctrl+A` selects all, `Ctrl+C` copies as files, `Ctrl+V` pastes, `Enter` opens.
- Small, Medium and Large sizes each have their own layout; text is never scaled down.

### Quick actions
- The bottom of the shelf shows 0, 2 or 4 quick actions. Two take the same space as four, with bigger buttons and labels; with none, the whole shelf accepts drops.
- The settings page shows the current actions in the same row as on the shelf (left and right for two; left 1, left 2, right 2, right 1 for four). Drag any built-in action (compress/extract, copy path, move to, properties, show in File Explorer) or custom action from "More actions" onto a position to replace it, or drag between two positions to swap them.
- Custom actions:
  - Send to folder: move or copy to a fixed folder with one click.
  - Compress image: compress to a JPG no larger than a given size (e.g. 200 KB), lowering quality first and then the dimensions if still too large; photos are rotated upright and transparency is filled with white. The compressed image replaces the original on the shelf while the original file stays unchanged, ready to drag into a web page or chat.
  - Open with app: hand files to a chosen program; use `{files}` to place them in the arguments.
- Click a button to process the items on the shelf (only the selection when the list is expanded), or drop files directly on a button while dragging.
- ZIP files you create and folders you extract go to a new shelf next to the current one (or into the current one if it's empty), ready to drag away. Compressing, extracting and copying never overwrite existing items with the same name.
- A single ZIP or 7z file is extracted. 7z uses the tar built into Windows; older Windows versions without support show a message.

### Settings and appearance
- Windows 11 style settings window with Mica, setting cards, toggles and segmented choices. The Overview page has four cards leading to General, Gestures & shelf, Quick actions and Appearance, each showing the current settings; sub-pages have a back button at the top left.
- On Windows 11, shelves use translucent Acrylic and system rounded corners. The accent color follows Windows or can be one of eight colors, with text contrast ensured automatically.
- System, Light and Dark themes. All settings apply immediately and are saved automatically.
- English and Simplified Chinese interface: follows the Windows display language by default and can be changed under General > Language. Open windows, shelves and the tray menu update right away, no restart needed.
- Below the Overview page, recently closed shelves appear as thumbnail stacks, one row by default and expandable; click one to restore it next to the main window. Nothing is shown when there's no history. "Get started" goes away for good once closed.
- The title bar shows the running status; the version number at the bottom right opens the About window with the locations of the program, settings and logs, plus Quit and Uninstall.
- A four-step animated tutorial is shown on first run and can be viewed again from General.

### System integration
- Tray icon: left-click opens settings; right-click to create a new shelf, reopen a recently closed shelf or quit.
- Optional start with Windows, running in the background only.
- The installer can add DropShelf to the "Send to" context menu. Dropping files on the program icon or running `DropShelf.exe --shelf files...` also opens a shelf; if DropShelf is already running, the existing instance handles it.
- Starting it again brings up the existing window instead of a second instance.

## Privacy and data safety

- Regular files are recorded by path only; their contents are never read or copied. Adding, removing and clearing never change files on disk.
- Files from temporary folders (such as images dragged from chat apps), attachments not yet saved to disk and dropped images are cached in the system temp folder and cleaned up when their shelf closes; leftovers from an unexpected exit are cleaned up on the next start.
- Dragging out moves by default, with the receiving app (such as File Explorer) performing the move. "Move to" and folder custom actions move files and keep both copies when names clash. Image compression only creates new files and never changes the original.
- Recently closed shelves record only paths and names of files and folders (no text, links or temporary attachments), up to 10, and can be cleared in settings. Mouse positions are used in memory for gesture detection only and are never saved or uploaded.
- DropShelf runs with normal user rights and never needs admin rights.

## Install and uninstall

The installer installs to the current user's local programs folder (`%LOCALAPPDATA%\Programs\DropShelf`) without admin rights, and can upgrade version 1.0 in place with settings migrated automatically. Uninstall from Settings > Apps > Installed apps, the Start menu, or `卸载 DropShelf.exe` in the program folder. Uninstalling closes a running DropShelf and removes the program files, shortcuts and startup entry; your settings and logs are kept by default. The portable version has no install record: quit it and delete the folder.

## Data locations

- Settings: `%LOCALAPPDATA%\DropShelf\settings.json`
- Recently closed shelves: `%LOCALAPPDATA%\DropShelf\history.json`
- Logs: `%LOCALAPPDATA%\DropShelf\Logs` (logs older than 14 days are deleted automatically)

## System requirements

- Windows 11, 64-bit. Windows 10 and earlier versions have not been tested and may not work
- The released EXE includes its runtime; no separate .NET installation is needed

## Building from source

The project uses .NET 10 and WPF with no third-party NuGet packages.

```powershell
dotnet build .\DropShelf.sln -c Release
dotnet run --project .\src\DropShelf\DropShelf.csproj
```

Run the tests:

```powershell
dotnet run --project .\tests\DropShelf.Tests\DropShelf.Tests.csproj
```

Build the portable version and installer (requires Inno Setup 6 or 7):

```powershell
.\installer\Build-Installer.ps1
```

The output goes to `artifacts\installer` (installer) and `artifacts\win-x64` (portable).

For development, the environment variables `DROPSHELF_INSTANCE` (a separate instance that can run alongside an installed copy), `DROPSHELF_SETTINGS_PATH` and `DROPSHELF_LOG_DIRECTORY` are available.

## Project structure

- `src/DropShelf/AppController.cs`: app controller coordinating settings, gestures, shelves, tray and windows.
- `src/DropShelf/Services/Gestures`: Raw Input mouse listener, shake detection and screen edge dwell.
- `src/DropShelf/Views/ProbeWindow.cs`: probe window that confirms what is being dragged.
- `src/DropShelf/Shelves/ShelfManager.cs`: creating, reusing and closing shelves.
- `src/DropShelf/Views/ShelfWindow.xaml`: the shelf; `MainWindow.xaml`: settings; `OnboardingWindow.xaml`: tutorial.
- `src/DropShelf/Services`: drop data parsing (including chat app support), native drag-out, quick actions and image compression, shelf history, clipboard, thumbnails, theme, tray, hotkeys and more.
- `src/DropShelf/Localization`: English and Chinese UI text; XAML references it with `{l:Loc Name}` and refreshes when the language changes.
- `src/DropShelf/Themes`: colors, icons and Fluent style control templates.
- `src/DropShelf/Interop`: Win32, DWM and Shell interfaces.
- `tests/DropShelf.Tests`: automated tests.

## Releasing a new version

1. Update the version in `DropShelf.csproj`, `app.manifest` and both `.iss` files under `installer`, then merge into `main`.
2. On the GitHub Releases page, click "Draft a new release", create the tag `v<version>` (e.g. `v2.0.8`, matching the version) and publish.
3. GitHub Actions builds the installer and portable version and attaches them to the release a few minutes later. You can also push the tag directly: `git tag v2.0.8 && git push origin v2.0.8`.

## License

[MIT](LICENSE)
