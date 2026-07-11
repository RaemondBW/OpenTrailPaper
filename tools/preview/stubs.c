// Host stubs for epdiy's hardware layer — enough to link epdiy.c's
// drawing/rotation code without an ESP32 attached.
#include <stdint.h>
#include <stddef.h>

typedef struct EpdBoardDefinition EpdBoardDefinition;

static const EpdBoardDefinition* current_board = NULL;

void epd_set_board(const EpdBoardDefinition* board) { current_board = board; }
const EpdBoardDefinition* epd_current_board(void) { return current_board; }

void epd_renderer_init(int options) { (void)options; }
void epd_renderer_deinit(void) {}

typedef struct { int x, y, width, height; } EpdRectStub;
void epd_push_pixels(EpdRectStub area, short time, int color) {
    (void)area; (void)time; (void)color;
}

void* epd_ctrl_state(void) { return NULL; }

void epd_clear_area(EpdRectStub area) { (void)area; }
int epd_draw_base(EpdRectStub area, const uint8_t* data, EpdRectStub crop,
                  int mode, int temperature, const void* drawn_lines,
                  const void* waveform) {
    (void)area; (void)data; (void)crop; (void)mode; (void)temperature;
    (void)drawn_lines; (void)waveform;
    return 0;
}
void epd_lcd_set_pixel_clock_MHz(int frequency) { (void)frequency; }
