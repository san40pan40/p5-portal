#pragma once
// Ring buffer of the last N entries: a new entry overwrites the oldest one.
// at(0) is the newest, at(count()-1) the oldest.
template <class T, int N>
class Ring {
public:
  // Slot for a new entry (fill it in place, so strings can reuse their memory)
  T &next() {
    T &slot = _d[_head];
    _head = (_head + 1) % N;
    if (_count < N) _count++;
    return slot;
  }
  void push(const T &v) { next() = v; }
  int count() const { return _count; }
  const T &at(int i) const { return _d[(_head - 1 - i + N) % N]; }

private:
  T   _d[N];
  int _head = 0, _count = 0;
};
