// 第 7 步：主窗口、关于、引导，以及它们用到的文字、检查更新和翻译模板。

static void test_update_check(void)
{
    int major = 0, minor = 0, patch = 0;
    CHECK(update_parse_tag(L"v2.8.5", &major, &minor, &patch) && major == 2 && minor == 8 && patch == 5, "v2.8.5 没有读对");
    CHECK(update_parse_tag(L"V3.0", &major, &minor, &patch) && major == 3 && minor == 0 && patch == 0, "两段的版本号应补成 3.0.0");
    CHECK(!update_parse_tag(L"version-2", &major, &minor, &patch), "前缀后面不是数字的不算");
    CHECK(!update_parse_tag(L"style-v2.8.5", &major, &minor, &patch), "别的前缀不算");
    CHECK(!update_parse_tag(L"v2.8.5-beta", &major, &minor, &patch), "后面还有别的不算");

    const wchar_t *json =
        L"[ { \"tag_name\": \"v2.9.0\", \"draft\": true, \"html_url\": \"https://github.com/wangmingkun148/dropshelf/releases/tag/v2.9.0\" },"
        L"  { \"tag_name\": \"v2.8.9\", \"prerelease\": true },"
        L"  { \"tag_name\": \"v2.8.4\", \"html_url\": \"https://github.com/wangmingkun148/dropshelf/releases/tag/v2.8.4\" },"
        L"  { \"tag_name\": \"style-v9.0.0\" },"
        L"  { \"tag_name\": \"v2.8.3\", \"html_url\": \"https://evil.example/x\" } ]";
    UpdateInfo info;
    CHECK(update_find_newer(json, 2, 8, 2, &info) == UPDATE_NEWER && info.major == 2 && info.minor == 8 && info.patch == 4, "应找到 2.8.4");
    CHECK(info.url && wcscmp(info.url, L"https://github.com/wangmingkun148/dropshelf/releases/tag/v2.8.4") == 0, "下载页没有读对");
    free(info.url);
    CHECK(update_find_newer(L"[ { \"tag_name\": \"v2.8.3\", \"html_url\": \"https://evil.example/x\" } ]", 2, 8, 2, &info) == UPDATE_NEWER &&
              wcscmp(info.url, UPDATE_RELEASES_PAGE) == 0, "别的网站的地址应换成发布页");
    free(info.url);
    CHECK(update_find_newer(json, 2, 8, 4, &info) == UPDATE_LATEST, "已经是最新时应返回 LATEST");
    CHECK(update_find_newer(L"{ \"message\": \"rate limited\" }", 2, 8, 2, &info) == UPDATE_ERROR, "不是发布列表时应返回 ERROR");

    // 与 C# 版 TestUpdateChecker 相同的发布列表：版本号按数字比较，别的前缀、草稿、预发布和没有标签的都不算。
    CHECK(!update_parse_tag(L"style2.8.5", &major, &minor, &patch) && !update_parse_tag(L"vnext", &major, &minor, &patch) &&
          !update_parse_tag(L"v", &major, &minor, &patch) && !update_parse_tag(NULL, &major, &minor, &patch), "别的前缀或不是版本号的标签被当成了版本");
    const wchar_t *releases =
        L"[ { \"tag_name\": \"style9.9.9\", \"html_url\": \"https://github.com/wangmingkun148/dropshelf/releases/tag/style9.9.9\" },"
        L"  { \"tag_name\": \"v9.0.0\", \"draft\": true, \"html_url\": \"https://github.com/wangmingkun148/dropshelf/releases/tag/v9.0.0\" },"
        L"  { \"tag_name\": \"v8.0.0\", \"prerelease\": true, \"html_url\": \"https://github.com/wangmingkun148/dropshelf/releases/tag/v8.0.0\" },"
        L"  { \"tag_name\": \"v2.9.1\", \"draft\": false, \"prerelease\": false, \"html_url\": \"https://github.com/wangmingkun148/dropshelf/releases/tag/v2.9.1\" },"
        L"  { \"tag_name\": \"vTAG\", \"html_url\": \"https://example.com/elsewhere\" },"
        L"  { \"tag_name\": \"v2.8.0\", \"html_url\": \"https://github.com/wangmingkun148/dropshelf/releases/tag/v2.8.0\" },"
        L"  { \"name\": \"没有标签\" } ]";
    // vTAG 是占位：一份换成 v2.10.0，另一份换成 v2.1.0。
    WStr newer_list = {0}, older_list = {0};
    const wchar_t *mark = wcsstr(releases, L"vTAG");
    wstr_append_n(&newer_list, releases, (size_t)(mark - releases));
    wstr_append(&newer_list, L"v2.10.0");
    wstr_append(&newer_list, mark + 4);
    wstr_append_n(&older_list, releases, (size_t)(mark - releases));
    wstr_append(&older_list, L"v2.1.0");
    wstr_append(&older_list, mark + 4);
    CHECK(update_find_newer(newer_list.data, 2, 8, 5, &info) == UPDATE_NEWER && info.major == 2 && info.minor == 10 && info.patch == 0 &&
              wcscmp(info.url, UPDATE_RELEASES_PAGE) == 0, "没有找到最新的正式版本");
    free(info.url);
    CHECK(update_find_newer(newer_list.data, 2, 9, 0, &info) == UPDATE_NEWER && info.minor == 10, "版本号没有按数字比较");
    free(info.url);
    CHECK(update_find_newer(older_list.data, 2, 8, 5, &info) == UPDATE_NEWER && info.minor == 9 && info.patch == 1 &&
              wcscmp(info.url, L"https://github.com/wangmingkun148/dropshelf/releases/tag/v2.9.1") == 0, "下载页的地址不对");
    free(info.url);
    CHECK(update_find_newer(newer_list.data, 2, 10, 0, &info) == UPDATE_LATEST && update_find_newer(L"[]", 1, 0, 0, &info) == UPDATE_LATEST,
        "已经是最新版本时仍然提示有更新");
    wstr_free(&newer_list);
    wstr_free(&older_list);
    CHECK(update_parse_tag(L"v" DROPSHELF_VERSION, &major, &minor, &patch) && major >= 2, "当前版本号不正确");
}

static void test_main_strings(void)
{
    language_set_mode(LANGUAGE_CHINESE);
    wchar_t *text = str_general_summary(LANGUAGE_CHINESE, true, L"Ctrl+Shift+Space");
    CHECK(wcscmp(text, L"中文 · 开机启动 · Ctrl+Shift+Space") == 0, "常规的摘要不对");
    free(text);
    text = str_general_summary(LANGUAGE_SYSTEM, false, NULL);
    CHECK(wcscmp(text, L"语言跟随系统 · 不开机启动 · 未设置快捷键") == 0, "没有快捷键时的摘要不对");
    free(text);
    StrList labels = {0};
    text = str_actions_summary(&labels);
    CHECK(wcscmp(text, L"不显示快速操作") == 0, "没有快速操作时的摘要不对");
    free(text);
    strlist_add(&labels, L"压缩");
    strlist_add(&labels, L"属性");
    text = str_actions_summary(&labels);
    CHECK(wcscmp(text, L"压缩、属性") == 0, "快速操作的摘要不对");
    free(text);
    strlist_clear(&labels);
    text = str_status_running_detail(TRIGGER_SHAKE, L"Ctrl+Q");
    CHECK(wcscmp(text, L"拖住文件并快速晃动鼠标，或按 Ctrl+Q 打开置物架") == 0, "运行状态的说明不对");
    free(text);
    text = str_sensitivity_detail(3, 38);
    CHECK(wcscmp(text, L"来回晃动约 38 像素、掉头 3 次即可弹出") == 0, "灵敏度的说明不对");
    free(text);

    language_set_mode(LANGUAGE_ENGLISH);
    text = str_history_count(1);
    CHECK(wcscmp(text, L"1 recently closed shelf kept") == 0, "英文的单数不对");
    free(text);
    text = str_shelf_summary(TRIGGER_EDGE, TRANSFER_COPY, 0);
    CHECK(wcscmp(text, L"Pause at edge to open · Drag out to copy") == 0, "英文的置物架摘要不对");
    free(text);
    text = str_shelf_summary(TRIGGER_EDGE, TRANSFER_COPY, 1);
    CHECK(wcscmp(text, L"Pause at edge to open · Drag out to copy · Watching 1 folder") == 0, "英文的置物架摘要没有写观察的文件夹");
    free(text);
    language_set_mode(LANGUAGE_CHINESE);
}

