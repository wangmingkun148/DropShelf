// 压缩和解压的冒烟测试：打包两个文件（其中一个在子文件夹里），再解压出来核对内容。
#include "archive.h"
#include "base.h"
#include "file_ops.h"
#include "quick_action.h"

#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int failures;

static void check(bool condition, const char *message)
{
    if (condition) return;
    fprintf(stderr, "FAIL: %s\n", message);
    failures++;
}

static void write_file(const char *path, const char *text)
{
    FILE *file = fopen(path, "wb");
    if (!file) return;
    fwrite(text, 1, strlen(text), file);
    fclose(file);
}

static char *read_file(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    char *text = calloc(1, 4096);
    size_t length = fread(text, 1, 4095, file);
    text[length] = 0;
    fclose(file);
    return text;
}

static void remove_tree(const char *path)
{
    struct stat info;
    if (lstat(path, &info) != 0) return;
    if (S_ISDIR(info.st_mode))
    {
        GDir *directory = g_dir_open(path, 0, NULL);
        if (directory)
        {
            const char *name;
            while ((name = g_dir_read_name(directory)) != NULL)
            {
                char *child = g_build_filename(path, name, NULL);
                remove_tree(child);
                g_free(child);
            }
            g_dir_close(directory);
        }
        rmdir(path);
        return;
    }
    unlink(path);
}

int main(void)
{
    char *source = g_dir_make_tmp("dropshelf-archive-src-XXXXXX", NULL);
    char *output = g_dir_make_tmp("dropshelf-archive-out-XXXXXX", NULL);
    char *unpack = g_dir_make_tmp("dropshelf-archive-unpack-XXXXXX", NULL);
    check(source && output && unpack, "无法创建临时文件夹");
    if (!source || !output || !unpack) return 1;

    char *sub = g_build_filename(source, "子文件夹", NULL);
    g_mkdir_with_parents(sub, 0755);
    char *first = g_build_filename(source, "a.txt", NULL);
    char *second = g_build_filename(sub, "b.txt", NULL);
    write_file(first, "hello");
    write_file(second, "world");

    wchar_t *wide_first = wide_from_utf8(first, strlen(first));
    wchar_t *wide_sub = wide_from_utf8(sub, strlen(sub));
    wchar_t *wide_output = wide_from_utf8(output, strlen(output));
    StrList paths = {0};
    strlist_add(&paths, wide_first);
    strlist_add(&paths, wide_sub);
    QuickActionResult result = {0};
    archive_compress(&paths, wide_output, &result);
    check(result.succeeded, "压缩失败");
    check(result.outputs.count == 1, "没有生成压缩包");
    wchar_t *archive = result.outputs.count == 1 ? wcs_dup(result.outputs.items[0]) : NULL;
    quick_action_result_free(&result);
    if (archive)
    {
        char *archive_narrow = utf8_from_wide(archive, NULL);
        check(archive_narrow && g_file_test(archive_narrow, G_FILE_TEST_EXISTS), "压缩包不在磁盘上");
        free(archive_narrow);
    }

    if (archive)
    {
        wchar_t *wide_unpack = wide_from_utf8(unpack, strlen(unpack));
        StrList one = {0};
        strlist_add(&one, archive);
        QuickActionResult extracted = {0};
        archive_run(&one, wide_unpack, &extracted);
        check(extracted.succeeded, "解压失败");
        // 解压到和压缩包同名的文件夹。
        char *archive_narrow = utf8_from_wide(archive, NULL);
        char *base = archive_narrow ? g_path_get_basename(archive_narrow) : g_strdup("archive");
        size_t base_length = strlen(base);
        if (base_length > 4 && strcmp(base + base_length - 4, ".zip") == 0) base[base_length - 4] = 0;
        char *folder = g_build_filename(unpack, base, NULL);
        g_free(base);
        free(archive_narrow);
        char *file_a = g_build_filename(folder, "a.txt", NULL);
        char *file_b = g_build_filename(folder, "子文件夹", "b.txt", NULL);
        char *text_a = read_file(file_a);
        char *text_b = read_file(file_b);
        check(text_a && strcmp(text_a, "hello") == 0, "解压出来的 a.txt 内容不对");
        check(text_b && strcmp(text_b, "world") == 0, "解压出来的 b.txt 内容不对");
        free(text_a);
        free(text_b);
        g_free(file_a);
        g_free(file_b);
        g_free(folder);
        quick_action_result_free(&extracted);
        free(wide_unpack);
        strlist_clear(&one);
    }

    // 7z 交给 bsdtar：先用它打一个 7z，再走 archive_run 解开。没装 bsdtar（libarchive-tools）时这里会失败。
    char *seven = g_build_filename(output, "七.7z", NULL);
    char *pack[] = { "bsdtar", "--format", "7zip", "-cf", seven, "-C", source, "a.txt", "子文件夹", NULL };
    char **environment = g_environ_setenv(g_get_environ(), "LC_ALL", "C.UTF-8", TRUE);
    int status = -1;
    bool packed = g_spawn_sync(NULL, pack, environment, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, &status, NULL) && status == 0;
    g_strfreev(environment);
    check(packed, "bsdtar 打不出 7z（装了 bsdtar 吗？）");
    if (packed)
    {
        wchar_t *wide_seven = wide_from_utf8(seven, strlen(seven));
        wchar_t *wide_unpack = wide_from_utf8(unpack, strlen(unpack));
        StrList one = {0};
        strlist_add(&one, wide_seven);
        QuickActionResult extracted = {0};
        archive_run(&one, wide_unpack, &extracted);
        check(extracted.succeeded, "解压 7z 失败");
        char *file_b = g_build_filename(unpack, "七", "子文件夹", "b.txt", NULL);
        char *text_b = read_file(file_b);
        check(text_b && strcmp(text_b, "world") == 0, "7z 解压出来的 b.txt 内容不对");
        free(text_b);
        g_free(file_b);
        quick_action_result_free(&extracted);
        strlist_clear(&one);
        free(wide_seven);
        free(wide_unpack);
    }
    g_free(seven);

    free(archive);
    strlist_clear(&paths);
    free(wide_first);
    free(wide_sub);
    free(wide_output);
    remove_tree(unpack);
    remove_tree(output);
    remove_tree(source);
    g_free(first);
    g_free(second);
    g_free(sub);
    g_free(source);
    g_free(output);
    g_free(unpack);

    if (failures == 0) printf("archive tests passed\n");
    return failures == 0 ? 0 : 1;
}