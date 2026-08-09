#include <gg/gg.h>

int main(void) {
  gg_operation_options options = GG_OPERATION_OPTIONS_INIT;
  return gg_operation_options_init(&options, GG_OPTIONS_VERSION) == GIT_OK
             ? 0
             : 1;
}
