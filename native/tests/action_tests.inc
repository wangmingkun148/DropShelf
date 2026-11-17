// 第 5 步（快速操作）的测试，对应 C# 版 FileActionTests 里的各项（剪贴板的两项不测，免得改掉正在用的剪贴板）。由 tests.c 引入。

static char *read_all(const wchar_t *path)
{
    wchar_t *text = file_read_text(path, 1 << 20);
    if (!text) return NULL;
    char *bytes = utf8_from_wide(text, NULL);
    free(text);
    return bytes;
}

static bool file_text_is(const wchar_t *path, const char *expected)
{
    char *bytes = read_all(path);
    bool same = bytes && strcmp(bytes, expected) == 0;
    free(bytes);
    return same;
}

static void make_junction(const wchar_t *link, const wchar_t *target)
{
    wchar_t command[MAX_PATH * 3];
    swprintf(command, MAX_PATH * 3, L"cmd /c mklink /J \"%ls\" \"%ls\" >nul", link, target);
    _wsystem(command);
}

/// 一张满是杂色的 PNG（压不小），用 WIC 写出来。
static bool write_noisy_png(const wchar_t *path, UINT width, UINT height)
{
    IWICImagingFactory *factory = NULL;
    if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&factory))) return false;
    BYTE *pixels = xalloc((size_t)width * height * 3);
    unsigned seed = 12345;
    for (size_t index = 0; index < (size_t)width * height * 3; index++)
    {
        seed = seed * 1103515245u + 12345u;
        pixels[index] = (BYTE)(seed >> 16);
    }
    IWICBitmap *bitmap = NULL;
    IWICStream *stream = NULL;
    IWICBitmapEncoder *encoder = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    HRESULT result = IWICImagingFactory_CreateBitmapFromMemory(factory, width, height, &GUID_WICPixelFormat24bppBGR, width * 3, width * height * 3, pixels, &bitmap);
    if (SUCCEEDED(result)) result = IWICImagingFactory_CreateStream(factory, &stream);
    if (SUCCEEDED(result)) result = IWICStream_InitializeFromFilename(stream, path, GENERIC_WRITE);
    if (SUCCEEDED(result)) result = IWICImagingFactory_CreateEncoder(factory, &GUID_ContainerFormatPng, NULL, &encoder);
    if (SUCCEEDED(result)) result = IWICBitmapEncoder_Initialize(encoder, (IStream *)stream, WICBitmapEncoderNoCache);
    if (SUCCEEDED(result)) result = IWICBitmapEncoder_CreateNewFrame(encoder, &frame, NULL);
    if (SUCCEEDED(result)) result = IWICBitmapFrameEncode_Initialize(frame, NULL);
    if (SUCCEEDED(result)) result = IWICBitmapFrameEncode_WriteSource(frame, (IWICBitmapSource *)bitmap, NULL);
    if (SUCCEEDED(result)) result = IWICBitmapFrameEncode_Commit(frame);
    if (SUCCEEDED(result)) result = IWICBitmapEncoder_Commit(encoder);
    if (frame) IWICBitmapFrameEncode_Release(frame);
    if (encoder) IWICBitmapEncoder_Release(encoder);
    if (stream) IWICStream_Release(stream);
    if (bitmap) IWICBitmap_Release(bitmap);
    IWICImagingFactory_Release(factory);
    free(pixels);
    return SUCCEEDED(result);
}

/// 图片的容器格式和尺寸。
static bool image_info(const wchar_t *path, GUID *container, UINT *width, UINT *height)
{
    IWICImagingFactory *factory = NULL;
    if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&factory))) return false;
    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    HRESULT result = IWICImagingFactory_CreateDecoderFromFilename(factory, path, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);
    if (SUCCEEDED(result)) result = IWICBitmapDecoder_GetContainerFormat(decoder, container);
    if (SUCCEEDED(result)) result = IWICBitmapDecoder_GetFrame(decoder, 0, &frame);
    if (SUCCEEDED(result)) result = IWICBitmapFrameDecode_GetSize(frame, width, height);
    if (frame) IWICBitmapFrameDecode_Release(frame);
    if (decoder) IWICBitmapDecoder_Release(decoder);
    IWICImagingFactory_Release(factory);
    return SUCCEEDED(result);
}

static long long size_of(const wchar_t *path)
{
    long long size = -1;
    file_size_of(path, &size);
    return size;
}

// ───────────── 复制和移动 ─────────────

