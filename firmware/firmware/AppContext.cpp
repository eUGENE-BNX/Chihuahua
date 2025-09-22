#include "AppContext.h"

FirmwareState& app() {
  static FirmwareState state;
  return state;
}