static void test_language_template(void)
{
    wchar_t *json = language_template_build(true);
    JsonValue *root = json_parse(json, 0);
    CHECK(root && root->type == JSON_OBJECT, "模板应是一个 JSON 对象");
    const JsonValue *strings = root ? json_get(root, L"strings") : NULL;
    const JsonValue *reference = root ? json_get(root, L"reference") : NULL;
    CHECK(strings && strings->count > 300, "模板里的原文太少");
    CHECK(json_get(root, L"from") && wcscmp(json_get(root, L"from")->string, L"zh") == 0, "对照中文时 from 应是 zh");
    CHECK(strings && json_get(strings, L"打开置物架") && json_get(strings, L"已添加 {0} 项") && json_get(strings, L"来回晃动约 {0} 像素、掉头 {1} 次即可弹出"),
        "应包含没有参数和带参数的原文");
    CHECK(reference && json_get(reference, L"打开置物架") && wcscmp(json_get(reference, L"打开置物架")->string, L"Open shelf") == 0, "对照的英文不对");
    json_free(root);
    free(json);

    // 写出的模板能直接当语言包读回来（填了译文以后）。
    json = language_template_build(false);
    root = json_parse(json, 0);
    CHECK(root && json_get(root, L"from") && wcscmp(json_get(root, L"from")->string, L"en") == 0, "对照英文时 from 应是 en");
    CHECK(root && json_get(json_get(root, L"strings"), L"Added {0}"), "应包含英文模板");
    json_free(root);
    free(json);
}

static void test_matrix_invert(void)
{
    GfxMatrix matrix = gfx_matrix_multiply(gfx_matrix_scale(2, 0.5f, 10, 20), gfx_matrix_rotate(30, 5, 5)), inverse;
    CHECK(gfx_matrix_invert(matrix, &inverse), "应能求逆");
    float x = 13, y = -7;
    gfx_matrix_apply(matrix, &x, &y);
    gfx_matrix_apply(inverse, &x, &y);
    CHECK(fabsf(x - 13) < 1e-3f && fabsf(y + 7) < 1e-3f, "逆变换应回到原处");
    CHECK(!gfx_matrix_invert((GfxMatrix){ 0, 0, 0, 1, 0, 0 }, &inverse), "缩放为 0 时没有逆");
}

static DWORD WINAPI release_image_elsewhere(void *parameter)
{
    gfx_image_release(parameter);
    return 0;
}

/// 在别的线程上放掉图片的最后一个引用（缩略图缓存满了换掉旧的）：显卡上的位图不当场释放，留到界面线程下次开始画时。
static void test_image_release_on_other_thread(void)
{
    unsigned char pixels[4 * 4 * 4];
    memset(pixels, 0xFF, sizeof(pixels));
    GfxImage *image = gfx_image_create(pixels, 4, 4, 16);
    // 先画一次，显卡上才有对应的位图。
    bool drawn = gfx_begin_image(8, 8, 96);
    if (drawn)
    {
        gfx_draw_image(image, (GfxRect){ 0, 0, 8, 8 }, 1);
        HBITMAP bitmap = gfx_end_image_bitmap();
        if (bitmap) DeleteObject(bitmap);
    }
    CHECK(drawn, "没能画进图片");
    int before = gfx_retired_bitmap_count();
    HANDLE thread = CreateThread(NULL, 0, release_image_elsewhere, image, 0, NULL);
    CHECK(thread != NULL, "没能开线程");
    if (!thread) return;
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    CHECK(gfx_retired_bitmap_count() == before + 1, "别的线程放掉图片时，显卡上的位图应留给界面线程释放");
    // 界面线程开始画下一帧时释放。
    if (gfx_begin_image(8, 8, 96))
    {
        HBITMAP bitmap = gfx_end_image_bitmap();
        if (bitmap) DeleteObject(bitmap);
    }
    CHECK(gfx_retired_bitmap_count() == 0, "界面线程开始画时应释放留下的位图");
}

/// 位图上 (x, y) 处的像素：自上而下、预乘透明度的 BGRA。
static const uint8_t *bitmap_pixel(HBITMAP bitmap, int x, int y)
{
    static const uint8_t none[4] = {0};
    DIBSECTION section;
    if (!bitmap || GetObjectW(bitmap, sizeof(section), &section) != sizeof(section) || !section.dsBm.bmBits) return none;
    return (const uint8_t *)section.dsBm.bmBits + (size_t)y * section.dsBm.bmWidthBytes + (size_t)x * 4;
}

static bool pixel_near(const uint8_t *pixel, int b, int g, int r, int a)
{
    return abs(pixel[0] - b) <= 2 && abs(pixel[1] - g) <= 2 && abs(pixel[2] - r) <= 2 && abs(pixel[3] - a) <= 2;
}

/// 层和直接盖色：省掉的层（不裁剪、不透明）弹出时不能多弹；盖色不和底下混合，用完恢复普通的叠加。
static void test_gfx_layers(void)
{
    HBITMAP bitmap = NULL;
    if (gfx_begin_image(40, 20, 96))
    {
        gfx_clear(COLOR_RGB(0, 0, 255));
        // 左半边：不开的层套着圆角裁剪和半透明。
        gfx_push_opacity(1);
        gfx_push_layer((GfxRect){ 2, 2, 18, 18 }, 4, 1);
        gfx_push_opacity(0.5f);
        gfx_fill_rect((GfxRect){ 0, 0, 40, 20 }, COLOR_RGB(255, 0, 0));
        gfx_pop_layer();
        gfx_pop_layer();
        gfx_pop_layer();
        gfx_fill_rect((GfxRect){ 0, 0, 1, 1 }, COLOR_RGB(0, 255, 0));
        // 右半边：红底上（经过平移）直接换成半透明的绿，之后再画的照常叠加。
        gfx_fill_rect((GfxRect){ 20, 0, 40, 20 }, COLOR_RGB(255, 0, 0));
        gfx_fill_rounded_replace((GfxRect){ 0, 0, 12, 12 }, 0, gfx_matrix_translate(24, 4), COLOR_ARGB(0x80, 0, 255, 0));
        gfx_fill_rect((GfxRect){ 37, 0, 40, 20 }, COLOR_ARGB(0x80, 0, 255, 0));
        bitmap = gfx_end_image_bitmap();
    }
    CHECK(bitmap != NULL, "没能画进图片（层没有成对弹出时 Direct2D 会报错）");
    CHECK(pixel_near(bitmap_pixel(bitmap, 10, 10), 128, 0, 128, 255), "半透明的层没有按一半透明度叠加");
    CHECK(pixel_near(bitmap_pixel(bitmap, 1, 10), 255, 0, 0, 255) && pixel_near(bitmap_pixel(bitmap, 2, 2), 255, 0, 0, 255),
        "圆角裁剪以外不应画上");
    CHECK(pixel_near(bitmap_pixel(bitmap, 0, 0), 0, 255, 0, 255), "层弹出以后裁剪没有解除");
    CHECK(pixel_near(bitmap_pixel(bitmap, 30, 10), 0, 128, 0, 128), "盖色应直接换掉底下的颜色，不和它混合");
    CHECK(pixel_near(bitmap_pixel(bitmap, 22, 10), 0, 0, 255, 255) && pixel_near(bitmap_pixel(bitmap, 30, 2), 0, 0, 255, 255),
        "盖色超出了范围");
    CHECK(pixel_near(bitmap_pixel(bitmap, 38, 10), 0, 128, 127, 255), "盖色以后应恢复普通的叠加");
    if (bitmap) DeleteObject(bitmap);
}