static void test_file_operations(void)
{
    language_set_mode(LANGUAGE_CHINESE);
    // 复制时保留下载来源标记和修改时间。
    wchar_t *source_folder = test_folder(L"ops-source");
    wchar_t *source = test_file(source_folder, L"download.txt", "downloaded");
    const char *zone = "[ZoneTransfer]\r\nZoneId=3\r\n";
    zone_identifier_write(source, zone, strlen(zone));
    HANDLE handle = CreateFileW(source, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    FILETIME modified = { 0x12345678, 0x01D5C1A0 };
    SetFileTime(handle, NULL, NULL, &modified);
    CloseHandle(handle);
    wchar_t *destination = test_folder(L"ops-destination");
    StrList sources = {0};
    strlist_add(&sources, source);
    FileOpResults results = file_ops_copy(&sources, destination);
    CHECK(results.count == 1 && results.items[0].succeeded, "复制没有成功");
    if (results.count == 1 && results.items[0].destination)
    {
        size_t length = 0;
        char *copied_zone = zone_identifier_read(results.items[0].destination, &length);
        CHECK(copied_zone && strstr(copied_zone, "ZoneId=3"), "复制后丢失了下载来源标记");
        free(copied_zone);
        WIN32_FILE_ATTRIBUTE_DATA data;
        GetFileAttributesExW(results.items[0].destination, GetFileExInfoStandard, &data);
        CHECK(data.ftLastWriteTime.dwLowDateTime == modified.dwLowDateTime && data.ftLastWriteTime.dwHighDateTime == modified.dwHighDateTime,
            "复制后修改时间被改成了当前时间");
    }
    file_op_results_free(&results);

    // 不覆盖同名文件，另起名字“(2)”。
    results = file_ops_copy(&sources, destination);
    wchar_t *second = path_join(destination, L"download (2).txt");
    CHECK(results.count == 1 && results.items[0].succeeded && wcs_equal(results.items[0].destination, second) && file_text_is(second, "downloaded"),
        "未生成正确的重名副本");
    file_op_results_free(&results);
    free(second);

    // 不存在的源文件、被占用的文件报告失败。
    StrList missing = {0};
    strlist_add_owned(&missing, path_join(source_folder, L"not-created.txt"));
    results = file_ops_copy(&missing, destination);
    CHECK(results.count == 1 && !results.items[0].succeeded && results.items[0].error == STR_ItemNotFound, "不存在的源文件没有被报告为失败");
    file_op_results_free(&results);
    strlist_clear(&missing);
    wchar_t *locked = test_file(source_folder, L"locked.txt", "locked");
    HANDLE lock = CreateFileW(locked, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    StrList locked_list = {0};
    strlist_add(&locked_list, locked);
    results = file_ops_copy(&locked_list, destination);
    CHECK(results.count == 1 && !results.items[0].succeeded && path_exists(locked), "被占用文件没有被报告为失败");
    file_op_results_free(&results);
    results = file_ops_move(&locked_list, destination);
    CHECK(results.count == 1 && !results.items[0].succeeded && path_exists(locked), "被占用文件移动时没有报告失败");
    file_op_results_free(&results);
    CloseHandle(lock);
    strlist_clear(&locked_list);

    // 别的程序只读着、不让删：复制得过去，原来的删不掉。新位置是完整的一份，按移动成功算，标出原位置有残留。
    wchar_t *reading = test_file(source_folder, L"reading.txt", "reading");
    HANDLE reader = CreateFileW(reading, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    StrList reading_list = {0};
    strlist_add(&reading_list, reading);
    results = file_ops_move(&reading_list, destination);
    CHECK(results.count == 1 && results.items[0].succeeded && results.items[0].source_left && path_exists(reading) &&
          results.items[0].destination && file_text_is(results.items[0].destination, "reading"), "原来的删不掉时没有按移动成功并标出残留");
    file_op_results_free(&results);
    if (reader != INVALID_HANDLE_VALUE) CloseHandle(reader);
    results = file_ops_move(&reading_list, destination);
    CHECK(results.count == 1 && results.items[0].succeeded && !results.items[0].source_left && !path_exists(reading), "能删掉时不应标出残留");
    file_op_results_free(&results);
    strlist_clear(&reading_list);
    free(reading);

    // 含联接点的文件夹在复制之前报错，什么都不留下。
    wchar_t *link_source = test_folder(L"ops-link-source");
    wchar_t *linked = test_folder(L"ops-link-target");
    free(test_file(linked, L"inside.txt", "linked"));
    wchar_t *junction = path_join(link_source, L"junction");
    make_junction(junction, linked);
    CHECK(path_is_directory(junction), "无法创建测试用的联接点");

    // 联接点、符号链接和认不得的重解析点算链接；OneDrive 等按需下载的文件和文件夹（云文件、重复数据删除等）带着同样的属性，
    // 却是真正的内容，不能当链接跳过（否则压缩时漏掉里面的文件、复制时报“含有链接”）。
    WIN32_FIND_DATAW find_data = {0};
    CHECK(!find_data_is_link(&find_data), "没有重解析点属性的项目被当成了链接");
    find_data.dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT;
    find_data.dwReserved0 = 0xA0000003u;    // 联接点
    CHECK(find_data_is_link(&find_data), "联接点没有被当成链接");
    find_data.dwReserved0 = 0xA000000Cu;    // 符号链接
    CHECK(find_data_is_link(&find_data), "符号链接没有被当成链接");
    find_data.dwReserved0 = 0x8000001Bu;    // 应用执行别名：读不了内容
    CHECK(find_data_is_link(&find_data), "认不得的重解析点没有按链接处理");
    find_data.dwReserved0 = 0;
    CHECK(find_data_is_link(&find_data), "读不出标记的重解析点没有按链接处理");
    find_data.dwReserved0 = 0x9000701Au;    // OneDrive 的云文件（CLOUD_7）
    CHECK(!find_data_is_link(&find_data), "OneDrive 的占位文件夹被当成了链接");
    find_data.dwReserved0 = 0x9000001Au;    // 云文件
    CHECK(!find_data_is_link(&find_data), "云文件被当成了链接");
    find_data.dwReserved0 = 0x80000013u;    // 重复数据删除
    CHECK(!find_data_is_link(&find_data), "重复数据删除的文件被当成了链接");
    wchar_t *junction_with_separator = path_join(junction, L"");
    CHECK(path_is_link(junction) && path_is_link(junction_with_separator) && !path_is_link(link_source) && path_is_link(L"C:\\no\\such\\folder"),
        "按路径判断是不是链接不对");
    free(junction_with_separator);

    // 统计文件夹：联接点算一项，但不进去也不计大小；旁边的文件和子文件夹照常计入。
    wchar_t *counted_file = test_file(link_source, L"beside.bin", "12345");
    wchar_t *counted_folder = path_join(link_source, L"sub");
    directory_create(counted_folder);
    free(test_file(counted_folder, L"deep.bin", "123"));
    int counted_children = 0;
    bool counted_has_size = false;
    long long counted_size = 0;
    CHECK(folder_measure(link_source, &counted_children, &counted_has_size, &counted_size, NULL) && counted_children == 3 && counted_has_size &&
          counted_size == 8, "统计带联接点的文件夹时项目数或大小不对");
    volatile LONG stop = 1;
    CHECK(!folder_measure(link_source, &counted_children, &counted_has_size, &counted_size, &stop), "取消后统计没有停下");
    DeleteFileW(counted_file);
    directory_delete_tree(counted_folder);
    free(counted_file);
    free(counted_folder);

    // 删除带联接点的文件夹时只删联接点本身，不进去删它指向的内容。
    wchar_t *doomed = test_folder(L"ops-link-doomed");
    wchar_t *doomed_junction = path_join(doomed, L"junction");
    make_junction(doomed_junction, linked);
    wchar_t *linked_file = path_join(linked, L"inside.txt");
    CHECK(directory_delete_tree(doomed) && !path_exists(doomed) && file_text_is(linked_file, "linked"), "删除带联接点的文件夹时误删了联接点指向的内容");
    free(linked_file);
    free(doomed_junction);
    free(doomed);

    wchar_t *link_destination = test_folder(L"ops-link-destination");
    StrList link_list = {0};
    strlist_add(&link_list, link_source);
    results = file_ops_copy(&link_list, link_destination);
    wchar_t *partial = path_join(link_destination, L"ops-link-source");
    CHECK(results.count == 1 && !results.items[0].succeeded && results.items[0].error == STR_FolderContainsLinks && !path_exists(partial),
        "含联接点的文件夹没有明确报告失败");
    file_op_results_free(&results);
    free(partial);
    strlist_clear(&link_list);

    // 复制到文件夹自身或它的里面（包括经过联接点）不行。
    wchar_t *nested = test_folder(L"ops-nested");
    free(test_file(nested, L"file.txt", "content"));
    StrList nested_list = {0};
    strlist_add(&nested_list, nested);
    wchar_t *child = path_join(nested, L"child");
    results = file_ops_copy(&nested_list, child);
    CHECK(results.count == 1 && !results.items[0].succeeded && results.items[0].error == STR_FolderIntoItself, "复制到文件夹自身内部未被阻止");
    file_op_results_free(&results);
    results = file_ops_copy(&nested_list, nested);
    CHECK(results.count == 1 && !results.items[0].succeeded, "复制到文件夹自身未被阻止");
    file_op_results_free(&results);
    wchar_t *link_parent = test_folder(L"ops-nested-links");
    wchar_t *to_source = path_join(link_parent, L"to-source");
    make_junction(to_source, nested);
    wchar_t *inner = path_join(to_source, L"inner");
    results = file_ops_copy(&nested_list, inner);
    CHECK(results.count == 1 && !results.items[0].succeeded, "通过联接点复制到文件夹自身内部未被阻止");
    file_op_results_free(&results);
    strlist_clear(&nested_list);

    // 移动文件夹；已在目标里的不用动。
    wchar_t *mover = test_folder(L"ops-move-me");
    free(test_file(mover, L"a.txt", "a"));
    StrList move_list = {0};
    strlist_add(&move_list, mover);
    results = file_ops_move(&move_list, destination);
    wchar_t *moved = path_join(destination, L"ops-move-me");
    wchar_t *moved_file = path_join(moved, L"a.txt");
    CHECK(results.count == 1 && results.items[0].succeeded && !path_exists(mover) && file_text_is(moved_file, "a"), "文件夹没有移动过去");
    CHECK(path_is_direct_child(moved, destination) && !path_is_direct_child(moved_file, destination), "判断是否直接在文件夹里不对");
    file_op_results_free(&results);
    strlist_clear(&move_list);

    // 路径的小工具。
    wchar_t *stem, *extension;
    path_split_name(L"archive.tar.gz", &stem, &extension);
    CHECK(wcscmp(stem, L"archive.tar") == 0 && wcscmp(extension, L".gz") == 0, "文件名拆分不对");
    free(stem);
    free(extension);
    path_split_name(L".gitignore", &stem, &extension);
    CHECK(wcscmp(stem, L"") == 0 && wcscmp(extension, L".gitignore") == 0, "以点开头的文件名拆分不对");
    free(stem);
    free(extension);
    wchar_t *name = path_folder_name(L"C:\\Users\\me\\Desktop\\");
    CHECK(wcscmp(name, L"Desktop") == 0, "文件夹名称不对");
    free(name);
    name = path_folder_name(L"D:\\");
    CHECK(wcscmp(name, L"D:\\") == 0, "磁盘根目录的名称应是路径本身");
    free(name);

    strlist_clear(&sources);
    free(source_folder);
    free(source);
    free(destination);
    free(locked);
    free(link_source);
    free(linked);
    free(junction);
    free(link_destination);
    free(nested);
    free(child);
    free(link_parent);
    free(to_source);
    free(inner);
    free(mover);
    free(moved);
    free(moved_file);
}

// ───────────── 压缩和解压 ─────────────

static void write_zip(const wchar_t *path, const char *const *names, const char *const *contents, int count)
{
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    // 测试里没有 stdio 版本的 miniz：写到内存里再存成文件。
    mz_zip_writer_init_heap(&zip, 0, 0);
    for (int index = 0; index < count; index++)
        mz_zip_writer_add_mem(&zip, names[index], contents[index], contents[index] ? strlen(contents[index]) : 0, MZ_DEFAULT_LEVEL);
    void *buffer = NULL;
    size_t size = 0;
    mz_zip_writer_finalize_heap_archive(&zip, &buffer, &size);
    file_write_atomic(path, buffer, size);
    mz_zip_writer_end(&zip);
}

static void test_archive(void)
{
    language_set_mode(LANGUAGE_CHINESE);
    wchar_t *folder = test_folder(L"archive");
    wchar_t *source = test_file(folder, L"quick-action.txt", "quick action content");
    StrList list = {0};
    strlist_add(&list, source);
    QuickActionResult compressed = {0};
    archive_run(&list, NULL, &compressed);
    wchar_t *zip = path_join(folder, L"quick-action.zip");
    CHECK(compressed.succeeded && compressed.outputs.count == 1 && wcs_equal(compressed.outputs.items[0], zip) && path_exists(zip), "压缩没有生成 ZIP");
    CHECK(owned_text_is(wcs_dup(compressed.message), L"已创建 quick-action.zip"), "压缩完成的提示不对");
    quick_action_result_free(&compressed);
    strlist_clear(&list);

    // 解压回来；来自网络的压缩包解出的文件带上同样的来源。
    const char *zone = "[ZoneTransfer]\r\nZoneId=3\r\n";
    zone_identifier_write(zip, zone, strlen(zone));
    strlist_add(&list, zip);
    QuickActionResult extracted = {0};
    archive_run(&list, NULL, &extracted);
    wchar_t *expected_folder = path_join(folder, L"quick-action");
    wchar_t *inside = path_join(expected_folder, L"quick-action.txt");
    CHECK(extracted.succeeded && extracted.outputs.count == 1 && wcs_equal(extracted.outputs.items[0], expected_folder) &&
          file_text_is(inside, "quick action content"), "ZIP 解压后的文件内容不正确");
    size_t length = 0;
    char *propagated = zone_identifier_read(inside, &length);
    CHECK(propagated && strstr(propagated, "ZoneId=3"), "解压来自网络的 ZIP 后没有传递下载来源标记");
    free(propagated);
    quick_action_result_free(&extracted);
    strlist_clear(&list);

    // 一个文件夹：里面的空文件夹、中文名都在，链接不跟进。
    wchar_t *tree = test_folder(L"资料");
    wchar_t *sub = path_join(tree, L"子目录");
    directory_create(sub);
    free(test_file(sub, L"笔记.txt", "notes"));
    wchar_t *empty = path_join(tree, L"空的");
    directory_create(empty);
    strlist_add(&list, tree);
    QuickActionResult tree_zip = {0};
    wchar_t *output_folder = test_folder(L"archive-output");
    archive_run(&list, output_folder, &tree_zip);
    CHECK(tree_zip.succeeded, "压缩文件夹失败");
    strlist_clear(&list);
    if (tree_zip.succeeded)
    {
        strlist_add(&list, tree_zip.outputs.items[0]);
        QuickActionResult back = {0};
        archive_run(&list, output_folder, &back);
        wchar_t *notes = back.outputs.count ? path_join(back.outputs.items[0], L"资料\\子目录\\笔记.txt") : NULL;
        wchar_t *kept_empty = back.outputs.count ? path_join(back.outputs.items[0], L"资料\\空的") : NULL;
        CHECK(back.succeeded && notes && file_text_is(notes, "notes") && path_is_directory(kept_empty), "文件夹压缩再解压后内容不对");
        free(notes);
        free(kept_empty);
        quick_action_result_free(&back);
        strlist_clear(&list);
    }
    quick_action_result_free(&tree_zip);

    // 两个同名文件放进同一个压缩包：第二个改名“(2)”。
    wchar_t *other = test_folder(L"archive-other");
    wchar_t *same_name = test_file(other, L"quick-action.txt", "other");
    strlist_add(&list, source);
    strlist_add(&list, same_name);
    QuickActionResult pair = {0};
    archive_run(&list, output_folder, &pair);
    CHECK(pair.succeeded && pair.outputs.count == 1 && wcsstr(pair.outputs.items[0], L"DropShelf 压缩文件"), "多个文件压缩时名称不对");
    if (pair.succeeded)
    {
        mz_zip_archive reader;
        memset(&reader, 0, sizeof(reader));
        size_t zip_size = 0;
        char *bytes = NULL;
        HANDLE file = CreateFileW(pair.outputs.items[0], GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        LARGE_INTEGER size;
        if (file != INVALID_HANDLE_VALUE && GetFileSizeEx(file, &size))
        {
            zip_size = (size_t)size.QuadPart;
            bytes = xalloc(zip_size);
            DWORD read = 0;
            ReadFile(file, bytes, (DWORD)zip_size, &read, NULL);
        }
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        bool names_ok = bytes && mz_zip_reader_init_mem(&reader, bytes, zip_size, 0) && mz_zip_reader_get_num_files(&reader) == 2 &&
                        mz_zip_reader_locate_file(&reader, "quick-action (2).txt", NULL, 0) >= 0;
        CHECK(names_ok, "压缩包里的重名文件没有改名");
        mz_zip_reader_end(&reader);
        free(bytes);
    }
    quick_action_result_free(&pair);
    strlist_clear(&list);

    // 不安全的路径不解压，外面什么也不写。
    wchar_t *unsafe = path_join(folder, L"unsafe.zip");
    const char *unsafe_names[] = { "../escaped.txt" };
    const char *unsafe_contents[] = { "escaped" };
    write_zip(unsafe, unsafe_names, unsafe_contents, 1);
    strlist_add(&list, unsafe);
    QuickActionResult unsafe_result = {0};
    archive_run(&list, NULL, &unsafe_result);
    wchar_t *escaped = path_join(folder, L"escaped.txt");
    wchar_t *unsafe_folder = path_join(folder, L"unsafe");
    CHECK(!unsafe_result.succeeded && !path_exists(escaped) && !path_exists(unsafe_folder), "含有不安全路径的 ZIP 被错误解压");
    CHECK(owned_text_is(wcs_dup(unsafe_result.message), L"解压失败，压缩包没有改变"), "解压失败的提示不对");
    quick_action_result_free(&unsafe_result);
    strlist_clear(&list);

    // 带根目录项“./”的压缩包照常解压。
    wchar_t *root_entry = path_join(folder, L"root-entry.zip");
    const char *root_names[] = { "./", "inside.txt" };
    const char *root_contents[] = { NULL, "inside root entry archive" };
    write_zip(root_entry, root_names, root_contents, 2);
    strlist_add(&list, root_entry);
    QuickActionResult root_result = {0};
    archive_run(&list, NULL, &root_result);
    wchar_t *root_inside = root_result.outputs.count ? path_join(root_result.outputs.items[0], L"inside.txt") : NULL;
    CHECK(root_result.succeeded && root_inside && file_text_is(root_inside, "inside root entry archive"), "包含根目录项的 ZIP 解压不正确");
    free(root_inside);
    quick_action_result_free(&root_result);
    strlist_clear(&list);

    // 7z 交给 Windows 自带的 tar（有的话）。
    wchar_t system_directory[MAX_PATH];
    GetSystemDirectoryW(system_directory, MAX_PATH);
    wchar_t *tar = path_join(system_directory, L"tar.exe");
    if (path_exists(tar))
    {
        free(test_file(folder, L"native-seven.txt", "native 7z content"));
        wchar_t *seven = path_join(folder, L"native-seven.7z");
        wchar_t command[MAX_PATH * 4];
        swprintf(command, MAX_PATH * 4, L"\"\"%ls\" -caf \"%ls\" -C \"%ls\" native-seven.txt\"", tar, seven, folder);
        _wsystem(command);
        if (path_exists(seven))
        {
            strlist_add(&list, seven);
            QuickActionResult seven_result = {0};
            archive_run(&list, NULL, &seven_result);
            wchar_t *seven_inside = seven_result.outputs.count ? path_join(seven_result.outputs.items[0], L"native-seven.txt") : NULL;
            CHECK(seven_result.succeeded && seven_inside && file_text_is(seven_inside, "native 7z content"), "Windows 内置 7z 解压结果不正确");
            free(seven_inside);
            quick_action_result_free(&seven_result);
            strlist_clear(&list);
        }
        free(seven);
    }
    free(tar);

    // 没有留下临时文件。
    wchar_t *pattern = path_join(folder, L".dropshelf-*");
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW(pattern, &data);
    CHECK(find == INVALID_HANDLE_VALUE, "压缩或解压后留下了临时文件");
    if (find != INVALID_HANDLE_VALUE) FindClose(find);
    free(pattern);

    free(folder);
    free(source);
    free(zip);
    free(expected_folder);
    free(inside);
    free(tree);
    free(sub);
    free(empty);
    free(output_folder);
    free(other);
    free(same_name);
    free(unsafe);
    free(escaped);
    free(unsafe_folder);
    free(root_entry);
}

/// 已经压缩过的内容（视频、照片、压缩包）原样存入，能压小的照常压缩；解压出来都和原来一样。
static void test_archive_already_compressed(void)
{
    wchar_t *folder = test_folder(L"archive-stored");
    const size_t size = 1536 * 1024;
    char *noise = xalloc(size), *plain = xalloc(size);
    unsigned state = 20240229;
    for (size_t index = 0; index < size; index++)
    {
        state = state * 1664525u + 1013904223u;
        noise[index] = (char)(state >> 24);
        plain[index] = (char)('a' + index % 7);
    }
    wchar_t *noise_path = path_join(folder, L"video.bin"), *plain_path = path_join(folder, L"report.txt");
    // 小文件不取样，杂乱的内容也照常压缩。
    wchar_t *small_path = path_join(folder, L"small.bin");
    CHECK(file_write_atomic(noise_path, noise, size) && file_write_atomic(plain_path, plain, size) && file_write_atomic(small_path, noise, 4096),
        "无法写出测试文件");

    StrList list = {0};
    strlist_add(&list, noise_path);
    strlist_add(&list, plain_path);
    strlist_add(&list, small_path);
    QuickActionResult result = {0};
    archive_compress(&list, NULL, &result);
    CHECK(result.succeeded && result.outputs.count == 1, "压缩失败");
    if (result.succeeded && result.outputs.count == 1)
    {
        const wchar_t *zip_path = result.outputs.items[0];
        long long zip_size = size_of(zip_path);
        // 杂乱的那份几乎原样大小，重复的那份压得很小。
        CHECK(zip_size > (long long)size && zip_size < (long long)size + 200 * 1024, "压缩包的大小不对");
        char *bytes = zip_size > 0 ? xalloc((size_t)zip_size) : NULL;
        HANDLE file = CreateFileW(zip_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        DWORD read = 0;
        bool loaded = bytes && file != INVALID_HANDLE_VALUE && ReadFile(file, bytes, (DWORD)zip_size, &read, NULL) && read == (DWORD)zip_size;
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        mz_zip_archive reader;
        memset(&reader, 0, sizeof(reader));
        if (loaded && mz_zip_reader_init_mem(&reader, bytes, (size_t)zip_size, 0))
        {
            mz_zip_archive_file_stat stored = {0}, deflated = {0}, tiny = {0};
            int stored_at = mz_zip_reader_locate_file(&reader, "video.bin", NULL, 0);
            int deflated_at = mz_zip_reader_locate_file(&reader, "report.txt", NULL, 0);
            int small_at = mz_zip_reader_locate_file(&reader, "small.bin", NULL, 0);
            bool found = stored_at >= 0 && deflated_at >= 0 && small_at >= 0 && mz_zip_reader_file_stat(&reader, (mz_uint)stored_at, &stored) &&
                         mz_zip_reader_file_stat(&reader, (mz_uint)deflated_at, &deflated) && mz_zip_reader_file_stat(&reader, (mz_uint)small_at, &tiny);
            CHECK(found, "压缩包里少了文件");
            if (found)
            {
                CHECK(stored.m_method == 0 && stored.m_comp_size == size && stored.m_uncomp_size == size, "压不小的大文件没有原样存入");
                CHECK(deflated.m_method == MZ_DEFLATED && deflated.m_comp_size < size / 20, "能压小的大文件没有压缩");
                CHECK(tiny.m_method == MZ_DEFLATED, "小文件应照常压缩");
                size_t out_size = 0;
                void *out = mz_zip_reader_extract_to_heap(&reader, (mz_uint)stored_at, &out_size, 0);
                CHECK(out && out_size == size && memcmp(out, noise, size) == 0, "原样存入的文件解出来和原来不一样");
                mz_free(out);
                out = mz_zip_reader_extract_to_heap(&reader, (mz_uint)deflated_at, &out_size, 0);
                CHECK(out && out_size == size && memcmp(out, plain, size) == 0, "压缩的文件解出来和原来不一样");
                mz_free(out);
            }
            mz_zip_reader_end(&reader);
        }
        else CHECK(false, "无法读取压缩包");
        free(bytes);

        // 自己解压一遍：原样存入的条目照常解出来。
        StrList zips = {0};
        strlist_add(&zips, zip_path);
        QuickActionResult extracted = {0};
        archive_run(&zips, NULL, &extracted);
        CHECK(extracted.succeeded && extracted.outputs.count == 1, "含原样存入条目的压缩包解压失败");
        if (extracted.succeeded && extracted.outputs.count == 1)
        {
            wchar_t *back = path_join(extracted.outputs.items[0], L"video.bin");
            CHECK(size_of(back) == (long long)size, "解压出来的文件大小不对");
            free(back);
        }
        quick_action_result_free(&extracted);
        strlist_clear(&zips);
    }
    quick_action_result_free(&result);
    strlist_clear(&list);
    free(noise);
    free(plain);
    free(noise_path);
    free(plain_path);
    free(small_path);
    free(folder);
}

// ───────────── 图片 ─────────────

static void test_image_tools(void)
{
    HRESULT com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    wchar_t *folder = test_folder(L"images");
    wchar_t *large = path_join(folder, L"photo.png");
    CHECK(write_noisy_png(large, 900, 700), "无法生成测试图片");
    const long long limit = 80 * 1024;
    CHECK(size_of(large) > limit, "测试图片不够大");

    wchar_t *output = NULL;
    ImageStatus status = image_compress(large, limit, NULL, &output);
    GUID container = {0};
    UINT width = 0, height = 0;
    CHECK(status == IMAGE_DONE && output && wcscmp(path_file_name(output), L"photo-80KB.jpg") == 0 && size_of(output) <= limit, "压缩后的图片超过了限制或名称不对");
    CHECK(output && image_info(output, &container, &width, &height) && IsEqualGUID(&container, &GUID_ContainerFormatJpeg) && width <= 900, "输出不是 JPG");
    CHECK(path_exists(large), "原图被改动");
    wchar_t *again = NULL;
    CHECK(output && image_compress(output, limit, NULL, &again) == IMAGE_ALREADY, "已经足够小的图片不应再压缩");
    free(again);

    wchar_t *text = test_file(folder, L"note.txt", "x");
    CHECK(image_compress(text, limit, NULL, &again) == IMAGE_NOT_IMAGE, "非图片文件没有跳过");
    char *junk = xalloc(200001);
    memset(junk, 'x', 200000);
    junk[200000] = 0;
    wchar_t *broken = test_file(folder, L"broken.jpg", junk);
    free(junk);
    CHECK(image_compress(broken, limit, NULL, &again) == IMAGE_FAILED, "损坏的图片没有报告失败");
    wchar_t *formatted = image_format_limit(2 * 1024 * 1024);
    CHECK(owned_text_is(formatted, L"2MB") && owned_text_is(str_image_size_text(2048), L"2 MB") && owned_text_is(str_image_size_text(500), L"500 KB"),
        "大小文字不正确");

    // 细长的图片缩到最小仍然过大时报告失败，不保存名不副实的文件。
    wchar_t *banner = path_join(folder, L"banner.png");
    write_noisy_png(banner, 3000, 40);
    wchar_t *banner_output = NULL;
    ImageStatus banner_status = image_compress(banner, 10 * 1024, NULL, &banner_output);
    CHECK(banner_status == IMAGE_FAILED || (banner_status == IMAGE_DONE && size_of(banner_output) <= 10 * 1024), "细长图片压缩后超过了限制却报告成功");
    free(banner_output);

    // 转换：PNG 转 JPG，再转回 PNG 时不覆盖同名文件。
    wchar_t *shot = path_join(folder, L"shot.png");
    write_noisy_png(shot, 300, 200);
    wchar_t *jpg = NULL;
    CHECK(image_convert(shot, IMAGE_FORMAT_JPEG, NULL, &jpg) == IMAGE_DONE && jpg && wcscmp(path_file_name(jpg), L"shot.jpg") == 0, "PNG 没有转成 JPG");
    CHECK(jpg && image_info(jpg, &container, &width, &height) && IsEqualGUID(&container, &GUID_ContainerFormatJpeg) && width == 300 && height == 200,
        "转出的 JPG 格式或尺寸不对");
    wchar_t *unused = NULL;
    CHECK(jpg && image_convert(jpg, IMAGE_FORMAT_JPEG, NULL, &unused) == IMAGE_ALREADY, "已经是 JPG 的图片不应再转");
    wchar_t *back = NULL;
    CHECK(jpg && image_convert(jpg, IMAGE_FORMAT_PNG, NULL, &back) == IMAGE_DONE && back && wcscmp(path_file_name(back), L"shot (2).png") == 0,
        "转成 PNG 时覆盖了同名文件");
    CHECK(back && image_info(back, &container, &width, &height) && IsEqualGUID(&container, &GUID_ContainerFormatPng), "转出的不是 PNG");
    CHECK(image_convert(text, IMAGE_FORMAT_PNG, NULL, &unused) == IMAGE_NOT_IMAGE, "非图片文件没有跳过");
    wchar_t *broken_webp = test_file(folder, L"broken.webp", "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx");
    CHECK(image_convert(broken_webp, IMAGE_FORMAT_PNG, NULL, &unused) == IMAGE_FAILED, "损坏的图片没有报告失败");
    wchar_t *pattern = path_join(folder, L".dropshelf-*");
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW(pattern, &data);
    CHECK(find == INVALID_HANDLE_VALUE, "转换失败后留下了临时文件");
    if (find != INVALID_HANDLE_VALUE) FindClose(find);

    free(pattern);
    free(broken_webp);
    free(back);
    free(jpg);
    free(shot);
    free(banner);
    free(broken);
    free(text);
    free(output);
    free(large);
    free(folder);
    if (SUCCEEDED(com)) CoUninitialize();
}

// ───────────── 操作本身 ─────────────

typedef struct ActionWait
{
    volatile bool done;
    QuickActionResult result;
} ActionWait;

static void on_action_done(QuickActionResult *result, void *context)
{
    ActionWait *wait = context;
    wait->result = *result;
    // 结果交给测试，原来的那份清空，执行完释放时不会重复释放。
    *result = (QuickActionResult){0};
    wait->done = true;
}

static QuickAction builtin_action(QuickActionType type)
{
    for (int index = 0; index < quick_action_builtin_count(); index++)
        if (quick_action_builtin_type(index) == type) return (QuickAction){ wcs_dup(quick_action_builtin_id(index)), type, false, {0} };
    return (QuickAction){0};
}

static QuickAction custom_quick_action(int kind)
{
    QuickAction action = { NULL, QUICK_ACTION_CUSTOM, true, {0} };
    custom_action_init(&action.custom);
    action.custom.kind = kind;
    action.id = wcs_dup(action.custom.id);
    return action;
}

static void execute_and_wait(const QuickAction *action, const wchar_t *path, const wchar_t *folder, ActionWait *wait)
{
    QuickActionRequest request = {0};
    quick_action_clone(&request.action, action);
    strlist_add(&request.paths, path);
    request.folder = wcs_dup(folder);
    *wait = (ActionWait){0};
    quick_action_execute(&request, on_action_done, wait);
    pump_until(&wait->done, 20000);
}

typedef struct ModelWait
{
    volatile bool done;
    bool succeeded;
    int produced;
    StrList produced_paths;
    int notified;
    bool last_success;
} ModelWait;

static void model_finished(ShelfModel *model, const QuickActionResult *result, void *context)
{
    (void)model;
    ModelWait *wait = context;
    wait->succeeded = result->succeeded;
    wait->done = true;
}

static void model_produced(void *context, const StrList *paths)
{
    ModelWait *wait = context;
    wait->produced++;
    strlist_clone(&wait->produced_paths, paths);
}

static void model_notify(void *context, const wchar_t *message, bool success)
{
    (void)message;
    ModelWait *wait = context;
    wait->notified++;
    wait->last_success = success;
}

static void run_on_model(ShelfModel *model, const QuickAction *action, ModelWait *wait)
{
    wait->done = false;
    ShelfItem **items = xalloc((size_t)(model->count ? model->count : 1) * sizeof(ShelfItem *));
    int count = model->count;
    memcpy(items, model->items, (size_t)count * sizeof(ShelfItem *));
    shelf_model_run_action(model, action, items, count, NULL, model_finished, wait);
    free(items);
    pump_until(&wait->done, 20000);
}

/// 超过 260 个字符的路径：建文件夹、读写、统计大小、复制、移动、压缩和解压、图片转换、删除都照常工作。
/// 测试程序没有声明支持长路径，靠的是 path_for_io 加上的 \\?\。
static void test_long_paths(void)
{
    HRESULT com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    // 6 层，每层 50 多个字符。
    WStr chain = {0};
    for (int index = 0; index < 6; index++) wstr_appendf(&chain, L"\\%02d-深层文件夹-a-rather-long-folder-name-for-path-tests", index);
    wchar_t *base = test_folder(L"long-paths");
    WStr builder = {0};
    wstr_appendf(&builder, L"%ls%ls", base, chain.data);
    wchar_t *deep = wstr_take(&builder);
    const wchar_t *second = wcschr(chain.data + 1, L'\\');
    wchar_t *top_name = wcs_dup_n(chain.data + 1, (size_t)(second - chain.data - 1));
    wchar_t *top = path_join(base, top_name);
    CHECK(wcslen(deep) > 330, "测试用的路径不够长");

    // 短的不动；长的先整理成完整路径再加 \\?\；网络路径换成 \\?\UNC\。
    CHECK(owned_text_is(path_for_io(L"C:\\a\\..\\b.txt"), L"C:\\a\\..\\b.txt"), "短路径不应改动");
    wchar_t *prefixed = path_for_io(deep);
    CHECK(prefixed && wcsncmp(prefixed, L"\\\\?\\", 4) == 0 && wcscmp(prefixed + 4, deep) == 0, "长路径没有加上 \\\\?\\");
    free(prefixed);
    WStr messy = {0}, expected = {0};
    wstr_appendf(&messy, L"%ls/x/../y.txt", deep);
    wstr_appendf(&expected, L"\\\\?\\%ls\\y.txt", deep);
    CHECK(owned_text_is(path_for_io(messy.data), expected.data), "长路径没有先整理成完整路径");
    wstr_free(&messy);
    wstr_free(&expected);
    WStr unc = {0}, unc_expected = {0};
    wstr_appendf(&unc, L"\\\\server\\share%ls\\a.txt", chain.data);
    wstr_appendf(&unc_expected, L"\\\\?\\UNC\\server\\share%ls\\a.txt", chain.data);
    CHECK(owned_text_is(path_for_io(unc.data), unc_expected.data), "长的网络路径没有换成 \\\\?\\UNC\\ 的形式");
    wstr_free(&unc);
    wstr_free(&unc_expected);

    // 建文件夹、读写文件、文件信息、统计大小。
    CHECK(directory_create(deep) && path_is_directory(deep), "建不了长路径的文件夹");
    wchar_t *text = test_file(deep, L"说明.txt", "long text");
    CHECK(path_exists(text) && file_text_is(text, "long text"), "长路径的文件读写不正常");
    FileFacts facts;
    CHECK(file_facts_read(text, &facts) && !facts.is_folder && facts.has_size && facts.size_bytes == 9, "读不到长路径文件的信息");
    int children = 0;
    bool has_size = false;
    long long size = 0;
    CHECK(folder_measure(base, &children, &has_size, &size, NULL) && children == 1 && has_size && size == 9, "统计不了长路径里的大小");

    // 复制文件和整个文件夹（复制出来的路径同样很长）。
    wchar_t *outside = test_folder(L"long-paths-out");
    StrList list = {0};
    strlist_add(&list, text);
    FileOpResults results = file_ops_copy(&list, outside);
    CHECK(results.count == 1 && results.items[0].succeeded && results.items[0].destination && file_text_is(results.items[0].destination, "long text"),
        "长路径里的文件复制不出来");
    file_op_results_free(&results);
    strlist_clear(&list);
    strlist_add(&list, top);
    results = file_ops_copy(&list, outside);
    WStr copied = {0};
    wstr_appendf(&copied, L"%ls%ls\\说明.txt", outside, chain.data);
    CHECK(results.count == 1 && results.items[0].succeeded && file_text_is(copied.data, "long text"), "长路径的文件夹复制不过去");
    file_op_results_free(&results);
    wstr_free(&copied);

    // 压缩成 ZIP，再解压出来（解压出来的路径也很长）。
    QuickActionResult zipped = {0};
    archive_compress(&list, outside, &zipped);
    CHECK(zipped.succeeded && zipped.outputs.count == 1, "长路径的文件夹压缩失败");
    if (zipped.succeeded && zipped.outputs.count == 1)
    {
        StrList zips = {0};
        strlist_add(&zips, zipped.outputs.items[0]);
        QuickActionResult unzipped = {0};
        archive_run(&zips, NULL, &unzipped);
        int unzipped_children = 0;
        bool unzipped_has_size = false;
        long long unzipped_size = 0;
        CHECK(unzipped.succeeded && unzipped.outputs.count == 1 &&
              folder_measure(unzipped.outputs.items[0], &unzipped_children, &unzipped_has_size, &unzipped_size, NULL) && unzipped_has_size &&
              unzipped_size == 9, "长路径的文件解压不出来");
        quick_action_result_free(&unzipped);
        strlist_clear(&zips);
    }
    quick_action_result_free(&zipped);

    // 图片转换：读和写都在长路径里。
    wchar_t *picture = path_join(deep, L"photo.png");
    wchar_t *picture_io = path_for_io(picture);
    CHECK(write_noisy_png(picture_io, 64, 48), "无法在长路径里生成测试图片");
    wchar_t *jpg = NULL;
    CHECK(image_convert(picture, IMAGE_FORMAT_JPEG, NULL, &jpg) == IMAGE_DONE && jpg && path_exists(jpg), "长路径里的图片没有转成 JPG");
    free(jpg);
    free(picture_io);
    free(picture);

    // 移动整个文件夹，最后连同里面的长路径一起删掉。
    wchar_t *moved_to = test_folder(L"long-paths-moved");
    results = file_ops_move(&list, moved_to);
    WStr moved = {0};
    wstr_appendf(&moved, L"%ls%ls\\说明.txt", moved_to, chain.data);
    CHECK(results.count == 1 && results.items[0].succeeded && !path_exists(top) && file_text_is(moved.data, "long text"), "长路径的文件夹移动失败");
    file_op_results_free(&results);
    wstr_free(&moved);
    CHECK(directory_delete_tree(moved_to) && !path_exists(moved_to) && directory_delete_tree(outside) && !path_exists(outside),
        "删不掉含长路径的文件夹");

    strlist_clear(&list);
    directory_delete_tree(base);
    free(moved_to);
    free(outside);
    free(text);
    free(top);
    free(top_name);
    free(deep);
    free(base);
    wstr_free(&chain);
    if (SUCCEEDED(com)) CoUninitialize();
}

static void test_quick_actions(void)
{
    language_set_mode(LANGUAGE_CHINESE);
    HRESULT ole = OleInitialize(NULL);
    task_initialize();

    // 名称、属性和参数。
    QuickAction convert = custom_quick_action(CUSTOM_ACTION_CONVERT_IMAGE);
    convert.custom.image_format = IMAGE_FORMAT_PNG;
    CHECK(owned_text_is(quick_action_label(&convert), L"→ PNG") && owned_text_is(quick_action_title(&convert), L"图片转成 PNG") &&
          quick_action_shows_progress(&convert) && !quick_action_takes_text(&convert), "转换图片的名称或属性不正确");
    QuickAction compress_image = custom_quick_action(CUSTOM_ACTION_COMPRESS_IMAGE);
    compress_image.custom.image_size_kb = 2048;
    CHECK(owned_text_is(quick_action_label(&compress_image), L"≤ 2 MB") && owned_text_is(quick_action_title(&compress_image), L"压缩图片到 2 MB"),
        "压缩图片的名称不正确");
    QuickAction zip_action = builtin_action(QUICK_ACTION_COMPRESS_ZIP);
    CHECK(owned_text_is(quick_action_label(&zip_action), L"压缩") && quick_action_takes_text(&zip_action) &&
          owned_text_is(quick_action_tooltip(&zip_action), L"压缩：压缩为 ZIP；只有一个 ZIP 或 7z 时改为解压到它旁边。"), "压缩的名称或提示不对");
    QuickAction copy_path = builtin_action(QUICK_ACTION_COPY_FULL_PATH);
    CHECK(owned_text_is(quick_action_label(&copy_path), L"复制路径") && owned_text_is(quick_action_title(&copy_path), S(STR_MenuCopyPath)),
        "复制路径的名称不对");
    language_set_mode(LANGUAGE_ENGLISH);
    CHECK(owned_text_is(quick_action_label(&zip_action), L"Compress"), "英文名称不对");
    wchar_t *moved_message = str_moved_to(1, L"Docs");
    CHECK(owned_text_is(moved_message, L"Moved 1 item to Docs"), "英文的移动提示不对");
    CHECK(owned_text_is(str_images_skipped_small(1), L", 1 already fits") && owned_text_is(str_images_skipped_small(2), L", 2 already fit"),
        "英文的单复数不对");
    CHECK(owned_text_is(str_move_left_behind(1), L"; 1 item couldn't be fully removed from the old location"), "英文的残留提示不对");
    language_set_mode(LANGUAGE_CHINESE);
    CHECK(owned_text_is(str_opened_with(2, L"画图"), L"已用 画图 打开 2 项"), "用程序打开的提示不对");
    CHECK(owned_text_is(str_move_left_behind(2), L"，2 项没能从原位置删干净"), "残留的提示不对");

    StrList arguments = {0};
    strlist_add(&arguments, L"C:\\a b\\x.png");
    strlist_add(&arguments, L"D:\\");
    CHECK(owned_text_is(quick_action_build_arguments(NULL, &arguments), L"\"C:\\a b\\x.png\" \"D:\\\\\""), "路径引号不正确");
    strlist_clear(&arguments);
    strlist_add(&arguments, L"C:\\x.png");
    CHECK(owned_text_is(quick_action_build_arguments(L"-q 80 {FILES} --out", &arguments), L"-q 80 \"C:\\x.png\" --out"), "{files} 没有替换");
    CHECK(owned_text_is(quick_action_build_arguments(L" --edit ", &arguments), L"--edit \"C:\\x.png\""), "没有 {files} 时文件应放在最后");
    strlist_clear(&arguments);
    wchar_t *notepad = program_display_name(L"C:\\Windows\\System32\\notepad.exe");
    CHECK(notepad && *notepad, "程序名称为空");
    free(notepad);

    // 设置里的布局决定显示哪几个。
    AppSettings settings;
    settings_init(&settings);
    settings_normalize(&settings);
    settings.quick_action_layout = LAYOUT_TWO;
    QuickActionList visible = quick_actions_visible(&settings);
    CHECK(visible.count == 2 && visible.items[0].type == QUICK_ACTION_COMPRESS_ZIP && visible.items[1].type == QUICK_ACTION_COPY_FULL_PATH,
        "两个操作时显示的不对");
    settings.quick_action_layout = LAYOUT_FOUR;
    QuickActionList four = quick_actions_visible(&settings);
    CHECK(four.count == 4 && !quick_action_list_same(&visible, &four), "四个操作时显示的不对");
    settings.quick_action_layout = LAYOUT_OFF;
    QuickActionList none = quick_actions_visible(&settings);
    CHECK(none.count == 0, "关闭快速操作时不应显示");
    quick_action_list_free(&visible);
    quick_action_list_free(&four);
    quick_action_list_free(&none);
    settings_free(&settings);

    // 移动到：取消选择文件夹时不动文件。
    wchar_t *folder = test_folder(L"move-to");
    wchar_t *destination = test_folder(L"move-to-destination");
    wchar_t *source = test_file(folder, L"move-me.txt", "move");
    QuickAction move = builtin_action(QUICK_ACTION_MOVE_TO);
    ActionWait wait;
    execute_and_wait(&move, source, NULL, &wait);
    CHECK(wait.done && wait.result.cancelled && path_exists(source), "取消选择文件夹后仍然移动了文件");
    quick_action_result_free(&wait.result);
    execute_and_wait(&move, source, destination, &wait);
    wchar_t *moved = path_join(destination, L"move-me.txt");
    CHECK(wait.result.succeeded && wait.result.move_destinations.count == 1 && wcs_equal(wait.result.move_destinations.items[0], moved) &&
          !path_exists(source) && file_text_is(moved, "move"), "文件没有移动到目标位置");
    CHECK(owned_text_is(wcs_dup(wait.result.message), L"已移动 1 项到 move-to-destination"), "移动的提示不对");
    quick_action_result_free(&wait.result);

    // 发送到文件夹：复制、移动，已经在目标里的不改名，文件夹不在时提示。
    wchar_t *custom_source = test_folder(L"custom-folder-source");
    wchar_t *target = test_folder(L"custom-folder-target");
    wchar_t *copy_source = test_file(custom_source, L"copy.txt", "copy");
    wchar_t *move_source = test_file(custom_source, L"move.txt", "move");
    QuickAction copy_to = custom_quick_action(CUSTOM_ACTION_FOLDER);
    copy_to.custom.folder = wcs_dup(target);
    copy_to.custom.folder_mode = TRANSFER_COPY;
    execute_and_wait(&copy_to, copy_source, NULL, &wait);
    wchar_t *copied = path_join(target, L"copy.txt");
    CHECK(wait.result.succeeded && path_exists(copy_source) && file_text_is(copied, "copy"), "复制到文件夹没有成功");
    quick_action_result_free(&wait.result);

    ModelWait model_wait = {0};
    ShelfModelEvents events = { &model_wait, NULL, model_notify, NULL, model_produced };
    ShelfModel *model = shelf_model_new(&events);
    StrList one = {0};
    strlist_add(&one, move_source);
    shelf_model_add_files(model, &one);
    strlist_clear(&one);
    QuickAction move_to = custom_quick_action(CUSTOM_ACTION_FOLDER);
    move_to.custom.folder = wcs_dup(target);
    move_to.custom.folder_mode = TRANSFER_MOVE;
    run_on_model(model, &move_to, &model_wait);
    wchar_t *moved_into = path_join(target, L"move.txt");
    CHECK(model_wait.succeeded && !path_exists(move_source) && model->count == 1 && wcs_equal(model->items[0]->value, moved_into),
        "移动后置物架上的项目没有指向新位置");
    run_on_model(model, &move_to, &model_wait);
    wchar_t *renamed = path_join(target, L"move (2).txt");
    CHECK(model_wait.succeeded && !path_exists(renamed), "已在目标文件夹中的项目被重命名");
    free(move_to.custom.folder);
    move_to.custom.folder = path_join(target, L"gone");
    execute_and_wait(&move_to, copy_source, NULL, &wait);
    CHECK(!wait.result.succeeded && owned_text_is(wcs_dup(wait.result.message), L"找不到文件夹“gone”"), "目标文件夹不存在时没有提示");
    quick_action_result_free(&wait.result);
    shelf_model_release_resources(model);
    shelf_model_release(model);

    // 压缩：已有内容的置物架把压缩包交出去放进新的置物架；空置物架提示先放文件。
    wchar_t *report_folder = test_folder(L"archive-shelf");
    wchar_t *report = test_file(report_folder, L"report.txt", "report");
    model_wait = (ModelWait){0};
    model = shelf_model_new(&events);
    strlist_add(&one, report);
    shelf_model_add_files(model, &one);
    strlist_clear(&one);
    run_on_model(model, &zip_action, &model_wait);
    CHECK(model_wait.succeeded && model->count == 1 && model_wait.produced == 1 && model_wait.produced_paths.count == 1 &&
          wcsstr(model_wait.produced_paths.items[0], L"report.zip"), "压缩结果没有放进新的置物架");
    CHECK(model_wait.notified == 1 && model_wait.last_success && !model->busy, "操作结果没有提示");
    strlist_clear(&model_wait.produced_paths);

    // 文本在需要文件的操作之前先转成文件。
    model_wait.notified = 0;
    ShelfModel *texts = shelf_model_new(&events);
    DropReadResult *note = xcalloc(1, sizeof(DropReadResult));
    note->entries = xcalloc(1, sizeof(DroppedEntry));
    note->entries[0].kind = ITEM_TEXT;
    note->entries[0].value = wcs_dup(L"会议纪要");
    note->count = 1;
    shelf_model_add(texts, note);
    drop_result_free(note);
    run_on_model(texts, &zip_action, &model_wait);
    CHECK(model_wait.succeeded && texts->count == 1 && texts->items[0]->kind == ITEM_FILE, "文本没有先转成文件再压缩");
    strlist_clear(&model_wait.produced_paths);
    shelf_model_release_resources(texts);
    shelf_model_release(texts);

    ShelfModel *empty = shelf_model_new(&events);
    run_on_model(empty, &zip_action, &model_wait);
    CHECK(model_wait.done && !model_wait.succeeded && !model_wait.last_success, "空置物架执行操作没有提示");
    shelf_model_release(empty);
    shelf_model_release_resources(model);
    shelf_model_release(model);

    // 压缩图片：结果替换置物架上的原图，标识不变，原图留着。
    HRESULT com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    wchar_t *image_folder = test_folder(L"compress-replace");
    wchar_t *scan = path_join(image_folder, L"scan.png");
    write_noisy_png(scan, 800, 600);
    model_wait = (ModelWait){0};
    model = shelf_model_new(&events);
    strlist_add(&one, scan);
    shelf_model_add_files(model, &one);
    strlist_clear(&one);
    wchar_t *id = model->count ? wcs_dup(model->items[0]->id) : NULL;
    compress_image.custom.image_size_kb = 60;
    run_on_model(model, &compress_image, &model_wait);
    CHECK(model_wait.succeeded && model->count == 1 && wcsstr(model->items[0]->value, L"scan-60KB.jpg") && wcs_equal(model->items[0]->id, id),
        "压缩后的图片没有替换原图");
    CHECK(path_exists(scan), "原图被删除");
    free(id);
    shelf_model_release_resources(model);
    shelf_model_release(model);
    if (SUCCEEDED(com)) CoUninitialize();

    quick_action_free(&convert);
    quick_action_free(&compress_image);
    quick_action_free(&zip_action);
    quick_action_free(&copy_path);
    quick_action_free(&move);
    quick_action_free(&copy_to);
    quick_action_free(&move_to);
    free(folder);
    free(destination);
    free(source);
    free(moved);
    free(custom_source);
    free(target);
    free(copy_source);
    free(move_source);
    free(copied);
    free(moved_into);
    free(renamed);
    free(report_folder);
    free(report);
    free(image_folder);
    free(scan);
    if (SUCCEEDED(ole)) OleUninitialize();
}

// ───────────── 置物架上的快速操作 ─────────────

static void test_shelf_actions(void)
{
    language_set_mode(LANGUAGE_CHINESE);
    HRESULT ole = OleInitialize(NULL);
    task_initialize();
    ShelfWindow *shelf = shelves_open_at((POINT){ 500, 400 }, false, false, EDGE_NONE);
    CHECK(shelf != NULL, "没有打开置物架");
    if (shelf)
    {
        float collapsed_width, collapsed_height;
        shelf_target_size(shelf, false, &collapsed_width, &collapsed_height);
        CHECK(shelf->actions.count == app_settings()->quick_action_layout, "置物架上的快速操作数量和设置不一致");
        if (shelf->actions.count > 0)
        {
            // 窗口底部是按钮栏，按钮均分宽度；拖入时卡片从按钮栏的位置向上展开。
            shelf_layout(shelf);
            const ShelfLayout *layout = &shelf->layout;
            CHECK(fabsf(layout->body.bottom - (layout->height - 52)) < 0.01f && layout->action_buttons[0].bottom == layout->height - 8,
                "按钮栏的位置不对");
            CHECK(fabsf(layout->tiles_host.top - (layout->height - SHELF_ACTION_BAR_HEIGHT)) < 0.01f, "卡片收起时应和按钮栏一样高");
            shelf_reset_tiles(shelf);
            shelf_set_hovered_action(shelf, 0, true);
            CHECK(shelf->tile_highlight[0].value == 1 && shelf->frame_opacity.value < 0.5, "拖到卡片上时没有高亮");
            shelf_set_hovered_action(shelf, -1, true);
            CHECK(shelf->tile_highlight[0].value == 0 && shelf->frame_opacity.value == 1, "离开卡片后没有恢复");
        }
        // 生成的新文件放进旁边的新置物架。
        wchar_t *folder = test_folder(L"shelf-actions");
        wchar_t *made = test_file(folder, L"made.zip", "zip");
        StrList paths = {0};
        strlist_add(&paths, made);
        // 交出新文件的置物架上已经有东西（用过了），不会被当成空的复用。
        shelf->model->has_interacted = true;
        int before = shelves_open_count();
        shelves_open_beside(shelf, &paths);
        CHECK(shelves_open_count() == before + 1, "没有在旁边打开新的置物架");
        strlist_clear(&paths);
        free(made);
        free(folder);
        pump_for(300);
    }
    shelves_close_all();
    pump_for(100);
    if (SUCCEEDED(ole)) OleUninitialize();
}
