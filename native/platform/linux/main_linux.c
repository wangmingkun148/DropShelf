// Linux 入口：GtkApplication + GApplication 单实例。进程和命令行只放在这里，置物架由 shelves.c 管理。
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "app.h"
#include "base.h"
#include "foreground.h"
#include "foreground_linux.h"
#include "hyprland_linux.h"
#include "lang.h"
#include "log.h"
#include "settings.h"
#include "shelf_history.h"
#include "shelf_window.h"
#include "task.h"
#include "theme.h"
#include "ui.h"

#include <glib-unix.h>
#include <gtk/gtk.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define APPLICATION_ID "io.github.wangmingkun148.DropShelf"

static GtkApplication *application;
static bool command_line_seen;      // 第一次是启动（开机自启、点图标），之后再启动就是用户又点了一次

// ───────────── 命令行 ─────────────

/// 调试用：打印共享代码看到的全局光标位置和工作区，不开窗口。
static void print_cursor(GApplicationCommandLine *command_line)
{
    POINT cursor;
    if (!GetCursorPos(&cursor))
    {
        g_application_command_line_print(command_line, "拿不到光标位置\n");
        return;
    }
    g_application_command_line_print(command_line, "%d,%d\n", (int)cursor.x, (int)cursor.y);
}

static void print_monitors(GApplicationCommandLine *command_line)
{
    GdkDisplay *display = gdk_display_get_default();
    if (!display) return;
    GListModel *monitors = gdk_display_get_monitors(display);
    guint count = g_list_model_get_n_items(monitors);
    for (guint index = 0; index < count; index++)
    {
        MONITORINFO info;
        memset(&info, 0, sizeof info);
        info.cbSize = sizeof info;
        if (!GetMonitorInfoW((HMONITOR)(intptr_t)(index + 1), &info)) continue;
        g_application_command_line_print(command_line, "%u: 屏幕 %ld,%ld %ldx%ld 工作区 %ld,%ld %ldx%ld\n", index,
            (long)info.rcMonitor.left, (long)info.rcMonitor.top, (long)(info.rcMonitor.right - info.rcMonitor.left),
            (long)(info.rcMonitor.bottom - info.rcMonitor.top), (long)info.rcWork.left, (long)info.rcWork.top,
            (long)(info.rcWork.right - info.rcWork.left), (long)(info.rcWork.bottom - info.rcWork.top));
    }
}

/// 调试用：打印前台窗口的应用标识和有窗口的程序，不开窗口。
static void print_foreground(GApplicationCommandLine *command_line)
{
    char *reply = hyprland_request("j/activewindow");
    wchar_t *text = reply ? wide_from_utf8(reply, strlen(reply)) : NULL;
    wchar_t *active = text ? foreground_active_class(text) : NULL;
    char *name = active ? utf8_from_wide(active, NULL) : NULL;
    g_application_command_line_print(command_line, "前台：%s%s\n", name ? name : "（没有）",
        text && foreground_active_fullscreen(text) ? "（全屏）" : "");
    free(name);
    free(active);
    free(text);
    free(reply);
    StrList apps = {0};
    foreground_windowed_apps(&apps);
    for (int index = 0; index < apps.count; index++)
    {
        char *each = utf8_from_wide(apps.items[index], NULL);
        g_application_command_line_print(command_line, "  %s\n", each ? each : "");
        free(each);
    }
    strlist_clear(&apps);
}

/// 一次性的命令（--version、--cursor…）：第一次启动时打印完就退出；已经有实例在跑时只打印，不打扰它。
static void finish_one_shot(void)
{
    if (!command_line_seen) g_application_quit(G_APPLICATION(application));
}