/// 画一个阴影，取回像素（自上而下、32 位、预乘透明度）。失败时返回 NULL。
static unsigned char *shadow_pixels(int width, int height, float dpi, GfxMatrix matrix, GfxRect rect, float radius, float blur, Color color,
    bool templates)
{
    gfx_set_shadow_templates(templates);
    unsigned char *pixels = NULL;
    if (gfx_begin_image(width, height, dpi))
    {
        gfx_clear(COLOR_TRANSPARENT);
        gfx_push_transform(matrix);
        gfx_shadow(rect, radius, blur, 0, 4, color);
        gfx_pop_transform();
        HBITMAP bitmap = gfx_end_image_bitmap();
        DIBSECTION section;
        if (bitmap && GetObjectW(bitmap, sizeof(section), &section) == sizeof(section) && section.dsBm.bmBits)
        {
            pixels = xalloc((size_t)width * height * 4);
            memcpy(pixels, section.dsBm.bmBits, (size_t)width * height * 4);
        }
        if (bitmap) DeleteObject(bitmap);
    }
    gfx_set_shadow_templates(true);
    return pixels;
}

/// 每次现算和按模板拼出来的阴影逐像素比较。
static void compare_shadow(const char *name, int width, int height, float dpi, GfxMatrix matrix, GfxRect rect, float radius, float blur, Color color)
{
    unsigned char *expected = shadow_pixels(width, height, dpi, matrix, rect, radius, blur, color, false);
    unsigned char *actual = shadow_pixels(width, height, dpi, matrix, rect, radius, blur, color, true);
    CHECK(expected && actual, "没能画出阴影");
    if (expected && actual)
    {
        int largest = 0, strongest = 0;
        double total = 0;
        size_t count = (size_t)width * height * 4;
        for (size_t index = 0; index < count; index++)
        {
            int difference = abs((int)expected[index] - (int)actual[index]);
            if (difference > largest) largest = difference;
            total += difference;
            if (index % 4 == 3 && expected[index] > strongest) strongest = expected[index];
        }
        double mean = total / (double)count;
        bool same = largest <= 8 && mean <= 0.5;
        if (!same || strongest < 20)
            printf("  阴影 %s：最大差 %d，平均差 %.3f，现算的阴影最浓处透明度 %d\n", name, largest, mean, strongest);
        CHECK(strongest >= 20, "现算的阴影没有画出来");
        CHECK(same, "按模板拼出的阴影和现算的差得太多");
    }
    free(expected);
    free(actual);
}

static void test_shadow_templates(void)
{
    GfxMatrix identity = gfx_matrix_identity();
    // 置物架上的牌：100%、150% 缩放，以及转了一个角度、缩小一点（出场时）。
    compare_shadow("牌", 200, 200, 96, identity, (GfxRect){ 50, 50, 122, 122 }, 10, 10, COLOR_ARGB(41, 0, 0, 0));
    int before = gfx_shadow_template_count();
    CHECK(before >= 1, "画过阴影后应有模板");
    compare_shadow("牌 150%", 300, 300, 144, identity, (GfxRect){ 50, 50, 122, 122 }, 10, 10, COLOR_ARGB(41, 0, 0, 0));
    GfxMatrix turned = gfx_matrix_multiply(gfx_matrix_scale(0.9f, 0.9f, 86, 86), gfx_matrix_rotate(-7, 86, 86));
    compare_shadow("转动的牌", 300, 300, 144, turned, (GfxRect){ 50, 50, 122, 122 }, 10, 10, COLOR_ARGB(41, 0, 0, 0));
    // 放大两倍：模板按两倍的像素画。
    compare_shadow("放大的牌", 400, 400, 96, gfx_matrix_scale(2, 2, 0, 0), (GfxRect){ 30, 30, 102, 102 }, 10, 10, COLOR_ARGB(41, 0, 0, 0));
    // 对话框那样的大矩形：四条边拉长。
    compare_shadow("对话框", 600, 500, 120, identity, (GfxRect){ 40, 40, 400, 330 }, 8, 32, COLOR_ARGB(56, 0, 0, 0));
    // 同样的圆角、模糊和颜色只是透明度不同，用同一个模板。
    int templates = gfx_shadow_template_count();
    compare_shadow("淡一些的对话框", 600, 500, 120, identity, (GfxRect){ 40, 40, 300, 260 }, 8, 32, COLOR_ARGB(30, 0, 0, 0));
    CHECK(gfx_shadow_template_count() == templates, "只是透明度和大小不同时不应另画模板");
    // 太小拼不开：照旧现算，两种画法应完全一样。
    compare_shadow("小矩形", 120, 120, 96, identity, (GfxRect){ 40, 40, 60, 60 }, 8, 20, COLOR_ARGB(51, 0, 0, 0));
}

/// 放掉显卡缓存以后照常能画，阴影的模板还留着。
static void test_graphics_trim(void)
{
    unsigned char *first = shadow_pixels(200, 200, 96, gfx_matrix_identity(), (GfxRect){ 50, 50, 122, 122 }, 10, 10, COLOR_ARGB(41, 0, 0, 0), true);
    int templates = gfx_shadow_template_count();
    gfx_trim();
    CHECK(gfx_shadow_template_count() == templates, "放掉显卡缓存不应丢掉阴影的模板");
    unsigned char *second = shadow_pixels(200, 200, 96, gfx_matrix_identity(), (GfxRect){ 50, 50, 122, 122 }, 10, 10, COLOR_ARGB(41, 0, 0, 0), true);
    CHECK(first && second && memcmp(first, second, (size_t)200 * 200 * 4) == 0, "放掉显卡缓存以后画出的阴影应和之前一样");
    free(first);
    free(second);
    // 写字也照常：排好的文字和字形缓存都会重新建。
    bool drawn = gfx_begin_image(200, 60, 96);
    if (drawn)
    {
        GfxTextStyle style = { GFX_FONT_TEXT, 14, 400, false, false, GFX_ALIGN_START, GFX_ALIGN_START, 0 };
        gfx_clear(COLOR_TRANSPARENT);
        gfx_text_draw(L"DropShelf 置物架", (GfxRect){ 4, 4, 196, 56 }, &style, COLOR_RGB(0, 0, 0));
        HBITMAP bitmap = gfx_end_image_bitmap();
        drawn = bitmap != NULL;
        if (bitmap) DeleteObject(bitmap);
    }
    CHECK(drawn, "放掉显卡缓存以后应能照常画");
}

