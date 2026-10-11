#pragma once
#include <gui/view.h>
/* Lightweight message keyboard; Done returns to the existing send preview. */
View* chat_keyboard_alloc(
    char* text,
    size_t limit,
    const char* title,
    void (*done)(void*),
    void* context);
void chat_keyboard_free(View* view);
