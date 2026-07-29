#include "ga_dsp.h"

namespace ga {

const SineTable& sineTable() {
  static SineTable table;
  return table;
}

}  // namespace ga