static void change_window_size(AppSettings *settings, void *context)
{
    (void)context;
    settings->window_width = 900;
    settings->window_height = 680;
    settings->quick_action_layout = LAYOUT_FOUR;
}

/// 主窗口：打开、翻到设置页再翻回来、从牌边换页、对话框，以及拖动更换快速操作背后的设置改动。
static void test_main_window(void)
{
    app_update_settings(change_window_size, NULL);
    main_window_show();
    CHECK(main_window_is_open() && M->window, "主窗口没有打开");
    if (!M->window) return;
    ui_window_layout(M->window);
    CHECK(M->page == PAGE_OVERVIEW && !M->overview_scroll->hidden, "打开时应在概览页");
    CHECK(M->tiles[PAGE_GENERAL]->bounds.bottom > M->tiles[PAGE_GENERAL]->bounds.top, "概览页的卡片没有排版");

    // 点开卡片翻成设置页；直接摆到终点。
    flip_open(PAGE_GENERAL);
    CHECK(flip_morphing() || M->page == PAGE_GENERAL, "点开卡片应开始翻牌");
    flip_cancel();
    CHECK(!flip_morphing() && M->page == PAGE_GENERAL, "翻牌结束应在常规页");
    CHECK(!M->pages[PAGE_GENERAL].host->hidden && M->overview_scroll->hidden && !M->stack_layer->hidden, "常规页应显示，牌的边露出来");
    GfxRect sheet = main_sheet_area();
    CHECK(M->pages[PAGE_GENERAL].sheet->bounds.top == sheet.top && M->pages[PAGE_GENERAL].content->bounds.bottom > sheet.top + 100, "设置页没有排版");
    CHECK(!M->pages[PAGE_GENERAL].host->transformed && !M->pages[PAGE_GENERAL].host->clip, "落定后不应再有变换和裁剪");

    // 返回概览。
    flip_back();
    flip_cancel();
    CHECK(M->page == PAGE_OVERVIEW && !M->overview_scroll->hidden && M->stack_layer->hidden, "返回后应在概览页");
    CHECK(M->overview_page->opacity == 1 && M->tiles[PAGE_GENERAL]->opacity == 1, "概览页和卡片应回到静止状态");

    // 对话框：打开、Esc 关掉。
    dialogs_edit_action(NULL);
    CHECK(dialogs_open(), "添加自定义操作的对话框没有打开");
    CHECK(dialogs_escape() && !dialogs_open(), "Esc 应关掉对话框");
    dialogs_open_calibration();
    CHECK(dialogs_open(), "校准对话框没有打开");
    dialogs_escape();

    // 拖到位置上：没在位置上的操作替换，已在别的位置的两者互换。
    actions_assign_slot(0, L"Reveal");
    CHECK(wcscmp(app_settings()->quick_action_slots.items[0], L"Reveal") == 0, "应换成在资源管理器中显示");
    wchar_t *second = wcs_dup(app_settings()->quick_action_slots.items[1]);
    actions_assign_slot(1, L"Reveal");
    CHECK(wcscmp(app_settings()->quick_action_slots.items[1], L"Reveal") == 0 && wcscmp(app_settings()->quick_action_slots.items[0], second) == 0,
        "已在别的位置的操作应和原位置互换");
    free(second);

    main_window_close();
    CHECK(!main_window_is_open() && !M->window, "主窗口没有关掉");
}

// ───────────── 主窗口的设置（C# 版 TestMainWindowSettings） ─────────────

static float rect_width(GfxRect rect) { return rect.right - rect.left; }
static float rect_height(GfxRect rect) { return rect.bottom - rect.top; }

/// 元素所在的滚动区（概览页或某个设置页的）；不在滚动区里时返回 NULL。
static UiElement *scroll_of(UiElement *element)
{
    for (UiElement *item = element; item; item = item->parent)
    {
        if (item == M->overview_scroll) return item;
        for (int page = 0; page < PAGE_COUNT; page++)
            if (item == M->pages[page].scroll) return item;
    }
    return NULL;
}

/// 像用户一样点 element 横向 fraction 处（0.5 是正中）：先滚到能看见它，确认那里点到的就是它，再按下、松开。
/// 被别的东西挡住或看不见时不点，返回 false。
static bool click_element_at(UiElement *element, float fraction)
{
    if (!element) return false;
    ui_window_layout(M->window);
    UiElement *scroll = scroll_of(element);
    if (scroll)
    {
        float middle = (element->bounds.top + element->bounds.bottom) / 2;
        if (middle < scroll->bounds.top + 20 || middle > scroll->bounds.bottom - 20)
        {
            ui_scroll_set_offset(scroll, ui_scroll_offset(scroll) + middle - (scroll->bounds.top + scroll->bounds.bottom) / 2);
            ui_window_layout(M->window);
        }
    }
    float x = element->bounds.left + (element->bounds.right - element->bounds.left) * fraction;
    float y = (element->bounds.top + element->bounds.bottom) / 2;
    ui_element_to_window(element, &x, &y);
    UiElement *hit = ui_hit_test(M->root, x, y);
    if (!hit || !ui_is_within(hit, element)) return false;
    click_at(M->window, x, y);
    return true;
}

static bool click_element(UiElement *element)
{
    return click_element_at(element, 0.5f);
}

/// 主窗口所在的屏幕放得下 width × height（DIP）的窗口吗：窗口不能比屏幕大，GitHub 的构建机屏幕只有 1024×768。
static bool screen_fits(float width, float height)
{
    MONITORINFO info = { .cbSize = sizeof(info) };
    GetMonitorInfoW(MonitorFromWindow(ui_window_handle(M->window), MONITOR_DEFAULTTONEAREST), &info);
    float scale = ui_window_scale(M->window);
    bool fits = (info.rcMonitor.right - info.rcMonitor.left) / scale >= width && (info.rcMonitor.bottom - info.rcMonitor.top) / scale >= height;
    if (!fits) printf("  （屏幕放不下 %.0f×%.0f 的窗口，跳过这部分检查）\n", width, height);
    return fits;
}

static bool main_window_idle(void)
{
    return !ui_window_is_animating(M->window) && !flip_morphing();
}

/// 先处理 minimum 毫秒的消息，再等主窗口的动画都停下来。动画跟着屏幕刷新推进，
/// 刷新很慢时（锁屏、远程桌面、没有显示器的构建机）比固定的时间久：等的是结果，不是固定的时间。
static void pump_until_main_idle(int minimum)
{
    pump_for(minimum);
    pump_until_done(main_window_idle, 10000);
}

static void resize_main_window(float width, float height)
{
    RECT rect = {0};
    GetWindowRect(ui_window_handle(M->window), &rect);
    ui_window_place(M->window, rect.left, rect.top, width, height);
    pump_until_main_idle(300);
    ui_window_layout(M->window);
}

/// 设置页底下露出的哪条边是 page 那一页（按提示文字找）；没有时返回 NULL。
static UiElement *stack_edge_for(int page)
{
    for (int index = 0; index < M->stack_layer->child_count; index++)
    {
        UiElement *edge = M->stack_layer->children[index];
        const wchar_t *tooltip = ui_element_tooltip(edge);
        if (!edge->hidden && tooltip && wcscmp(tooltip, S(main_page_title(page))) == 0) return edge;
    }
    return NULL;
}

/// 牌的边恰好是除了 page 以外的三页。
static bool stack_edges_are_others(int page)
{
    for (int other = 0; other < PAGE_COUNT; other++)
        if ((stack_edge_for(other) != NULL) != (other != page)) return false;
    return true;
}

