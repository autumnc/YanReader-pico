#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace crossstore::reading {

// ReadingStatsStore keeps these vectors sorted. Read in place; never copy a
// user's complete history into the Lua heap or allocate a native result vector.
template <typename Days>
uint64_t millisecondsOnDay(const Days& days, uint32_t ordinal) {
  const auto found = std::lower_bound(days.begin(), days.end(), ordinal,
                                      [](const auto& day, uint32_t value) { return day.dayOrdinal < value; });
  return found != days.end() && found->dayOrdinal == ordinal ? found->readingMs : 0;
}

struct BookDayMatch {
  size_t book = 0;  // One-based, 0 when this bounded window has no match.
  size_t next = 0;  // One-based continuation, 0 at the end in either direction.
  uint64_t milliseconds = 0;
};

constexpr size_t MAX_BOOK_SCAN = 32;

template <typename Books>
BookDayMatch nextBookOnDay(const Books& books, uint32_t ordinal, size_t cursor, bool reverse) {
  BookDayMatch result;
  if (cursor < 1 || cursor > books.size()) return result;
  for (size_t checked = 0; checked < MAX_BOOK_SCAN; ++checked) {
    const auto elapsed = millisecondsOnDay(books[cursor - 1].readingDays, ordinal);
    result.next = reverse ? cursor - 1 : (cursor == books.size() ? 0 : cursor + 1);
    if (elapsed) {
      result.book = cursor;
      result.milliseconds = elapsed;
      return result;
    }
    if (!result.next) return result;
    cursor = result.next;
  }
  return result;
}

// Strict decimal ordinal, optionally '-' to scan the book list backwards.
// This is a date token, never a path or timestamp interpreted through the SD HAL.
inline bool parseDay(const char* value, uint32_t maximum, uint32_t& ordinal, bool& reverse) {
  reverse = *value == '-';
  if (reverse) ++value;
  if (!*value) return false;
  uint32_t parsed = 0;
  for (; *value; ++value) {
    if (*value < '0' || *value > '9') return false;
    const auto digit = static_cast<uint32_t>(*value - '0');
    if (digit > maximum || parsed > (maximum - digit) / 10) return false;
    parsed = parsed * 10 + digit;
  }
  if (!parsed || parsed > maximum) return false;
  ordinal = parsed;
  return true;
}

inline bool parseMonth(const char* value, int& year, unsigned& month) {
  if (std::strlen(value) != 7 || value[4] != '-') return false;
  for (size_t i = 0; i < 7; ++i) {
    if (i != 4 && (value[i] < '0' || value[i] > '9')) return false;
  }
  year = (value[0] - '0') * 1000 + (value[1] - '0') * 100 + (value[2] - '0') * 10 + value[3] - '0';
  month = (value[5] - '0') * 10 + value[6] - '0';
  return year >= 1970 && year <= 2106 && month >= 1 && month <= 12;
}

}  // namespace crossstore::reading
