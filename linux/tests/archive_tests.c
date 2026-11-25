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

/// 压缩 paths 再解压到 unpack，返回解压出来的文件夹（g_free）；哪一步失败都返回 NULL。
static char *round_trip(const char *const *paths, int count, const char *output, const char *unpack)
{
    StrList list = {0};
    for (int index = 0; index < count; index++)
    {
        wchar_t *wide = wide_from_utf8(paths[index], strlen(paths[index]));
        strlist_add(&list, wide);
        free(wide);
    }
    wchar_t *wide_output = wide_from_utf8(output, strlen(output));
    wchar_t *wide_unpack = wide_from_utf8(unpack, strlen(unpack));
    QuickActionResult packed = {0};
    archive_compress(&list, wide_output, &packed);
    char *folder = NULL;
    if (packed.succeeded && packed.outputs.count == 1)
    {
        StrList one = {0};
        strlist_add(&one, packed.outputs.items[0]);
        QuickActionResult extracted = {0};
        archive_run(&one, wide_unpack, &extracted);
        if (extracted.succeeded && extracted.outputs.count == 1)
        {
            char *narrow = utf8_from_wide(extracted.outputs.items[0], NULL);
            folder = g_strdup(narrow);
            free(narrow);
        }
        quick_action_result_free(&extracted);
        strlist_clear(&one);
    }
    quick_action_result_free(&packed);
    strlist_clear(&list);
    free(wide_output);
    free(wide_unpack);
    return folder;
}

static bool file_has(const char *folder, const char *name, const char *text)
{
    char *path = g_build_filename(folder, name, NULL);
    char *content = read_file(path);
    bool same = content && strcmp(content, text) == 0;
    free(content);
    g_free(path);
    return same;
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

    // 单个文件：压缩包按文件名去掉扩展名命名，里面的条目保留扩展名。
    // 大小写不同的两个文件（Linux 上是两个文件）都要打进去；文件夹里的空文件夹解压后还在。
    {
        char *cases = g_dir_make_tmp("dropshelf-archive-cases-XXXXXX", NULL);
        char *upper = g_build_filename(cases, "photo.JPG", NULL);
        char *lower = g_build_filename(cases, "photo.jpg", NULL);
        write_file(upper, "upper");
        write_file(lower, "lower");
        char *tree = g_build_filename(cases, "tree", NULL);
        char *empty = g_build_filename(tree, "empty", NULL);
        char *inside = g_build_filename(tree, "inside.txt", NULL);
        g_mkdir_with_parents(empty, 0755);
        write_file(inside, "in");
        char *bare = g_build_filename(cases, "bare", NULL);
        g_mkdir_with_parents(bare, 0755);

        const char *single[] = { upper };
        char *output_single = g_build_filename(cases, "out1", NULL);
        char *unpack_single = g_build_filename(cases, "unpack1", NULL);
        g_mkdir_with_parents(output_single, 0755);
        g_mkdir_with_parents(unpack_single, 0755);
        char *folder = round_trip(single, 1, output_single, unpack_single);
        check(folder && g_str_has_suffix(folder, "/photo"), "单个文件的压缩包没有按文件名命名");
        check(folder && file_has(folder, "photo.JPG", "upper"), "单个文件压缩后丢了扩展名");
        g_free(folder);

        const char *both[] = { upper, lower };
        char *output_both = g_build_filename(cases, "out2", NULL);
        char *unpack_both = g_build_filename(cases, "unpack2", NULL);
        g_mkdir_with_parents(output_both, 0755);
        g_mkdir_with_parents(unpack_both, 0755);
        folder = round_trip(both, 2, output_both, unpack_both);
        check(folder && file_has(folder, "photo.JPG", "upper"), "大小写不同的两个文件压缩后少了 photo.JPG");
        check(folder && file_has(folder, "photo (2).jpg", "lower"), "大小写不同的两个文件压缩后少了 photo.jpg");
        g_free(folder);

        const char *trees[] = { tree, bare };
        char *output_tree = g_build_filename(cases, "out3", NULL);
        char *unpack_tree = g_build_filename(cases, "unpack3", NULL);
        g_mkdir_with_parents(output_tree, 0755);
        g_mkdir_with_parents(unpack_tree, 0755);
        folder = round_trip(trees, 2, output_tree, unpack_tree);
        check(folder && file_has(folder, "tree/inside.txt", "in"), "文件夹压缩后少了文件");
        char *empty_out = folder ? g_build_filename(folder, "tree", "empty", NULL) : NULL;
        char *bare_out = folder ? g_build_filename(folder, "bare", NULL) : NULL;
        check(empty_out && g_file_test(empty_out, G_FILE_TEST_IS_DIR), "文件夹里的空文件夹压缩后丢了");
        check(bare_out && g_file_test(bare_out, G_FILE_TEST_IS_DIR), "空文件夹本身压缩后丢了");
        g_free(empty_out);
        g_free(bare_out);
        g_free(folder);

        remove_tree(cases);
        g_free(output_single);
        g_free(unpack_single);
        g_free(output_both);
        g_free(unpack_both);
        g_free(output_tree);
        g_free(unpack_tree);
        g_free(upper);
        g_free(lower);
        g_free(tree);
        g_free(empty);
        g_free(inside);
        g_free(bare);
        g_free(cases);
    }

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