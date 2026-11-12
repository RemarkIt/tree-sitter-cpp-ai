constexpr int make_flag(int bit) { return 1 << bit; }

enum Flags {
  CastCall = (Flags) make_flag(1),
  Plain = 0
};

enum Flags selected { CastCall };

struct Holder {
  enum Flags member { Plain };
};

int read_selected() { return selected; }
