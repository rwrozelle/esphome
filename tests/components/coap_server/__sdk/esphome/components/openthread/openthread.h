#pragma once
// Stand-in for the openthread component's InstanceLock: acquire() does nothing and get_instance()
// returns one fixed instance.

#include "openthread/coap.h"

namespace esphome::openthread {

class InstanceLock {
 public:
  static InstanceLock acquire() { return InstanceLock{}; }

  otInstance *get_instance() {
    static otInstance dummy{};
    return &dummy;
  }
};

}  // namespace esphome::openthread
