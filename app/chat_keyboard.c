#include "chat_keyboard.h"
#include <furi.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char* text;
    size_t limit;
    const char* title;
    unsigned row, column;
    bool upper, symbols;
    void (*done)(void*);
    void* context;
} ChatKeys;
static const char* letters[] = {"qwertyuiop", "asdfghjkl'", "zxcvbnm.,?"};
static const char* symbols[] = {"1234567890", "!?@#$%&*()", "-+=/:;_\"[]"};
static const unsigned bounds[] = {0, 28, 48, 78, 98, 128};
static void draw(Canvas* c, void* context) {
    ChatKeys* k = context;
    canvas_clear(c);
    canvas_set_font(c, FontSecondary);
    char title[32];
    size_t n = strlen(k->text);
    snprintf(title, sizeof(title), "%s %u/%u", k->title, (unsigned)n, (unsigned)k->limit);
    canvas_draw_str(c, 0, 8, title);
    const char* visible = k->text;
    while(*visible && canvas_string_width(c, visible) > 116)
        ++visible;
    canvas_draw_str(c, 2, 19, visible);
    unsigned end = canvas_string_width(c, visible) + 3;
    canvas_draw_line(c, end, 12, end, 20);
    canvas_draw_line(c, 0, 21, 127, 21);
    for(unsigned r = 0; r < 4; ++r) {
        for(unsigned col = 0; col < (r == 3 ? 5U : 10U); ++col) {
            unsigned x = r == 3 ? bounds[col] : col * 128 / 10;
            unsigned width = r == 3 ? bounds[col + 1] - x : (col + 1) * 128 / 10 - x;
            unsigned y = 23 + r * 10;
            char label[2] = {0};
            const char* text = label;
            if(r == 3) {
                const char* controls[] = {
                    k->upper ? "ABC" : "abc", k->symbols ? "abc" : "123", "Space", "Del", "Done"};
                text = controls[col];
            } else {
                label[0] = (k->symbols ? symbols : letters)[r][col];
                if(k->upper && label[0] >= 'a' && label[0] <= 'z') label[0] -= 'a' - 'A';
            }
            if(k->row == r && k->column == col) {
                canvas_draw_box(c, x, y, width - 1, 10);
                canvas_set_color(c, ColorWhite);
            }
            unsigned tw = canvas_string_width(c, text);
            canvas_draw_str(c, x + (width > tw ? (width - tw) / 2 : 0), y + 8, text);
            canvas_set_color(c, ColorBlack);
        }
    }
}
static bool input(InputEvent* e, void* context) {
    View* v = context;
    if(e->key == InputKeyBack && e->type == InputTypeShort) return false;
    if(e->type != InputTypeShort && e->type != InputTypeRepeat && e->type != InputTypeLong)
        return false;
    bool finish = false;
    void (*done)(void*) = NULL;
    void* callback_context = NULL;
    with_view_model(
        v,
        ChatKeys * k,
        {
            unsigned columns = k->row == 3 ? 5 : 10;
            size_t n = strlen(k->text);
            if(e->key == InputKeyLeft)
                k->column = (k->column + columns - 1) % columns;
            else if(e->key == InputKeyRight)
                k->column = (k->column + 1) % columns;
            else if(e->key == InputKeyUp) {
                if(k->row == 3) k->column *= 2;
                k->row = (k->row + 3) % 4;
                if(k->row == 3) k->column /= 2;
            } else if(e->key == InputKeyDown) {
                if(k->row == 3) k->column *= 2;
                k->row = (k->row + 1) % 4;
                if(k->row == 3) k->column /= 2;
            } else if(e->key == InputKeyBack) {
                if(n) k->text[n - 1] = 0;
            } else if(e->key == InputKeyOk && e->type != InputTypeRepeat) {
                char ch = 0;
                if(k->row < 3) {
                    ch = (k->symbols ? symbols : letters)[k->row][k->column];
                    if((k->upper ^ (e->type == InputTypeLong)) && ch >= 'a' && ch <= 'z')
                        ch -= 'a' - 'A';
                    k->upper = false;
                } else if(e->type == InputTypeShort) {
                    if(k->column == 0)
                        k->upper = !k->upper;
                    else if(k->column == 1)
                        k->symbols = !k->symbols;
                    else if(k->column == 2)
                        ch = ' ';
                    else if(k->column == 3) {
                        if(n) k->text[n - 1] = 0;
                    } else if(n) {
                        finish = true;
                        done = k->done;
                        callback_context = k->context;
                    }
                }
                if(ch && n < k->limit) {
                    k->text[n] = ch;
                    k->text[n + 1] = 0;
                }
            }
        },
        true);
    if(finish) done(callback_context);
    return true;
}
View* chat_keyboard_alloc(
    char* text,
    size_t limit,
    const char* title,
    void (*done)(void*),
    void* context) {
    View* v = view_alloc();
    view_allocate_model(v, ViewModelTypeLocking, sizeof(ChatKeys));
    view_set_context(v, v);
    view_set_draw_callback(v, draw);
    view_set_input_callback(v, input);
    with_view_model(
        v,
        ChatKeys * k,
        {
            k->row = 0;
            k->column = 0;
            k->symbols = false;
            k->text = text;
            k->limit = limit;
            k->title = title;
            k->upper = !text[0];
            k->done = done;
            k->context = context;
        },
        true);
    return v;
}
void chat_keyboard_free(View* view) {
    view_free(view);
}