/// 一手牌以中间的牌在最上面排开，默认不选出任何一张。
static bool fan_arranged(int count)
{
    if (fan_card_count() != count) return false;
    for (int index = 0; index < count; index++)
        if (fan_card_hot(index)) return false;
    int middle = count / 2;
    return fan_card_z(middle) > fan_card_z(0) && fan_card_z(middle) > fan_card_z(count - 1);
}

static void reset_main_window_settings(AppSettings *settings, void *context)
{
    (void)context;
    settings_free(settings);
    settings_init(settings);
    settings->has_completed_first_run = true;
    settings->window_width = 900;
    settings->window_height = 680;
}

static bool summary_is(int page, const wchar_t *expected)
{
    const wchar_t *summary = pages_tile_summary(page);
    return summary && wcscmp(summary, expected) == 0;
}

static bool label_is(UiElement *label, const wchar_t *expected)
{
    const wchar_t *text = label ? ui_label_text(label) : NULL;
    return text && wcscmp(text, expected) == 0;
}

static const CustomAction *custom_action_sized(const AppSettings *settings, int size)
{
    for (int index = 0; index < settings->custom_action_count; index++)
        if (settings->custom_actions[index].kind == CUSTOM_ACTION_COMPRESS_IMAGE && settings->custom_actions[index].image_size_kb == size)
            return &settings->custom_actions[index];
    return NULL;
}

