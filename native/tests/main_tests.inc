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
    text = str_shelf_summary(TRIGGER_EDGE, TRANSFER_COPY);
    CHECK(wcscmp(text, L"Pause at edge to open · Drag out to copy") == 0, "英文的置物架摘要不对");
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

static void test_about_and_onboarding(void)
{
    about_window_show(NULL);
    CHECK(about_window_is_open(), "关于窗口没有打开");
    about_window_close();
    CHECK(!about_window_is_open(), "关于窗口没有关掉");

    onboarding_window_show();
    CHECK(onboarding_window_is_open() && onboarding_window_step() == 0, "引导应从第 1 步开始");
    onboarding_window_go(3);
    CHECK(onboarding_window_step() == 3, "应能翻到第 4 步");
    onboarding_window_go(9);
    CHECK(onboarding_window_step() == 3, "超出时停在最后一步");
    onboarding_window_close();
    CHECK(!onboarding_window_is_open() && app_settings()->has_completed_first_run, "关掉引导后应记下已完成第一次运行");
}
