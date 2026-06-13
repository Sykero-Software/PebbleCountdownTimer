#include <pebble.h>

static Window *s_window;

static void init(void) {
  s_window = window_create();
  window_stack_push(s_window, true);
}

static void deinit(void) {
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