/// 主窗口的设置：左侧状态栏、卡片高度、翻到设置页后露出的牌边和从牌边换页、各项设置的控件改动后立即生效并保存、
/// 概览页卡片上的摘要、快速上手、最近关闭的一手牌、添加和编辑自定义操作、切换语言。
static void test_main_window_settings(void)
{
    language_set_mode(LANGUAGE_CHINESE);
    task_initialize();
    wchar_t *history_file = path_join(temporary_directory, L"history-main.json");
    DeleteFileW(history_file);
    shelf_history_load(history_file);
    app_update_settings(reset_main_window_settings, NULL);
    main_window_show();
    CHECK(main_window_is_open() && M->window, "主窗口没有打开");
    if (!M->window)
    {
        free(history_file);
        return;
    }
    pump_for(200);
    ui_window_layout(M->window);

    // 窗口足够宽时显示左侧状态栏，概览页的标题移到状态栏里；变窄后收起。
    CHECK(!M->side_panel_shown && M->side_panel->hidden && !M->overview_header->hidden, "窗口较窄时不应显示左侧状态栏");
    if (screen_fits(1500, 680))
    {
        resize_main_window(1500, 680);
        CHECK(M->side_panel_shown && !M->side_panel->hidden && M->overview_header->hidden, "窗口足够宽时应显示左侧状态栏，概览标题移到状态栏");
        resize_main_window(900, 680);
        CHECK(!M->side_panel_shown && M->side_panel->hidden && !M->overview_header->hidden, "窗口变窄后应收起左侧状态栏，标题回到页面");
    }

    // 窗口变高时卡片拉长填满页面，够高时换成大卡片的样式；变矮后恢复。
    float short_cards = pages_card_height();
    bool tall_fits = screen_fits(900, 1000);
    if (tall_fits)
    {
        resize_main_window(900, 1000);
        CHECK(pages_card_height() > short_cards + 40, "窗口变高后设置卡片应拉长");
        CHECK(rect_height(M->tiles[PAGE_GENERAL]->bounds) < 144 || pages_tile_is_tall(PAGE_GENERAL), "卡片够高时应切换成大卡片样式");
        resize_main_window(900, 680);
        CHECK(fabsf(pages_card_height() - short_cards) < 1, "窗口变矮后设置卡片应恢复");
        CHECK(!pages_tile_is_tall(PAGE_GENERAL) || rect_height(M->tiles[PAGE_GENERAL]->bounds) >= 132, "窗口变矮后卡片应回到单行样式");
    }

    // 点卡片翻到置物架页。
    shelf_history_clear();
    CHECK(click_element(M->tiles[PAGE_SHELF]), "概览页的置物架卡片点不到");
    pump_until_main_idle(300);
    PageView *shelf = &M->pages[PAGE_SHELF];
    CHECK(M->page == PAGE_SHELF && !shelf->host->hidden && M->overview_scroll->hidden, "点击设置卡片没有切换页面");
    CHECK(!flip_morphing() && !shelf->host->transformed && !shelf->host->clip, "翻牌结束后页面应回到静止状态：没有变换和裁剪");
    for (int page = 0; page < PAGE_COUNT; page++)
        CHECK(M->tiles[page]->opacity == 1, "翻牌结束后概览页的卡片应恢复显示");

    // 设置页底下露出其余三张牌的边；这些边是真按钮，鼠标点得到。
    GfxRect sheet = shelf->sheet->bounds;
    int visible_edges = 0;
    for (int index = 0; index < M->stack_layer->child_count; index++)
    {
        UiElement *edge = M->stack_layer->children[index];
        if (edge->hidden) continue;
        visible_edges++;
        CHECK(edge->bounds.top >= sheet.bottom - 0.5f && rect_width(edge->bounds) < rect_width(sheet), "牌的边应露在设置页的下面，比设置页窄");
        float x = (edge->bounds.left + edge->bounds.right) / 2, y = (edge->bounds.top + edge->bounds.bottom) / 2;
        CHECK(ui_hit_test(M->root, x, y) == edge, "露出的牌边必须能被鼠标命中，不能被设置页的透明区域挡住");
    }
    CHECK(!M->stack_layer->hidden && !M->stack_layer->hit_through && visible_edges == 3 && stack_edges_are_others(PAGE_SHELF),
        "设置页底下应露出其余三张牌的边");

    // 鼠标移到最下面那条边上时它多露出一截，仍离窗口底边至少 20 DIP。
    UiElement *lowest = NULL;
    for (int index = 0; index < M->stack_layer->child_count; index++)
    {
        UiElement *edge = M->stack_layer->children[index];
        if (!edge->hidden && (!lowest || edge->bounds.bottom > lowest->bounds.bottom)) lowest = edge;
    }
    if (lowest)
    {
        float scale = ui_window_scale(M->window);
        SendMessageW(ui_window_handle(M->window), WM_MOUSEMOVE, 0,
            MAKELPARAM((int)lroundf((lowest->bounds.left + lowest->bounds.right) / 2 * scale), (int)lroundf((lowest->bounds.top + lowest->bounds.bottom) / 2 * scale)));
        pump_until_main_idle(100);
        float bottom = 0;
        for (int index = 0; index < M->stack_layer->child_count; index++)
            if (!M->stack_layer->children[index]->hidden && M->stack_layer->children[index]->bounds.bottom > bottom) bottom = M->stack_layer->children[index]->bounds.bottom;
        CHECK(bottom > lowest->bounds.top && M->stage.bottom - bottom >= 20, "牌边悬停展开后距离窗口底部仍应至少留 20 DIP");
    }

    // 点牌边换页：每次都换到目标页，牌堆恰好是另外三页；各页的滚动位置保留。
    float saved_offset = fminf(80, ui_scroll_extent(shelf->scroll));
    ui_scroll_set_offset(shelf->scroll, saved_offset);
    ui_window_layout(M->window);
    const int targets[] = { PAGE_GENERAL, PAGE_APPEARANCE, PAGE_ACTIONS, PAGE_SHELF };
    for (int index = 0; index < 4; index++)
    {
        int page = targets[index];
        CHECK(click_element(stack_edge_for(page)), "露出的牌边点不到");
        pump_until_main_idle(300);
        PageView *view = &M->pages[page];
        CHECK(M->page == page && !view->host->hidden && M->overview_scroll->hidden && !view->host->transformed && !view->host->clip &&
                  !M->stack_layer->hidden && !M->stack_layer->hit_through,
            "抽牌完成后应显示目标页，页面回到静止状态");
        CHECK(stack_edges_are_others(page), "切换后牌堆应恰好包含另外三页，旧页也能再点回来");
    }
    CHECK(fabsf(ui_scroll_offset(shelf->scroll) - saved_offset) < 1, "设置页之间切换不应重置各页的滚动位置");
    CHECK(M->history_section->hidden, "没有记录时不应显示最近关闭");

    // 滚轮：光标在设置页上时滚动这一页，不能被盖在页面上的牌边那一层挡住。
    {
        UiElement *scroll = shelf->scroll;
        ui_scroll_set_offset(scroll, 0);
        ui_window_layout(M->window);
        HWND handle = ui_window_handle(M->window);
        POINT point = ui_window_to_screen(M->window, (scroll->bounds.left + scroll->bounds.right) / 2, (scroll->bounds.top + scroll->bounds.bottom) / 2);
        CHECK(ui_scroll_extent(scroll) > 48, "置物架页应比窗口高，能滚动");
        SendMessageW(handle, WM_MOUSEWHEEL, MAKEWPARAM(0, (WORD)(SHORT)-WHEEL_DELTA), MAKELPARAM(point.x, point.y));
        CHECK(ui_scroll_offset(scroll) > 0, "滚轮在设置页上没有滚动页面");
        SendMessageW(handle, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), MAKELPARAM(point.x, point.y));
        CHECK(ui_scroll_offset(scroll) == 0, "向上滚轮没有滚回顶部");
        ui_window_layout(M->window);
    }

    // 灵敏度：点滑块，说明立即更新；停下后三个字偏向稳健，并保存。
    UiElement *slider = pages_control(PAGES_SENSITIVITY_SLIDER);
    ui_window_layout(M->window);
    float slider_width = rect_width(slider->bounds);
    CHECK(slider_width > 40 && click_element_at(slider, (10 + (slider_width - 20) * 0.2f) / slider_width), "灵敏度滑块点不到");
    CHECK(ui_slider_value(slider) == 20, "点在滑块 20% 的位置应选中 20");
    int reversals = 0, distance = 0;
    shake_describe(20, &reversals, &distance);
    wchar_t *detail = str_sensitivity_detail(reversals, distance);
    CHECK(label_is(pages_control(PAGES_SENSITIVITY_DESCRIPTION), detail), "拖动滑块时说明没有立即更新");
    free(detail);
    pump_until_main_idle(400);
    float steady = pages_sensitivity_label_scale(0), balanced = pages_sensitivity_label_scale(1), responsive = pages_sensitivity_label_scale(2);
    CHECK(steady > balanced && balanced > responsive && fabsf(responsive - 0.85f) < 0.001f, "滑块偏向稳健时，稳健应最大、均衡其次、灵敏保持最小");
    // 各项选择和开关。
    ui_segmented_choose(pages_control(PAGES_TRIGGER), TRIGGER_EDGE);
    ui_window_layout(M->window);
    CHECK(pages_control(PAGES_SENSITIVITY_CARD)->hidden && !pages_control(PAGES_EDGE_DWELL_CARD)->hidden, "只用屏幕边缘时不应显示晃动灵敏度");
    ui_segmented_choose(pages_control(PAGES_EDGE_DWELL), DWELL_LONG);
    ui_segmented_choose(pages_control(PAGES_DRAG_OUT_MODE), TRANSFER_COPY);
    ui_segmented_choose(pages_control(PAGES_SHELF_SIZE), SHELF_SIZE_SMALL);
    CHECK(click_element(pages_control(PAGES_CLOSE_WHEN_EMPTY)) && !app_settings()->close_when_empty, "全部拖出后关闭的开关点不动");
    ui_segmented_choose(pages_control(PAGES_LAYOUT), 1);

    UiElement *performance = pages_control(PAGES_PERFORMANCE);
    CHECK(ui_segmented_selected(performance) == PERFORMANCE_AUTO && label_is(pages_control(PAGES_PERFORMANCE_DESCRIPTION), S(STR_PerformanceDescriptionAuto)),
        "低性能模式默认应为自动，说明随之对应");
    ui_segmented_choose(performance, PERFORMANCE_LOW);
    CHECK(label_is(pages_control(PAGES_PERFORMANCE_DESCRIPTION), S(STR_PerformanceDescriptionLow)), "切换低性能模式后说明没有更新");
    CHECK(app_settings()->performance_mode == PERFORMANCE_LOW && low_performance_enabled(), "打开低性能模式后应立即降低动画的精度");
    ui_segmented_choose(performance, PERFORMANCE_NORMAL);
    CHECK(app_settings()->performance_mode == PERFORMANCE_NORMAL && !low_performance_enabled(), "关闭低性能模式后应恢复动画的精度");

    // 外观页：点深色主题和一个强调色。
    flip_back();
    flip_cancel();
    flip_open(PAGE_APPEARANCE);
    flip_cancel();
    pump_for(100);
    CHECK(click_element(pages_control(PAGES_THEME_DARK)), "深色主题点不到");
    int accent = -1;
    for (int index = 0; index < theme_accent_preset_count(); index++)
        if (wcs_equal_ignore_case(theme_accent_preset(index), L"#038387")) accent = index;
    UiElement *swatches = pages_control(PAGES_SWATCHES);
    CHECK(accent >= 0 && accent + 1 < swatches->child_count && click_element(swatches->children[accent + 1]), "强调色点不到");

    // 概览页卡片上的摘要、快速操作页的位置和更多操作。
    actions_assign_slot(1, L"Reveal");
    CHECK(summary_is(PAGE_SHELF, L"屏幕边缘呼出 · 拖出时复制") && summary_is(PAGE_ACTIONS, L"压缩、显示位置") &&
              summary_is(PAGE_APPEARANCE, L"深色 · 自选强调色"),
        "设置卡片上的摘要不正确");
    CHECK(actions_slot_count() == 2 && actions_slot_position(0) == STR_SlotLeft && actions_slot_position(1) == STR_SlotRight, "两个操作时位置应为左、右");
    QuickActionList available = quick_actions_available(app_settings());
    bool more_complete = actions_more_count() == available.count - 2;
    for (int index = 0; index < actions_more_count(); index++)
        for (int slot = 0; slot < actions_slot_count(); slot++)
            if (wcscmp(actions_more_id(index), actions_slot_id(slot)) == 0) more_complete = false;
    quick_action_list_free(&available);
    CHECK(more_complete, "更多操作应列出其余全部操作");

    // 回到概览页，关掉快速上手。
    flip_back();
    flip_cancel();
    pump_for(100);
    UiElement *getting_started = pages_control(PAGES_GETTING_STARTED);
    CHECK(!getting_started->hidden && click_element(getting_started->children[0]->children[1]), "快速上手的关闭按钮点不到");
    pump_for(50);
    CHECK(getting_started->hidden && !app_settings()->show_getting_started, "快速上手关闭后仍显示");

    // 最近关闭的置物架：全部显示在牌堆中，以中间的牌在最上面排开。
    wchar_t *history_folder = test_folder(L"main-history");
    for (int index = 0; index < 5; index++)
    {
        wchar_t name[16];
        swprintf(name, 16, L"%d.txt", index);
        wchar_t *path = test_file(history_folder, name, "x");
        ShelfItem *item = shelf_item_new(ITEM_FILE, path, NULL, false);
        shelf_history_record(&item, 1);
        shelf_item_release(item);
        free(path);
    }
    pump_for(100);
    ui_window_layout(M->window);
    CHECK(!M->history_section->hidden && fan_card_count() == 5, "最近关闭应全部显示在牌堆中");
    CHECK(fan_card_thumbs(0) == 1, "历史卡片的缩略图堆叠不正确");
    CHECK(fan_arranged(5), "牌堆应以中间的牌在最上面排开，默认不选出任何一张");
    // 调整顺序时卡片沿用原来的，只换位置。
    const ShelfHistoryEntry *first = fan_card_entry(0);
    const void *first_card = fan_card_identity(0);
    shelf_history_move((ShelfHistoryEntry *)first, 3);
    pump_for(50);
    CHECK(fan_card_entry(3) == first && fan_card_identity(3) == first_card, "调整顺序后卡片应沿用原来的，不重新生成");
    CHECK(fan_arranged(5), "调整顺序后牌堆应以中间的牌在最上面排开");
    shelf_history_move((ShelfHistoryEntry *)first, 0);
    pump_for(50);
    // 切换语言会重新生成同样数量的牌，也要重新排开。
    ui_segmented_choose(pages_control(PAGES_LANGUAGE), LANGUAGE_ENGLISH);
    pump_for(50);
    ui_segmented_choose(pages_control(PAGES_LANGUAGE), LANGUAGE_CHINESE);
    pump_for(50);
    CHECK(fan_arranged(5), "切换语言后牌堆应以中间的牌在最上面排开");
    // 牌堆贴着窗口底边；有牌堆时窗口变高，卡片也拉长。
    const float heights[] = { 640, 1000 };
    for (int index = 0; index < (tall_fits ? 2 : 1); index++)
    {
        resize_main_window(900, heights[index]);
        if (ui_scroll_extent(M->overview_scroll) > 0.5f) continue;
        CHECK(fabsf(M->fan->bounds.bottom - M->overview_scroll->bounds.bottom) < 1, "牌堆应贴着窗口底边");
    }
    CHECK(!tall_fits || pages_card_height() > short_cards + 40, "有牌堆时窗口变高后设置卡片也应拉长");
    resize_main_window(900, 680);
    shelf_history_clear();
    free(history_folder);

    // 添加自定义操作：没有选文件夹、大小无效时不保存；压缩到 200 KB 的名称默认是“≤ 200 KB”。
    dialogs_edit_action(NULL);
    pump_for(300);
    CHECK(dialogs_open() && !M->dialogs->hidden, "自定义操作对话框没有打开");
    UiElement *save = dialogs_part(DIALOG_PART_EDITOR_SAVE);
    CHECK(click_element(save) && dialogs_open() && !dialogs_part(DIALOG_PART_EDITOR_ERROR)->hidden, "没有选择文件夹时不应保存");
    ui_segmented_choose(dialogs_part(DIALOG_PART_EDITOR_KIND), 1);
    ui_textbox_type(dialogs_part(DIALOG_PART_EDITOR_SIZE), L"abc");
    CHECK(click_element(save) && dialogs_open(), "无效大小不应保存");
    ui_textbox_type(dialogs_part(DIALOG_PART_EDITOR_SIZE), L"200");
    const wchar_t *placeholder = ui_textbox_placeholder(dialogs_part(DIALOG_PART_EDITOR_NAME));
    CHECK(placeholder && wcscmp(placeholder, L"≤ 200 KB") == 0, "压缩到 200 KB 的默认名称不对");
    CHECK(click_element(save) && !dialogs_open(), "自定义操作没有保存");

    // 改过的设置都已保存并生效。
    AppSettings saved;
    settings_load(&saved);
    CHECK(saved.shake_sensitivity == 20, "灵敏度没有在停下后保存");
    CHECK(saved.auto_open_trigger == TRIGGER_EDGE && saved.edge_dwell == DWELL_LONG, "呼出方式没有保存");
    CHECK(saved.drag_out_mode == TRANSFER_COPY, "拖出方式没有保存");
    CHECK(saved.quick_shelf_size == SHELF_SIZE_SMALL, "尺寸没有立即保存");
    CHECK(saved.theme == THEME_DARK && theme_is_dark(), "主题没有保存或生效");
    CHECK(!saved.close_when_empty && !saved.show_getting_started, "开关设置没有保存");
    COLORREF expected_accent = 0;
    Color current_accent = theme_accent().base;
    CHECK(saved.accent_color && wcs_equal_ignore_case(saved.accent_color, L"#038387") && accent_color_parse(L"#038387", &expected_accent) &&
              current_accent.r == GetRValue(expected_accent) && current_accent.g == GetGValue(expected_accent) && current_accent.b == GetBValue(expected_accent),
        "强调色没有保存或生效");
    CHECK(saved.quick_action_layout == LAYOUT_TWO && actions_slot_count() == 2, "按钮数量没有保存");
    const CustomAction *created_action = custom_action_sized(&saved, 200);
    wchar_t *created = created_action ? wcs_dup(created_action->id) : NULL;
    settings_free(&saved);
    CHECK(created != NULL, "新建的自定义操作没有保存");
    if (created)
    {
        bool listed = false;
        for (int index = 0; index < actions_more_count(); index++)
            if (wcscmp(actions_more_id(index), created) == 0) listed = true;
        CHECK(listed, "新建的操作应出现在更多操作中");
        actions_assign_slot(0, created);
        settings_load(&saved);
        CHECK(saved.quick_action_slots.count >= 2 && wcscmp(saved.quick_action_slots.items[0], created) == 0 &&
                  wcscmp(saved.quick_action_slots.items[1], L"Reveal") == 0,
            "拖到位置上的操作没有保存");
        settings_free(&saved);

        // 编辑名称、删除。
        dialogs_edit_action(custom_action_sized(app_settings(), 200));
        pump_for(300);
        ui_textbox_type(dialogs_part(DIALOG_PART_EDITOR_NAME), L"压缩证件照");
        CHECK(click_element(dialogs_part(DIALOG_PART_EDITOR_SAVE)) && !dialogs_open(), "编辑自定义操作的保存点不到");
        settings_load(&saved);
        const CustomAction *edited = custom_action_sized(&saved, 200);
        CHECK(edited && wcscmp(edited->id, created) == 0 && edited->name && wcscmp(edited->name, L"压缩证件照") == 0, "编辑自定义操作没有保存");
        settings_free(&saved);
        actions_delete_custom(created);
        settings_load(&saved);
        bool gone = true;
        for (int index = 0; index < saved.custom_action_count; index++)
            if (wcscmp(saved.custom_actions[index].id, created) == 0) gone = false;
        for (int index = 0; index < saved.quick_action_slots.count; index++)
            if (wcscmp(saved.quick_action_slots.items[index], created) == 0) gone = false;
        CHECK(gone, "删除的自定义操作仍在置物架上");
        settings_free(&saved);
        free(created);
    }

    // 切换语言：保存并生效，主窗口的文字跟着变。
    CHECK(wcscmp(S(main_page_title(PAGE_GENERAL)), L"常规") == 0, "设置卡片文字不正确");
    ui_segmented_choose(pages_control(PAGES_LANGUAGE), LANGUAGE_ENGLISH);
    pump_for(50);
    settings_load(&saved);
    CHECK(saved.language == LANGUAGE_ENGLISH && !language_is_chinese(), "语言设置没有保存或生效");
    settings_free(&saved);
    const wchar_t *general = pages_tile_summary(PAGE_GENERAL);
    CHECK(wcscmp(S(main_page_title(PAGE_GENERAL)), L"General") == 0 && general && wcsncmp(general, L"English", 7) == 0, "切换语言后主窗口文字没有更新");
    ui_segmented_choose(pages_control(PAGES_LANGUAGE), LANGUAGE_CHINESE);
    pump_for(50);
    CHECK(wcscmp(S(main_page_title(PAGE_GENERAL)), L"常规") == 0 && language_is_chinese(), "切换回中文后主窗口文字没有更新");

    main_window_close();
    app_update_settings(reset_main_window_settings, NULL);
    language_set_mode(LANGUAGE_CHINESE);
    settings_flush();
    shelf_history_flush();
    DeleteFileW(history_file);
    free(history_file);
}

