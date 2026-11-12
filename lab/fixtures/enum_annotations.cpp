#include "enum_annotations.h"

ENUM_META(Blueprint)
enum class [[nodiscard]] ENUM_API Mode : unsigned char {
  Idle ENUM_META(Label = "Idle"),
  Legacy [[deprecated("use Idle")]] ENUM_META(Hidden) = 0,
  Shifted = 1 << 1 ENUM_META(Label = "Shifted"),
  Alias = Shifted ENUM_META(Label = "Alias"),
  Grouped = (1 << 2) ENUM_META(Label = "Grouped")
};

enum ENUM_DEPRECATED(5.2, "use Mode") OldMode {
  First ENUM_META(Hidden)
};

int read_mode(Mode mode) { return static_cast<int>(mode); }