static int on_command_line(GApplication *app, GApplicationCommandLine *command_line, gpointer data)
{
    (void)app;
    (void)data;
    int argc = 0;
    char **argv = g_application_command_line_get_arguments(command_line, &argc);
    bool show_shelf = false;
    bool print_version = false;
    bool show_cursor = false, show_monitors = false, show_foreground = false;
    StrList paths = {0};
    for (int index = 1; index < argc; index++)
    {
        if (strcmp(argv[index], "--shelf") == 0) show_shelf = true;
        else if (strcmp(argv[index], "--version") == 0 || strcmp(argv[index], "-v") == 0) print_version = true;
        else if (strcmp(argv[index], "--cursor") == 0) show_cursor = true;
        else if (strcmp(argv[index], "--monitors") == 0) show_monitors = true;
        else if (strcmp(argv[index], "--foreground") == 0) show_foreground = true;
        else if (strcmp(argv[index], "--help") == 0 || strcmp(argv[index], "-h") == 0)
        {
            g_application_command_line_print(command_line,
                "用法：dropshelf [--shelf] [文件…]\n"
                "  --shelf     打开置物架\n"
                "  --version   显示版本号\n"
                "  --cursor    打印全局光标位置（调试）\n"
                "  --monitors  打印各显示器的工作区（调试）\n"
                "  --foreground 打印前台程序（调试）\n");
            g_strfreev(argv);
            strlist_clear(&paths);
            finish_one_shot();
            return 0;
        }
        else if (argv[index][0] != '-')
        {
            wchar_t *wide = wide_from_utf8(argv[index], strlen(argv[index]));
            if (wide)
            {
                strlist_add(&paths, wide);
                free(wide);
            }
        }
    }
    if (show_cursor)
    {
        print_cursor(command_line);
        g_strfreev(argv);
        strlist_clear(&paths);
        finish_one_shot();
        return 0;
    }
    if (show_monitors)
    {
        print_monitors(command_line);
        g_strfreev(argv);
        strlist_clear(&paths);
        finish_one_shot();
        return 0;
    }
    if (show_foreground)
    {
        print_foreground(command_line);
        g_strfreev(argv);
        strlist_clear(&paths);
        finish_one_shot();
        return 0;
    }
    if (print_version)
    {
        g_application_command_line_print(command_line, "DropShelf %s\n", DROPSHELF_VERSION_UTF8);
        g_strfreev(argv);
        strlist_clear(&paths);
        finish_one_shot();
        return 0;
    }
    // 普通启动（开机自启、点图标）什么都不弹；--shelf 或带了文件才打开置物架。
    if (show_shelf || paths.count > 0)
    {
        command_line_seen = true;
        app_handle_arguments(&paths);
    }
    else if (command_line_seen)
    {
        // 程序已经在后台运行，用户又点了一次图标：打开主窗口。
        app_show_main_window();
    }
    else command_line_seen = true;
    g_strfreev(argv);
    strlist_clear(&paths);
    return 0;
}

static void on_activate(GApplication *app, gpointer data)
{
    (void)app;
    (void)data;
    if (shelves_open_count() == 0) app_handle_arguments(NULL);
}

static void on_startup(GApplication *app, gpointer data)
{
    (void)app;
    (void)data;
    g_set_prgname("dropshelf");
    task_initialize();
    theme_initialize(THEME_SYSTEM, NULL);
    language_set_mode(LANGUAGE_SYSTEM);
    shelf_history_load(NULL);
    shelves_prewarm();
    app_start(NULL, true);
}

static void on_shutdown(GApplication *app, gpointer data)
{
    (void)app;
    (void)data;
    shelves_close_all();
    shelf_history_flush();
    app_prepare_for_shutdown();
    task_shutdown();
    ui_close_all_windows();
}

/// 收到 SIGTERM / SIGINT（例如注销、systemctl stop）时正常退出，收掉 Hyprland 的绑定和托盘图标。
static gboolean on_terminate(gpointer data)
{
    (void)data;
    g_application_quit(G_APPLICATION(application));
    return G_SOURCE_CONTINUE;
}

int main(int argc, char **argv)
{
    application = gtk_application_new(APPLICATION_ID, G_APPLICATION_HANDLES_COMMAND_LINE);
    g_signal_connect(application, "startup", G_CALLBACK(on_startup), NULL);
    g_signal_connect(application, "activate", G_CALLBACK(on_activate), NULL);
    g_signal_connect(application, "command-line", G_CALLBACK(on_command_line), NULL);
    g_signal_connect(application, "shutdown", G_CALLBACK(on_shutdown), NULL);
    g_unix_signal_add(SIGTERM, on_terminate, NULL);
    g_unix_signal_add(SIGINT, on_terminate, NULL);
    int status = g_application_run(G_APPLICATION(application), argc, argv);
    g_object_unref(application);
    application = NULL;
    return status;
}