static void forget_watch_asked(AppSettings *settings, void *context)
{
    (void)context;
    settings->has_asked_folder_watch = false;
}

static void test_about_and_onboarding(void)
{
    about_window_show(NULL);
    CHECK(about_window_is_open(), "关于窗口没有打开");
    about_window_close();
    CHECK(!about_window_is_open(), "关于窗口没有关掉");

    // C# 版 TestOnboardingSteps：按钮文字、最后一步、说明里写着现在的快捷键、切换语言后文字跟着变。
    language_set_mode(LANGUAGE_CHINESE);
    onboarding_window_show();
    CHECK(onboarding_window_is_open() && onboarding_window_step() == 0, "引导应从第 1 步开始");
    const wchar_t *next = onboarding_window_next_text();
    CHECK(next && wcscmp(next, L"下一步") == 0, "教程起始状态不正确");
    for (int step = 1; step < 4; step++) onboarding_window_go(step);
    next = onboarding_window_next_text();
    CHECK(onboarding_window_step() == 3 && next && wcscmp(next, L"下一步") == 0, "快速操作这一步后面还有观察文件夹");
    const wchar_t *hotkey = app_settings()->hotkey;
    const wchar_t *body = onboarding_window_body_text();
    CHECK(hotkey && *hotkey && body && wcsstr(body, hotkey), "教程没有说明当前的快捷键");
    language_set_mode(LANGUAGE_ENGLISH);
    next = onboarding_window_next_text();
    const wchar_t *title = onboarding_window_title_text();
    CHECK(next && wcscmp(next, L"Next") == 0 && title && wcscmp(title, L"Quick actions and shortcut") == 0, "切换语言后教程文字没有更新");
    language_set_mode(LANGUAGE_CHINESE);
    // 最后一步问要不要观察文件夹：没有“下一步”，换成“不用了”和“添加文件夹”。
    onboarding_window_go(9);
    title = onboarding_window_title_text();
    const wchar_t *add = onboarding_window_add_folder_text();
    CHECK(onboarding_window_step() == 4 && !onboarding_window_next_text() && add && wcscmp(add, L"添加文件夹") == 0 &&
          title && wcscmp(title, L"新文件，就在手边") == 0, "超出时应停在问观察文件夹的最后一步");
    onboarding_window_close();
    CHECK(!onboarding_window_is_open() && app_settings()->has_completed_first_run && app_settings()->has_asked_folder_watch,
        "关掉引导后应记下已完成第一次运行，也算问过观察文件夹");

    // 升级来的老用户只问观察文件夹这一步。
    app_update_settings(forget_watch_asked, NULL);
    onboarding_window_show_watch();
    add = onboarding_window_add_folder_text();
    CHECK(onboarding_window_is_open() && onboarding_window_is_watch_only() && onboarding_window_step() == 4 && add, "单独询问时应直接显示观察文件夹这一步");
    onboarding_window_go(0);
    CHECK(onboarding_window_step() == 4, "单独询问时不能翻到引导的其他步");
    onboarding_window_close();
    CHECK(app_settings()->has_asked_folder_watch, "单独询问后应记下已经问过");
}

