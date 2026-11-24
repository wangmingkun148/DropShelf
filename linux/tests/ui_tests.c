// 元素树的冒烟测试：排版、命中测试和按钮点击都不依赖窗口系统，可以在 ctest 里跑。
#include "base.h"
#include "settings.h"
#include "theme.h"
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>

static int failures;
static int clicks;

static void check(bool condition, const char *message)
{
    if (condition) return;
    fprintf(stderr, "FAIL: %s\n", message);
    failures++;
}

static void button_clicked(void *context)
{
    (void)context;
    clicks++;
}

static void click(UiElement *element, float window_x, float window_y)
{
    float x = window_x, y = window_y;
    ui_element_to_local(element, &x, &y);
    UiMouseEvent down = { .kind = UI_MOUSE_DOWN, .x = x, .y = y };
    UiMouseEvent up = { .kind = UI_MOUSE_UP, .x = x, .y = y, .inside = true };
    element->type->mouse(element, &down);
    element->type->mouse(element, &up);
}

int main(void)
{
    theme_initialize(THEME_SYSTEM, NULL);

    UiElement *root = ui_stack(false, 8);
    ui_stack_set_padding(root, 20, 20, 20, 20);
    UiElement *button = ui_button(UI_BUTTON_STANDARD, UI_LIT(L"按钮"), button_clicked, NULL);
    ui_add(root, button);

    GfxSize desired = ui_measure(root, 320);
    check(desired.width > 0 && desired.height > 0, "stack measures a positive size");
    check(button->desired.width > 0, "button measures its text");

    ui_arrange(root, (GfxRect){ 0, 0, 320, 240 });
    check(button->bounds.right > button->bounds.left && button->bounds.bottom > button->bounds.top, "button is arranged");
    check(button->bounds.left >= 20, "stack padding moves the button in");
    check(button->bounds.right <= 320 - 20, "button stays inside the padding");
    check(button->bounds.bottom <= 240, "button stays inside the window");

    float x = (button->bounds.left + button->bounds.right) / 2;
    float y = (button->bounds.top + button->bounds.bottom) / 2;
    check(ui_hit_test(root, x, y) == button, "hit test finds the button");
    check(ui_hit_test(root, 319, 239) == NULL, "hit test misses the empty corner");

    click(button, x, y);
    check(clicks == 1, "clicking the button runs the handler");

    UiMouseEvent up_outside = { .kind = UI_MOUSE_UP, .x = -1, .y = -1, .inside = false };
    button->type->mouse(button, &up_outside);
    check(clicks == 1, "releasing outside does not click");

    ui_element_destroy(root);

    if (failures == 0) printf("ui_tests: 全部通过\n");
    return failures == 0 ? 0 : 1;
}