/// 校准对话框：太轻的示范不算，三次示范后给出建议，采用后灵敏度随之改变；重来时从头开始。
static void test_calibration_dialog(void)
{
    language_set_mode(LANGUAGE_CHINESE);
    app_update_settings(reset_main_window_settings, NULL);
    main_window_show();
    CHECK(main_window_is_open() && M->window, "主窗口没有打开");
    if (!M->window) return;
    dialogs_open_calibration();
    pump_for(300);
    CHECK(dialogs_open(), "校准对话框没有打开");
    UiElement *status = dialogs_part(DIALOG_PART_CALIBRATION_STATUS), *apply = dialogs_part(DIALOG_PART_CALIBRATION_APPLY);
    CHECK(label_is(status, S(STR_CalibrationStart)) && apply->disabled && dialogs_calibration_done() == 0, "校准的起始状态不正确");

    static ShakeSample demo[1200];
    calibration_demo(demo, 3);
    CHECK(!dialogs_calibration_submit(demo, 1200) && dialogs_calibration_done() == 0 && label_is(status, S(STR_CalibrationTooWeak)),
        "太轻的示范不应记下，并提示晃得更明显些");
    calibration_demo(demo, 40);
    int normal = shake_calibration_steadiest_level(demo, 1200);
    CHECK(dialogs_calibration_submit(demo, 1200), "正常的示范没有记下");
    calibration_demo(demo, 60);
    CHECK(dialogs_calibration_submit(demo, 1200), "用力的示范没有记下");
    wchar_t *progress = str_calibration_progress(2, CALIBRATION_DEMONSTRATIONS - 2);
    CHECK(label_is(status, progress) && apply->disabled, "两次示范后的进度不对");
    free(progress);
    calibration_demo(demo, 40);
    CHECK(dialogs_calibration_submit(demo, 1200) && dialogs_calibration_done() == CALIBRATION_DEMONSTRATIONS && !apply->disabled, "三次示范后应能采用");
    CHECK(!dialogs_calibration_submit(demo, 1200) && dialogs_calibration_done() == CALIBRATION_DEMONSTRATIONS, "示范够了以后不应再记");
    int recommended = dialogs_calibration_recommended();
    CHECK(recommended == shake_sensitivity_clamp(normal + CALIBRATION_MARGIN), "建议的灵敏度应是最稳健的那次再加一点余量");
    CHECK(click_element(apply) && !dialogs_open() && app_settings()->shake_sensitivity == recommended && ui_slider_value(pages_control(PAGES_SENSITIVITY_SLIDER)) == recommended,
        "采用后灵敏度没有改成建议的值");

    // 再打开时从头开始；示范一次后点重来回到起点。
    dialogs_open_calibration();
    pump_for(300);
    CHECK(dialogs_open() && dialogs_calibration_done() == 0, "再次打开校准应从头开始");
    CHECK(dialogs_calibration_submit(demo, 1200) && dialogs_calibration_done() == 1, "示范没有记下");
    CHECK(click_element(dialogs_part(DIALOG_PART_CALIBRATION_RETRY)) && dialogs_calibration_done() == 0 && label_is(status, S(STR_CalibrationStart)),
        "重来后应回到起点");
    CHECK(dialogs_escape() && !dialogs_open(), "Esc 应关掉校准对话框");

    main_window_close();
    app_update_settings(reset_main_window_settings, NULL);
    settings_flush();
}
