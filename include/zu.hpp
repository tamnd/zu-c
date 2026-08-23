/** @file zu.hpp
 *
 * The C++ wrapper over libzu.
 *
 * Header only, and additive. It includes zu.h and calls nothing else, so
 * a translation unit that already speaks the C API keeps speaking it and
 * a project that wants only this never sees a zu_ identifier.
 *
 * Two things it is for. The first is lifetime: every handle in zu.h is a
 * pointer with a matching free, and every one of them is a class here
 * that closes on the way out of scope, in the right order, on the error
 * path as well as the happy one. The second is the error model. A
 * failure carries a GQLSTATUS condition, a severity, a position and
 * whether a retry could work, and none of that survives being flattened
 * into a return code, so it arrives here as an object.
 *
 * There are two ways to read that object and both are here. The plain
 * spelling throws:
 *
 *   auto conn = zu::Connection::memory();
 *   for (auto row : conn.query("RETURN 1 AS one"))
 *       std::println("{}", row.get<int64_t>("one"));
 *
 * and every call that throws has a try_ twin returning
 * std::expected<T, zu::Error>, for a caller who would rather branch than
 * catch, or who builds without exceptions:
 *
 *   auto conn = zu::Connection::try_memory();
 *   if (!conn) return std::println("{}", conn.error().message()), 1;
 *
 * The twins are not a second implementation. Every operation is written
 * once and both spellings are one line over it, so they cannot come to
 * disagree about what a failure means. The try_ half needs
 * std::expected, which is C++23; the throwing half is C++20. Compile
 * this as C++20 and the twins are simply not declared, which is what
 * ZU_HAS_EXPECTED below reports.
 *
 * Nothing here copies a row. A column comes back as a std::span over the
 * engine's own buffer and a string as a std::string_view into the
 * result's own bytes, which is the whole reason the C API hands those
 * out. What follows from that is the rule zu.h states and this cannot
 * repeat too often: those views are good until the Result is destroyed,
 * and a span kept past it is a span into freed memory. Where a copy is
 * wanted, ask for std::string rather than std::string_view and the
 * wrapper makes one.
 *
 * A Result is a range of rows, so the standard algorithms and the views
 * work on it. It is a random access range rather than an input one,
 * because the rows are all in memory before the first one is read and
 * pretending otherwise would cost every caller who wanted the last row.
 */
#ifndef ZU_HPP
#define ZU_HPP

#include <zu.h>

#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

/* First, and not in alphabetical order with the rest, because the two
 * feature tests below read macros that only exist once something has
 * defined them and <version> is the header whose whole job is defining
 * them.
 *
 * Without it this was reading __cpp_lib_expected before anything had
 * declared it, so an undefined macro evaluated to nought and
 * ZU_HAS_EXPECTED came out 0 on GCC 13 at -std=c++23, where
 * std::expected has been there since GCC 12. Every try_ call in this
 * header then went undeclared, on a toolchain that has them, and the
 * suite for that half compiled away to a placeholder rather than
 * failing, so nothing said so.
 *
 * Worse than missing: it depended on include order. A translation unit
 * that had already included <expected> or <version> before this file
 * got the try_ half and one that had not did not, which is two
 * different APIs out of one header in one build. */
#include <version>

#if __cpp_lib_expected >= 202202L
#include <expected>
/** 1 when this toolchain has std::expected and the try_ half of the API
 * is declared, 0 when it does not and only the throwing half is. Always
 * defined, so it is read with \#if rather than \#ifdef, and a program
 * that offers both spellings switches on it. */
#define ZU_HAS_EXPECTED 1
#else
#define ZU_HAS_EXPECTED 0
#endif

#if defined(__cpp_lib_format) && __cpp_lib_format >= 201907L
#include <format>
/** 1 when this toolchain has std::format and the formatters at the foot
 * of this header are defined, 0 when it does not. to_string is there
 * either way and is what the formatters are written over. */
#define ZU_HAS_FORMAT 1
#else
#define ZU_HAS_FORMAT 0
#endif

namespace zu {

/* ---- what a call answered ---- */

/** The zu_status values, as an enum class so that a status cannot be
 * compared with a row count by accident. The numbers are the ABI's and
 * are fixed. */
enum class Status : int {
  ok = ZU_OK,
  done = ZU_DONE,
  error = ZU_ERROR,
  misuse = ZU_MISUSE,
  misuse_concurrent = ZU_MISUSE_CONCURRENT,
  misuse_closed = ZU_MISUSE_CLOSED,
  interrupted = ZU_INTERRUPTED,
  conflict = ZU_CONFLICT,
  corrupt = ZU_CORRUPT,
  unsupported = ZU_UNSUPPORTED,
  io = ZU_IO,
};

/** How bad a diagnostic is. A warning rides along with a result; an
 * exception replaces one. */
enum class Severity : int {
  success = ZU_SEVERITY_SUCCESS,
  no_data = ZU_SEVERITY_NO_DATA,
  warning = ZU_SEVERITY_WARNING,
  informational = ZU_SEVERITY_INFORMATIONAL,
  exception = ZU_SEVERITY_EXCEPTION,
};

/** What a cell holds. */
enum class Type : int {
  null = ZU_TYPE_NULL,
  boolean = ZU_TYPE_BOOL,
  integer = ZU_TYPE_INT,
  floating = ZU_TYPE_FLOAT,
  string = ZU_TYPE_STR,
  node = ZU_TYPE_NODE,
  rel = ZU_TYPE_REL,
  list = ZU_TYPE_LIST,
  path = ZU_TYPE_PATH,
  temporal = ZU_TYPE_TEMPORAL,
  record = ZU_TYPE_RECORD,
  graph = ZU_TYPE_GRAPH,
  binding_table = ZU_TYPE_BINDING_TABLE,
  /** Octets rather than text, so nothing here is validated as UTF-8 and
   * nothing is decoded on the way out. Last in the list because the
   * order is the ABI's numbering and this is what ABI 0.14 added. */
  bytes = ZU_TYPE_BYTES,
};

/** Which temporal a temporal is. The unit follows the kind: days for a
 * date, months for a year-month duration, nanoseconds for the other
 * five. */
enum class TemporalKind : int {
  date = ZU_TEMPORAL_DATE,
  local_time = ZU_TEMPORAL_LOCAL_TIME,
  zoned_time = ZU_TEMPORAL_ZONED_TIME,
  local_datetime = ZU_TEMPORAL_LOCAL_DATETIME,
  zoned_datetime = ZU_TEMPORAL_ZONED_DATETIME,
  duration_year_month = ZU_TEMPORAL_DURATION_YEAR_MONTH,
  duration_day_time = ZU_TEMPORAL_DURATION_DAY_TIME,
  /** Not a temporal at all, which is what a frame column of plain
   * numbers passes. */
  plain = ZU_FRAME_PLAIN,
};

/** Where in a statement something happened. Line and column are 1-based
 * and the column counts characters, so a line of multi-byte text does
 * not read as wider than it looks. The offset is a 0-based byte index
 * into the statement and is always on a character boundary. */
struct Position {
  /** The line the place is on, counting from one. */
  std::uint32_t line = 0;
  /** The character within that line, counting from one. */
  std::uint32_t column = 0;
  /** The same place counted in bytes from the start of the statement,
   * from zero. */
  std::uint32_t offset = 0;

  /** Two positions are the same place when all three agree. */
  friend bool operator==(const Position&, const Position&) = default;
};

/** A node is a table and a row of it. Neither half identifies one on its
 * own, because two tables number their rows from zero. */
struct Node {
  /** Which node table, by the id the catalog gave it. */
  std::uint32_t table = 0;
  /** Which row of that table, counting from zero. */
  std::uint64_t offset = 0;

  /** The same table and the same row is the same node. */
  friend bool operator==(const Node&, const Node&) = default;
};

/** An edge is a table and the two rows it runs between. */
struct Rel {
  /** Which edge table, by the id the catalog gave it. */
  std::uint32_t table = 0;
  /** The row the edge runs from. */
  std::uint64_t src = 0;
  /** The row the edge runs to. */
  std::uint64_t dst = 0;

  /** The same table between the same two rows is the same edge. */
  friend bool operator==(const Rel&, const Rel&) = default;
};

/** A temporal value, which is a kind and a count in the unit that kind
 * implies. The offset is minutes east of UTC and is nought for the five
 * kinds that carry none.
 *
 * The chrono helpers are conversions rather than the representation,
 * because there is no one chrono type these seven map onto: a date is a
 * count of days, a duration in months is not a chrono duration at all
 * without a calendar to resolve it against, and a zoned time carries an
 * offset that sys_time has nowhere to keep. What is offered is the
 * conversion each kind does have, and a caller who wants the others
 * writes the arithmetic where the meaning is known. */
struct Temporal {
  /** Which of the seven this is, and so what unit the count is in. */
  TemporalKind kind = TemporalKind::date;
  /** How many of that unit, from the epoch for a point in time and from
   * nothing for a duration. */
  std::int64_t count = 0;
  /** Minutes east of UTC, and nought for the kinds that carry none. */
  std::int32_t offset = 0;

  /** Two temporals are equal when the kind, the count and the offset
   * all agree, which means the same instant written in two zones is
   * two values rather than one. */
  friend bool operator==(const Temporal&, const Temporal&) = default;

  ///@{
  /** One of these per kind, because chrono has no single type these
   * seven map onto and a constructor taking a count and a kind is the
   * one a caller gets backwards. Each names the kind it builds and
   * takes the chrono type that kind is actually written in. */
  static Temporal date(std::chrono::sys_days d) {
    return {TemporalKind::date, d.time_since_epoch().count(), 0};
  }
  static Temporal date(std::chrono::year_month_day ymd) {
    return date(std::chrono::sys_days{ymd});
  }
  static Temporal local_time(std::chrono::nanoseconds since_midnight) {
    return {TemporalKind::local_time, since_midnight.count(), 0};
  }
  static Temporal zoned_time(std::chrono::nanoseconds since_midnight, std::chrono::minutes east) {
    return {TemporalKind::zoned_time, since_midnight.count(),
            static_cast<std::int32_t>(east.count())};
  }
  static Temporal local_datetime(std::chrono::sys_time<std::chrono::nanoseconds> t) {
    return {TemporalKind::local_datetime, t.time_since_epoch().count(), 0};
  }
  static Temporal zoned_datetime(std::chrono::sys_time<std::chrono::nanoseconds> t,
                                 std::chrono::minutes east) {
    return {TemporalKind::zoned_datetime, t.time_since_epoch().count(),
            static_cast<std::int32_t>(east.count())};
  }
  static Temporal months(std::chrono::months m) {
    return {TemporalKind::duration_year_month, m.count(), 0};
  }
  static Temporal nanos(std::chrono::nanoseconds d) {
    return {TemporalKind::duration_day_time, d.count(), 0};
  }
  ///@}

  ///@{
  /** The count as the chrono type its kind means it in. Reading one as
   * the wrong kind is a caller's mistake and not a thing this can
   * check, so each is named for the unit rather than for the kind. The
   * offset is not folded in: east() reads it, and what to do with it is
   * the caller's, because a zoned time and a zoned datetime want
   * different things done with the same number. */
  std::chrono::sys_days as_days() const {
    return std::chrono::sys_days{std::chrono::days{count}};
  }
  std::chrono::nanoseconds as_nanos() const { return std::chrono::nanoseconds{count}; }
  std::chrono::months as_months() const { return std::chrono::months{count}; }
  std::chrono::sys_time<std::chrono::nanoseconds> as_time() const {
    return std::chrono::sys_time<std::chrono::nanoseconds>{std::chrono::nanoseconds{count}};
  }
  std::chrono::minutes east() const { return std::chrono::minutes{offset}; }
  ///@}
};

/* ---- errors ---- */

/** Everything a failure has to say, read out of the zu_error before it
 * was freed.
 *
 * The strings are copies rather than views, and they have to be: the
 * handle they came from is freed by the call that builds this, because
 * an error that had to be freed by hand would be the one allocation in
 * this header a caller could leak. Errors are the cold path and a copy
 * of a sentence is nothing beside the query that failed.
 *
 * A field the condition has no answer for is empty rather than filled
 * with a guess. A division by zero happens while a statement runs and
 * has no token to point at, so it carries a code and no position; a
 * statement that failed to parse carries both. */
class Error {
 public:
  Error() = default;

  /** Reads e and frees it. Safe on a null e, which is what a structural
   * failure hands over: the status is then the whole of what happened
   * and what names the call. */
  static Error take(Status status, zu_error* e, std::string_view what = {}) {
    Error out;
    out.status_ = status;
    if (e == nullptr) {
      out.message_ = what.empty() ? default_message(status) : std::string(what);
      out.severity_ = status == Status::ok ? Severity::success : Severity::exception;
      return out;
    }
    out.message_ = copy(e, &zu_error_message).value_or("");
    out.code_ = copy(e, &zu_error_code);
    out.condition_ = copy(e, &zu_error_standard_text);
    out.doc_url_ = copy(e, &zu_error_doc_url);
    out.excerpt_ = copy(e, &zu_error_excerpt);
    out.subject_kind_ = copy(e, &zu_error_subject_kind);
    out.subject_ = copy(e, &zu_error_subject);
    out.graph_ = copy(e, &zu_error_graph);
    out.schema_ = copy(e, &zu_error_schema);
    const std::int32_t sev = zu_error_severity(e);
    out.severity_ = sev < 0 ? Severity::exception : static_cast<Severity>(sev);
    out.retryable_ = zu_error_retryable(e) == 1;
    std::uint32_t line = 0;
    std::uint32_t column = 0;
    std::uint32_t offset = 0;
    if (zu_error_position(e, &line, &column) == ZU_OK) {
      zu_error_offset(e, &offset);
      out.position_ = Position{line, column, offset};
    }
    zu_error_free(e);
    if (out.message_.empty()) {
      out.message_ = what.empty() ? default_message(status) : std::string(what);
    }
    return out;
  }

  /** What the call answered, which is the shape of the failure as
   * against the condition it raised. */
  Status status() const noexcept { return status_; }

  /** zu's own account, naming the table, the token or the value. Never
   * empty, because a failure a caller can only print is still a failure
   * a caller has to print. */
  std::string_view message() const noexcept { return message_; }

  /** The five-character GQLSTATUS code, "42001" for a syntax error.
   * Empty for the failures the standard has no condition for, an
   * interrupted statement among them. */
  std::optional<std::string_view> code() const noexcept { return view(code_); }

  /** The standard's own words for the condition class and subclass,
   * which is what a conformance harness grades. */
  std::optional<std::string_view> condition() const noexcept { return view(condition_); }

  /** Where the condition is written up, so a reader is handed a page
   * rather than five characters to search for. */
  std::optional<std::string_view> doc_url() const noexcept { return view(doc_url_); }

  /** The line the position is on, without its newline, which the column
   * counts characters into. Both halves of a caret without having kept
   * the statement text. */
  std::optional<std::string_view> excerpt() const noexcept { return view(excerpt_); }

  ///@{
  /** What the condition is about, as a kind and a name: "variable" and
   * "nope", "function" and "nosuchfn". This is the pair a tool acts on
   * rather than prints, because a name in a sentence has to be parsed
   * back out of it and a name here does not. An editor underlines the
   * subject, a REPL suggests a spelling near it, and a test asserts
   * which thing was wrong rather than matching on English.
   *
   * Empty for the conditions that are about no particular thing, a
   * division by zero among them. */
  std::optional<std::string_view> subject_kind() const noexcept { return view(subject_kind_); }
  std::optional<std::string_view> subject() const noexcept { return view(subject_); }
  ///@}

  ///@{
  /** Where the statement was running, which a host with more than one
   * graph open needs in order to say which one refused it. */
  std::optional<std::string_view> graph() const noexcept { return view(graph_); }
  std::optional<std::string_view> schema() const noexcept { return view(schema_); }
  ///@}

  /** How bad it is. Everything thrown is an exception; a warning rides
   * along with a result and reaches a caller through notices rather
   * than through a throw. */
  Severity severity() const noexcept { return severity_; }

  /** True when running the same statement again could succeed. A write
   * that lost to a concurrent one is the case: nothing of it was
   * applied. A retry loop reads this rather than carrying a list of
   * codes, which is the sort of list that is right in one binding and
   * stale in the others. */
  bool retryable() const noexcept { return retryable_; }

  /** Where in the statement it happened, when the condition has a token
   * to point at. Empty rather than nought for the ones that do not,
   * because a made up position underlines the wrong word rather than
   * none. */
  std::optional<Position> position() const noexcept { return position_; }

  /** The whole report, which is the message and, where there is one, the
   * excerpt with a caret under the column. Two lines rather than one,
   * for a program that prints a failure to somebody who has to fix the
   * statement. */
  std::string report() const {
    std::string out(message_);
    if (excerpt_ && position_) {
      out += '\n';
      out += *excerpt_;
      out += '\n';
      /* The column counts characters and the excerpt is bytes, so the
       * caret is placed by walking characters rather than by
       * multiplying. A continuation byte is not the start of one. */
      std::size_t bytes = 0;
      std::uint32_t chars = 1;
      while (bytes < excerpt_->size() && chars < position_->column) {
        ++bytes;
        while (bytes < excerpt_->size() &&
               (static_cast<unsigned char>((*excerpt_)[bytes]) & 0xC0) == 0x80) {
          ++bytes;
        }
        ++chars;
      }
      out.append(chars > 1 ? chars - 1 : 0, ' ');
      out += '^';
    }
    return out;
  }

 private:
  using accessor = const char* (*)(const zu_error*, std::size_t*);

  static std::optional<std::string> copy(const zu_error* e, accessor fn) {
    std::size_t len = 0;
    const char* p = fn(e, &len);
    if (p == nullptr) {
      return std::nullopt;
    }
    return std::string(p, len);
  }

  static std::optional<std::string_view> view(const std::optional<std::string>& s) noexcept {
    if (!s) {
      return std::nullopt;
    }
    return std::string_view(*s);
  }

  static std::string default_message(Status s) {
    switch (s) {
      case Status::ok:
        return "no failure";
      case Status::done:
        return "there is nothing to read";
      case Status::error:
        return "the engine refused the work";
      case Status::misuse:
        return "the call broke the contract in zu.h";
      case Status::misuse_concurrent:
        return "two threads used one connection at once";
      case Status::misuse_closed:
        return "the handle was used after its connection closed";
      case Status::interrupted:
        return "the statement was interrupted";
      case Status::conflict:
        return "the write lost to a concurrent one";
      case Status::corrupt:
        return "the file says something that cannot be true";
      case Status::unsupported:
        return "this build does not have that";
      case Status::io:
        return "the operating system refused a read or a write";
    }
    return "unknown status";
  }

  Status status_ = Status::ok;
  std::string message_;
  std::optional<std::string> code_;
  std::optional<std::string> condition_;
  std::optional<std::string> doc_url_;
  std::optional<std::string> excerpt_;
  std::optional<std::string> subject_kind_;
  std::optional<std::string> subject_;
  std::optional<std::string> graph_;
  std::optional<std::string> schema_;
  Severity severity_ = Severity::success;
  bool retryable_ = false;
  std::optional<Position> position_;
};

/** The base of the exception hierarchy. There is one subclass per
 * GQLSTATUS condition class, which is what the two characters that open
 * a code are for: catching DataError catches every one of the
 * conditions in class 22 without listing them, and a condition zu adds
 * to that class later is caught by the same catch.
 *
 * what() is the message. The rest is on error(). */
class Exception : public std::runtime_error {
 public:
  /** Takes the failure, and hands its message to std::runtime_error so
   * that a catch site that only knows about the standard library still
   * prints something worth reading from what(). */
  explicit Exception(Error e)
      : std::runtime_error(std::string(e.message())), error_(std::move(e)) {}

  /** The whole of the failure: the code, the condition, the position,
   * the subject. what() is the message alone, which is a sentence and
   * not a thing to act on. */
  const Error& error() const noexcept { return error_; }

  ///@{
  /** The three fields a catch site reads often enough to be worth
   * reaching without going through error() first. */
  Status status() const noexcept { return error_.status(); }
  std::optional<std::string_view> code() const noexcept { return error_.code(); }
  bool retryable() const noexcept { return error_.retryable(); }
  ///@}

 private:
  Error error_;
};

/** Class 08: the database could not be reached, or the operating system
 * refused a read or a write. */
class ConnectionError : public Exception {
 public:
  using Exception::Exception;
};
/** Class 22: the data. A value out of range, a cast that will not go, a
 * division by zero. */
class DataError : public Exception {
 public:
  using Exception::Exception;
};
/** Classes 25, 2D and 40, and a write that lost to a concurrent one:
 * everything about a transaction. */
class TransactionError : public Exception {
 public:
  using Exception::Exception;
};
/** Class 42: the statement did not parse, or named something that is not
 * there. */
class SyntaxError : public Exception {
 public:
  using Exception::Exception;
};
/** The contract in zu.h was broken. An index out of range, an accessor
 * asked for a column that does not hold what it reads. Nothing is wrong
 * with the database and the call did nothing. */
class ProgrammingError : public Exception {
 public:
  using Exception::Exception;
};
/** Two threads used one connection at once. Connect again rather than
 * share. */
class ConcurrentError : public ProgrammingError {
 public:
  using ProgrammingError::ProgrammingError;
};
/** A handle was used after its connection closed. */
class ClosedError : public ProgrammingError {
 public:
  using ProgrammingError::ProgrammingError;
};
/** The caller stopped the statement. Nothing failed so much as stopped,
 * and the connection runs the next statement normally. */
class InterruptedError : public Exception {
 public:
  using Exception::Exception;
};
/** Everything else, which is the engine having gone wrong rather than
 * the caller. */
class InternalError : public Exception {
 public:
  using Exception::Exception;
};

namespace detail {

[[noreturn]] inline void raise(Error e) {
  const auto code = e.code();
  const std::string_view klass = code && code->size() >= 2 ? code->substr(0, 2) : std::string_view{};
  if (klass == "08") {
    throw ConnectionError(std::move(e));
  }
  if (klass == "22") {
    throw DataError(std::move(e));
  }
  if (klass == "25" || klass == "2D" || klass == "40") {
    throw TransactionError(std::move(e));
  }
  if (klass == "42") {
    throw SyntaxError(std::move(e));
  }
  switch (e.status()) {
    case Status::misuse:
      throw ProgrammingError(std::move(e));
    case Status::misuse_concurrent:
      throw ConcurrentError(std::move(e));
    case Status::misuse_closed:
      throw ClosedError(std::move(e));
    case Status::interrupted:
      throw InterruptedError(std::move(e));
    case Status::conflict:
      throw TransactionError(std::move(e));
    case Status::io:
      throw ConnectionError(std::move(e));
    default:
      throw InternalError(std::move(e));
  }
}

/** What every operation in this header is written as. One of these is a
 * value or a failure, and the two public spellings are each one line
 * over it, which is what keeps them from disagreeing. void is carried
 * as monostate rather than specialised, because a specialisation would
 * be the second implementation this exists to avoid. */
template <class T>
class Outcome {
 public:
  Outcome(T value) : v_(std::move(value)) {}  // NOLINT(google-explicit-constructor)
  Outcome(Error e) : v_(std::move(e)) {}      // NOLINT(google-explicit-constructor)

  bool ok() const noexcept { return v_.index() == 0; }
  T& value() noexcept { return *std::get_if<0>(&v_); }
  Error& error() noexcept { return *std::get_if<1>(&v_); }

 private:
  std::variant<T, Error> v_;
};

using Nothing = std::monostate;

template <class T>
T unwrap(Outcome<T>&& o) {
  if (!o.ok()) {
    raise(std::move(o.error()));
  }
  return std::move(o.value());
}

inline void unwrap_void(Outcome<Nothing>&& o) {
  if (!o.ok()) {
    raise(std::move(o.error()));
  }
}

/** Any range of anything a string_view can be made from, which covers a
 * vector of std::string, one of string_view, and an array of literals.
 * The two bulk paths that take strings take one of these rather than a
 * span of views, because a host holding strings should not have to
 * build a vector of views to hand them over. */
template <class R>
concept StringRange = std::ranges::input_range<R> &&
                      std::convertible_to<std::ranges::range_reference_t<R>, std::string_view>;

/** A contiguous range of numbers, which is what a frame column is over.
 * bool is out because a C++ vector of them is a bitfield with no array
 * behind it, and a frame takes Arrow's bitmap through its own call. */
template <class R>
concept NumericRange =
    std::ranges::contiguous_range<R> && std::ranges::sized_range<R> &&
    (std::integral<std::ranges::range_value_t<R>> ||
     std::floating_point<std::ranges::range_value_t<R>>) &&
    (!std::same_as<std::ranges::range_value_t<R>, bool>);

template <StringRange R>
std::vector<std::string_view> to_views(const R& r) {
  std::vector<std::string_view> out;
  if constexpr (std::ranges::sized_range<R>) {
    out.reserve(std::ranges::size(r));
  }
  for (auto&& s : r) {
    out.emplace_back(std::string_view(s));
  }
  return out;
}

/** Every handle in zu.h, closed on the way out of scope. Move only:
 * copying one would be two frees of one pointer, and there is no call
 * in the C API that duplicates a handle for us. */
template <class T, void (*Free)(T*)>
class Handle {
 public:
  Handle() = default;
  explicit Handle(T* p) noexcept : p_(p) {}
  ~Handle() {
    if (p_ != nullptr) {
      Free(p_);
    }
  }

  Handle(Handle&& o) noexcept : p_(std::exchange(o.p_, nullptr)) {}
  Handle& operator=(Handle&& o) noexcept {
    if (this != &o) {
      reset(std::exchange(o.p_, nullptr));
    }
    return *this;
  }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;

  T* get() const noexcept { return p_; }
  explicit operator bool() const noexcept { return p_ != nullptr; }
  T* release() noexcept { return std::exchange(p_, nullptr); }
  void reset(T* p = nullptr) noexcept {
    if (p_ != nullptr) {
      Free(p_);
    }
    p_ = p;
  }

 private:
  T* p_ = nullptr;
};

/** The two shapes a fallible call comes in. A call with an error handle
 * has something to say about why; a call without one is structural and
 * the status names it exactly, so the call's own name is the message.
 *
 * The error slot is passed by its address rather than by its value, and
 * that is not a style. Every one of these reads
 *
 *   checked(zu_something(..., &err), &err, "zu_something")
 *
 * and the order in which a compiler evaluates those two arguments is
 * unspecified: it may read err before it makes the call that sets it.
 * Passing the value that way loses every failure the engine explained,
 * on whichever compiler chose that order, and leaves a status with no
 * code, no message and no position behind it, which looks from the
 * outside exactly like an engine that said nothing. Taking the address
 * cannot go wrong in either order, because what is read early is where
 * err lives rather than what it holds, and it is read here after the
 * call has certainly returned. */
inline std::optional<Error> checked(zu_status st, zu_error** slot, std::string_view what) {
  zu_error* err = slot == nullptr ? nullptr : *slot;
  if (slot != nullptr) {
    *slot = nullptr;
  }
  if (st == ZU_OK || st == ZU_DONE) {
    if (err != nullptr) {
      zu_error_free(err);
    }
    return std::nullopt;
  }
  return Error::take(static_cast<Status>(st), err, what);
}

inline std::optional<Error> checked(zu_status st, std::string_view what) {
  if (st == ZU_OK || st == ZU_DONE) {
    return std::nullopt;
  }
  return Error::take(static_cast<Status>(st), nullptr, what);
}

}  // namespace detail

#if ZU_HAS_EXPECTED
/** What every try_ call answers. */
template <class T>
using expected = std::expected<T, Error>;

namespace detail {
template <class T>
expected<T> to_expected(Outcome<T>&& o) {
  if (!o.ok()) {
    return std::unexpected(std::move(o.error()));
  }
  return std::move(o.value());
}
inline expected<void> to_expected_void(Outcome<Nothing>&& o) {
  if (!o.ok()) {
    return std::unexpected(std::move(o.error()));
  }
  return {};
}
}  // namespace detail
#endif

///@{
/** What the loaded libzu calls itself, and the ABI this header was
 * written against. The two are not the same fact: a library and a
 * header that disagree about the ABI is what abi_version is for. */
inline std::string_view version() { return zu_version(); }
inline std::string_view abi_version() { return ZU_ABI_VERSION; }
///@}

/* ---- values ---- */

class Value;

/** A list, a path or a record, as a range over its elements. Cheap: it
 * holds the parent value and an index, and every element is read out of
 * the result where it lies. */
class ValueRange;

/** One cell, borrowed from the result that produced it. Nothing to free,
 * and it lives exactly as long as that result does.
 *
 * These read a value as the type it is and nothing else, which is where
 * they differ from the columns: a column reads bools and nulls as
 * integers too, because a column is one host array and something has to
 * go in every slot, while as_int on a bool is a failure. */
class Value {
 public:
  Value() = default;
  /** Borrows a cell the C API already handed out. Nothing is copied and
   * nothing is owned, so the zu_value has to outlive this. */
  explicit Value(const zu_value* v) noexcept : v_(v) {}

  ///@{
  /** The handle underneath and whether there is one, for a caller who
   * has to reach a zu_ call this header does not wrap. Reading through
   * raw() is fine; freeing what it points at is not. */
  const zu_value* raw() const noexcept { return v_; }
  explicit operator bool() const noexcept { return v_ != nullptr; }
  ///@}

  /** What the cell holds, which is what says which of the readers below
   * will answer. A cell with no value reads as Type::null. */
  Type type() const noexcept {
    const std::int32_t t = zu_value_type(v_);
    return t < 0 ? Type::null : static_cast<Type>(t);
  }
  /** True when the cell holds nothing, which is not the same as holding
   * an empty string or a zero. */
  bool is_null() const noexcept { return type() == Type::null; }

  ///@{
  /** Reads the cell as the type it is. Asking for another one throws
   * ProgrammingError rather than converting: a column of integers read
   * as text is a bug in the program and not something to paper over,
   * and null is not any of these types either. */
  bool as_bool() const { return detail::unwrap(bool_impl()); }
  std::int64_t as_int() const { return detail::unwrap(int_impl()); }
  double as_double() const { return detail::unwrap(double_impl()); }
  /** Points into the result's bytes and is NOT NUL-terminated, which is
   * the price of not copying. */
  std::string_view as_string() const { return detail::unwrap(string_impl()); }
  /** Octets, on the same terms: into the result, not copied, and not
   * NUL-terminated. A byte string and a string are different types here
   * and reading one as the other fails, because a blob that happens to
   * be valid UTF-8 is still a blob and a caller who wanted text should
   * be told the column is not text. */
  std::span<const std::uint8_t> as_bytes() const { return detail::unwrap(bytes_impl()); }
  Temporal as_temporal() const { return detail::unwrap(temporal_impl()); }
  Node as_node() const { return detail::unwrap(node_impl()); }
  Rel as_rel() const { return detail::unwrap(rel_impl()); }
  ///@}

  ///@{
  /** How many elements a list, a path or a record has, and 0 for
   * everything else, an empty list included. */
  std::uint64_t size() const noexcept { return zu_value_len(v_); }
  bool empty() const noexcept { return size() == 0; }
  ///@}

  ///@{
  /** One element of a list, a path or a record, counting from zero. Out
   * of range throws ProgrammingError, and so does asking a cell that is
   * not one of those three for an element at all. */
  Value at(std::uint64_t i) const { return detail::unwrap(at_impl(i)); }
  Value operator[](std::uint64_t i) const { return at(i); }
  ///@}

  /** A record's field names, in name order, which is what makes two
   * records written in different orders one value. */
  std::string_view field(std::uint64_t i) const { return detail::unwrap(field_impl(i)); }

  /** The same elements as a range, for the loop and the view pipeline
   * that at() spells out by hand. */
  inline ValueRange elements() const;

#if ZU_HAS_EXPECTED
  ///@{
  /** The expected spelling. Each of these is the call of the same name
   * above it, doing the same work through the same code, and answering
   * a failure rather than throwing it. */
  [[nodiscard]] expected<bool> try_as_bool() const { return detail::to_expected(bool_impl()); }
  [[nodiscard]] expected<std::int64_t> try_as_int() const { return detail::to_expected(int_impl()); }
  [[nodiscard]] expected<double> try_as_double() const { return detail::to_expected(double_impl()); }
  [[nodiscard]] expected<std::string_view> try_as_string() const { return detail::to_expected(string_impl()); }
  [[nodiscard]] expected<std::span<const std::uint8_t>> try_as_bytes() const {
    return detail::to_expected(bytes_impl());
  }
  [[nodiscard]] expected<Temporal> try_as_temporal() const { return detail::to_expected(temporal_impl()); }
  [[nodiscard]] expected<Node> try_as_node() const { return detail::to_expected(node_impl()); }
  [[nodiscard]] expected<Rel> try_as_rel() const { return detail::to_expected(rel_impl()); }
  [[nodiscard]] expected<Value> try_at(std::uint64_t i) const { return detail::to_expected(at_impl(i)); }
  [[nodiscard]] expected<std::string_view> try_field(std::uint64_t i) const {
    return detail::to_expected(field_impl(i));
  }
  ///@}
#endif

 private:
  detail::Outcome<bool> bool_impl() const {
    std::int32_t out = 0;
    if (auto e = detail::checked(zu_value_bool(v_, &out), "zu_value_bool")) {
      return std::move(*e);
    }
    return out != 0;
  }
  detail::Outcome<std::int64_t> int_impl() const {
    std::int64_t out = 0;
    if (auto e = detail::checked(zu_value_i64(v_, &out), "zu_value_i64")) {
      return std::move(*e);
    }
    return out;
  }
  detail::Outcome<double> double_impl() const {
    double out = 0;
    if (auto e = detail::checked(zu_value_f64(v_, &out), "zu_value_f64")) {
      return std::move(*e);
    }
    return out;
  }
  detail::Outcome<std::string_view> string_impl() const {
    const char* p = nullptr;
    std::size_t len = 0;
    if (auto e = detail::checked(zu_value_str(v_, &p, &len), "zu_value_str")) {
      return std::move(*e);
    }
    return std::string_view(p == nullptr ? "" : p, len);
  }
  detail::Outcome<std::span<const std::uint8_t>> bytes_impl() const {
    const std::uint8_t* p = nullptr;
    std::size_t len = 0;
    if (auto e = detail::checked(zu_value_bytes(v_, &p, &len), "zu_value_bytes")) {
      return std::move(*e);
    }
    /* An empty byte string is a length of zero and a pointer that may be
     * anything, and a span built on a null pointer is one nobody can
     * safely iterate even when it is empty. */
    if (p == nullptr || len == 0) {
      return std::span<const std::uint8_t>{};
    }
    return std::span<const std::uint8_t>(p, len);
  }
  detail::Outcome<Temporal> temporal_impl() const {
    std::int32_t kind = 0;
    std::int64_t count = 0;
    std::int32_t offset = 0;
    if (auto e = detail::checked(zu_value_temporal(v_, &kind, &count, &offset),
                                 "zu_value_temporal")) {
      return std::move(*e);
    }
    return Temporal{static_cast<TemporalKind>(kind), count, offset};
  }
  detail::Outcome<Node> node_impl() const {
    Node n;
    if (auto e = detail::checked(zu_value_node(v_, &n.table, &n.offset), "zu_value_node")) {
      return std::move(*e);
    }
    return n;
  }
  detail::Outcome<Rel> rel_impl() const {
    Rel r;
    if (auto e = detail::checked(zu_value_rel(v_, &r.table, &r.src, &r.dst), "zu_value_rel")) {
      return std::move(*e);
    }
    return r;
  }
  detail::Outcome<Value> at_impl(std::uint64_t i) const {
    const zu_value* out = nullptr;
    if (auto e = detail::checked(zu_value_at(v_, i, &out), "zu_value_at")) {
      return std::move(*e);
    }
    return Value(out);
  }
  detail::Outcome<std::string_view> field_impl(std::uint64_t i) const {
    const char* p = nullptr;
    std::size_t len = 0;
    if (auto e = detail::checked(zu_value_field(v_, i, &p, &len), "zu_value_field")) {
      return std::move(*e);
    }
    return std::string_view(p == nullptr ? "" : p, len);
  }

  const zu_value* v_ = nullptr;
};

/** The elements of a list, a path or a record, as a random access range.
 * Random access rather than input, because a list is already in memory
 * and an iterator that had to walk it would be a promise this makes
 * about the engine that is not true. */
class ValueRange : public std::ranges::view_interface<ValueRange> {
 public:
  /** A random access iterator over the elements, holding the parent and
   * an index and nothing else. */
  class iterator {
   public:
    ///@{
    /** What comes back is made on dereference rather than stored, so it
     * is a value and not a reference. The concept is what the ranges
     * algorithms read and it is random access, because everything is
     * already in memory; the category stays input, because a legacy
     * random access iterator has to hand out a real reference and this
     * one has none to hand out. */
    using iterator_category = std::input_iterator_tag;
    using iterator_concept = std::random_access_iterator_tag;
    using value_type = Value;
    using difference_type = std::ptrdiff_t;
    using reference = Value;
    ///@}

    iterator() = default;
    /** The one the range's begin and end build. Nothing is owned and
     * the parent has to outlive it. */
    iterator(const Value* parent, std::uint64_t i) noexcept : parent_(parent), i_(i) {}

    ///@{
    /** Reads the element, which is built here rather than pointed at. */
    Value operator*() const { return parent_->at(i_); }
    Value operator[](difference_type n) const {
      return parent_->at(static_cast<std::uint64_t>(static_cast<difference_type>(i_) + n));
    }
    ///@}

    ///@{
    /** The random access protocol, which is arithmetic on the index.
     * None of it reads the parent, so moving an iterator past the end
     * is defined and dereferencing it there is not. */
    iterator& operator++() noexcept {
      ++i_;
      return *this;
    }
    iterator operator++(int) noexcept {
      iterator c = *this;
      ++i_;
      return c;
    }
    iterator& operator--() noexcept {
      --i_;
      return *this;
    }
    iterator operator--(int) noexcept {
      iterator c = *this;
      --i_;
      return c;
    }
    iterator& operator+=(difference_type n) noexcept {
      i_ = static_cast<std::uint64_t>(static_cast<difference_type>(i_) + n);
      return *this;
    }
    iterator& operator-=(difference_type n) noexcept { return *this += -n; }
    friend iterator operator+(iterator it, difference_type n) noexcept { return it += n; }
    friend iterator operator+(difference_type n, iterator it) noexcept { return it += n; }
    friend iterator operator-(iterator it, difference_type n) noexcept { return it -= n; }
    friend difference_type operator-(const iterator& a, const iterator& b) noexcept {
      return static_cast<difference_type>(a.i_) - static_cast<difference_type>(b.i_);
    }
    friend bool operator==(const iterator& a, const iterator& b) noexcept { return a.i_ == b.i_; }
    friend std::strong_ordering operator<=>(const iterator& a, const iterator& b) noexcept {
      return a.i_ <=> b.i_;
    }
    ///@}

   private:
    const Value* parent_ = nullptr;
    std::uint64_t i_ = 0;
  };

  ValueRange() = default;
  /** Built by Value::elements rather than by hand. It borrows the cell
   * and has to be outlived by it. */
  explicit ValueRange(const Value* parent) noexcept : parent_(parent) {}

  ///@{
  /** The ends of the range. A default constructed ValueRange is empty
   * rather than undefined, so a range over nothing loops zero times. */
  iterator begin() const noexcept { return {parent_, 0}; }
  iterator end() const noexcept { return {parent_, parent_ == nullptr ? 0 : parent_->size()}; }
  ///@}

 private:
  const Value* parent_ = nullptr;
};

inline ValueRange Value::elements() const { return ValueRange(this); }

/* ---- results ---- */

class Result;
class Connection;

/** One row of a result, which is the result and a row number rather than
 * anything copied out of it. Cheap to make, cheap to pass, and good for
 * exactly as long as the result is. */
class Row {
 public:
  Row() = default;
  /** Built by the result rather than by hand. It borrows the result and
   * has to be outlived by it. */
  Row(const Result* result, std::uint64_t row) noexcept : result_(result), row_(row) {}

  /** Which row of the result this is, counting from zero. */
  std::uint64_t index() const noexcept { return row_; }
  /** How many columns, which is the result's count and the same for
   * every row. */
  inline std::uint32_t size() const noexcept;

  ///@{
  /** What a cell holds and whether it holds anything, by column number
   * or by column name. A name nothing matches throws ProgrammingError
   * rather than reading the first column. */
  inline Type type(std::uint32_t col) const;
  inline Type type(std::string_view name) const;
  inline bool is_null(std::uint32_t col) const;
  inline bool is_null(std::string_view name) const;
  ///@}

  ///@{
  /** One cell, as whatever T asks for.
   *
   * int64_t, double, bool, Temporal, Node and Rel read the cell as the
   * type it is. string_view and string read a string, the first
   * borrowed and NUL-terminated, the second copied. Value hands over
   * the cell itself, for the shapes a scalar cannot hold. And
   * optional<T> is any of those with the null case folded in, which is
   * what a nullable column wants. */
  template <class T>
  T get(std::uint32_t col) const;
  template <class T>
  T get(std::string_view name) const;
  ///@}

  ///@{
  /** The cell itself, untyped, for a caller who wants to ask it what it
   * is rather than tell it. get<T> is the spelling to reach for when
   * the type is known. */
  inline Value operator[](std::uint32_t col) const;
  inline Value operator[](std::string_view name) const;
  ///@}

 private:
  const Result* result_ = nullptr;
  std::uint64_t row_ = 0;
};

/** A statement's answer, and everything that can be read out of it.
 *
 * It owns its rows outright, so it stays readable after the connection
 * that produced it has gone back to a pool. Every view it hands out,
 * every span and every string_view, points into it and is good until it
 * is destroyed.
 *
 * It is a range of Row, so a range-for reads it and the standard views
 * compose over it. The columnar accessors are the other half: where a
 * loop over rows is what a program wants to write, a span over a whole
 * column is what a program wants to compute on, and the engine filled
 * that column contiguously so the span costs a bounds check. */
class Result {
 public:
  Result() = default;
  /** Adopts a zu_result and closes it at the end of the scope. The
   * calls that answer a result hand one over already made; this is for
   * a caller who got theirs from the C API directly. */
  explicit Result(zu_result* r) noexcept : h_(r) {}

  ///@{
  /** The handle underneath and whether there is one, for a caller who
   * has to reach a zu_ call this header does not wrap. Reading through
   * raw() is fine; closing what it points at is not, because the
   * destructor will close it again. */
  zu_result* raw() const noexcept { return h_.get(); }
  explicit operator bool() const noexcept { return static_cast<bool>(h_); }
  ///@}

  ///@{
  /** How many rows and how many columns. Both are counts the result
   * already knows, so neither reads the data. */
  std::uint64_t rows() const noexcept { return zu_result_rows(h_.get()); }
  std::uint32_t cols() const noexcept { return zu_result_cols(h_.get()); }
  ///@}

  ///@{
  /** The range spelling of the same count, so that the standard
   * algorithms and views see a sized range. */
  std::uint64_t size() const noexcept { return rows(); }
  bool empty() const noexcept { return rows() == 0; }
  ///@}

  /** What a column is called, borrowed from the result. Out of range
   * throws ProgrammingError. */
  std::string_view name(std::uint32_t col) const { return detail::unwrap(name_impl(col)); }

  /** Which column that name is, or nothing. The names are read once and
   * kept, because a program that reads a column by name reads it once a
   * row and a linear walk of the C API per read would be the cost this
   * wrapper was supposed to save. */
  std::optional<std::uint32_t> find(std::string_view name) const {
    const auto& all = names();
    for (std::uint32_t i = 0; i < all.size(); ++i) {
      if (all[i] == name) {
        return i;
      }
    }
    return std::nullopt;
  }

  /** The same, and a failure naming what was asked for when there is no
   * such column, which is the mistake a caller actually makes. */
  std::uint32_t column(std::string_view name) const {
    return detail::unwrap(column_impl(name));
  }

  /** Every column name in order, read once and kept. This is what find
   * and column walk, so a program that reads by name pays for the walk
   * of the C API once per result rather than once per read. */
  const std::vector<std::string_view>& names() const {
    if (!named_) {
      const std::uint32_t n = cols();
      names_.reserve(n);
      for (std::uint32_t i = 0; i < n; ++i) {
        names_.push_back(detail::unwrap(name_impl(i)));
      }
      named_ = true;
    }
    return names_;
  }

  /** What one cell holds. A column is not one type in general, because
   * a null is a type of its own and an expression may answer different
   * things on different rows. */
  Type type(std::uint64_t row, std::uint32_t col) const {
    return detail::unwrap(type_impl(row, col));
  }

  ///@{
  /** The whole column in one call, over the engine's own buffer.
   *
   * ints reads integers and booleans, doubles reads floats and
   * integers, and node_offsets reads the row offset that identifies a
   * node. A null reads as 0 in all three, which valid tells apart. A
   * node is not an integer here: reading one through ints is how a
   * binding ends up handing an internal row number to somebody who
   * asked for an identity, so it is a failure rather than a number.
   *
   * The span is empty for a result with no rows, which is the ZU_DONE
   * case and not a failure. */
  std::span<const std::int64_t> ints(std::uint32_t col) const {
    return detail::unwrap(ints_impl(col));
  }
  std::span<const double> doubles(std::uint32_t col) const {
    return detail::unwrap(doubles_impl(col));
  }
  std::span<const std::uint64_t> node_offsets(std::uint32_t col) const {
    return detail::unwrap(node_offsets_impl(col));
  }
  ///@}

  /** One byte a row, nonzero where the cell is not null. */
  std::span<const std::uint8_t> valid(std::uint32_t col) const {
    return detail::unwrap(valid_impl(col));
  }

  ///@{
  /** The same four columnar reads, by column name. Each is the numbered
   * one with column() in front of it, so a name nothing matches is a
   * failure rather than a read of column nought. */
  std::span<const std::int64_t> ints(std::string_view name) const { return ints(column(name)); }
  std::span<const double> doubles(std::string_view name) const { return doubles(column(name)); }
  std::span<const std::uint64_t> node_offsets(std::string_view name) const {
    return node_offsets(column(name));
  }
  std::span<const std::uint8_t> valid(std::string_view name) const { return valid(column(name)); }
  ///@}

  /** One string cell, NUL-terminated and borrowed from the result. */
  std::string_view str(std::uint64_t row, std::uint32_t col) const {
    return detail::unwrap(str_impl(row, col));
  }

  /** One cell, untyped, borrowed from the result. This is the reader
   * for the shapes a span cannot hold: a list, a path, a record. */
  Value cell(std::uint64_t row, std::uint32_t col) const {
    return detail::unwrap(cell_impl(row, col));
  }

  /** The completion condition, "00000" for a statement that answered
   * with columns and "00001" for one that had none to give back. Never
   * empty. */
  std::string_view gqlstatus() const {
    std::size_t len = 0;
    const char* p = zu_result_gqlstatus(h_.get(), &len);
    return std::string_view(p == nullptr ? "" : p, p == nullptr ? 0 : len);
  }

  ///@{
  /** The conditions the statement raised and carried on through. Almost
   * every statement raises none, so a caller that asks and finds nought
   * has paid for one call. */
  std::uint32_t notice_count() const { return zu_result_notices(h_.get()); }
  std::vector<Error> notices() const {
    std::vector<Error> out;
    const std::uint32_t n = notice_count();
    out.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) {
      zu_error* e = nullptr;
      if (zu_result_notice(h_.get(), i, &e) != ZU_OK) {
        break;
      }
      out.push_back(Error::take(Status::ok, e, "zu_result_notice"));
    }
    return out;
  }
  ///@}

  /* ---- chunks ----
   *
   * The same columns, a chunk of rows at a time. Which one to use is a
   * question of size, and only for the columns the engine did not fill:
   * a whole-column read of a computed column converts all of it before
   * returning any of it, so reading the first hundred rows of a million
   * pays for the other 999,900.
   *
   * That is the trade: a chunk span is good until the next call for the
   * same column and the same accessor, which may replace its contents,
   * or until the result is destroyed. */
  /** Where one chunk sits in the result: the row it starts at and how
   * many rows it holds. */
  struct ChunkExtent {
    /** The row this chunk starts at, counting from zero in the whole
     * result rather than in the chunk. */
    std::uint64_t offset = 0;
    /** How many rows are in it. The last chunk is usually short. */
    std::uint64_t rows = 0;
  };

  ///@{
  /** How many chunks there are and where each one sits. A result with
   * no rows has no chunks, and an index past the end is a failure
   * rather than an empty extent. */
  std::uint64_t chunk_count() const noexcept { return zu_result_chunk_count(h_.get()); }
  ChunkExtent chunk(std::uint64_t i) const { return detail::unwrap(chunk_impl(i)); }
  ///@}

  ///@{
  /** The columnar reads again, one chunk at a time rather than the
   * whole column. Each span covers that chunk's rows and is good until
   * the next call for the same column and the same accessor, or until
   * the result is destroyed. */
  std::span<const std::int64_t> chunk_ints(std::uint64_t chunk, std::uint32_t col) const {
    return detail::unwrap(chunk_ints_impl(chunk, col));
  }
  std::span<const double> chunk_doubles(std::uint64_t chunk, std::uint32_t col) const {
    return detail::unwrap(chunk_doubles_impl(chunk, col));
  }
  std::span<const std::uint64_t> chunk_node_offsets(std::uint64_t chunk, std::uint32_t col) const {
    return detail::unwrap(chunk_node_offsets_impl(chunk, col));
  }
  std::span<const std::uint8_t> chunk_valid(std::uint64_t chunk, std::uint32_t col) const {
    return detail::unwrap(chunk_valid_impl(chunk, col));
  }
  ///@}

  /* ---- rows as a range ---- */

  /** A random access iterator over the rows, holding the result and a
   * row number and nothing else. */
  class iterator {
   public:
    ///@{
    /** What comes back is made on dereference rather than stored, so it
     * is a value and not a reference. The concept is what the ranges
     * algorithms read and it is random access, because everything is
     * already in memory; the category stays input, because a legacy
     * random access iterator has to hand out a real reference and this
     * one has none to hand out. */
    using iterator_category = std::input_iterator_tag;
    using iterator_concept = std::random_access_iterator_tag;
    using value_type = Row;
    using difference_type = std::ptrdiff_t;
    using reference = Row;
    ///@}

    iterator() = default;
    /** The one the result's begin and end build. Nothing is owned and
     * the result has to outlive it. */
    iterator(const Result* result, std::uint64_t row) noexcept : result_(result), row_(row) {}

    ///@{
    /** Reads the row, which is built here rather than pointed at. */
    Row operator*() const noexcept { return Row(result_, row_); }
    Row operator[](difference_type n) const noexcept {
      return Row(result_, static_cast<std::uint64_t>(static_cast<difference_type>(row_) + n));
    }
    ///@}

    ///@{
    /** The random access protocol, which is arithmetic on the row
     * number. None of it reads the result, so moving an iterator past
     * the end is defined and dereferencing it there is not. */
    iterator& operator++() noexcept {
      ++row_;
      return *this;
    }
    iterator operator++(int) noexcept {
      iterator c = *this;
      ++row_;
      return c;
    }
    iterator& operator--() noexcept {
      --row_;
      return *this;
    }
    iterator operator--(int) noexcept {
      iterator c = *this;
      --row_;
      return c;
    }
    iterator& operator+=(difference_type n) noexcept {
      row_ = static_cast<std::uint64_t>(static_cast<difference_type>(row_) + n);
      return *this;
    }
    iterator& operator-=(difference_type n) noexcept { return *this += -n; }
    friend iterator operator+(iterator it, difference_type n) noexcept { return it += n; }
    friend iterator operator+(difference_type n, iterator it) noexcept { return it += n; }
    friend iterator operator-(iterator it, difference_type n) noexcept { return it -= n; }
    friend difference_type operator-(const iterator& a, const iterator& b) noexcept {
      return static_cast<difference_type>(a.row_) - static_cast<difference_type>(b.row_);
    }
    friend bool operator==(const iterator& a, const iterator& b) noexcept {
      return a.row_ == b.row_;
    }
    friend std::strong_ordering operator<=>(const iterator& a, const iterator& b) noexcept {
      return a.row_ <=> b.row_;
    }
    ///@}

   private:
    const Result* result_ = nullptr;
    std::uint64_t row_ = 0;
  };

  ///@{
  /** The ends of the range, and one row by number. Every one of them
   * borrows the result, which is why a Result is not a borrowed_range:
   * a view built over a temporary result would outlive the rows it
   * reads, and the standard is told so rather than trusted not to. */
  iterator begin() const noexcept { return {this, 0}; }
  iterator end() const noexcept { return {this, rows()}; }
  Row row(std::uint64_t i) const noexcept { return Row(this, i); }
  ///@}

  /* ---- arrow ---- */

  ///@{
  /** The other way a result ends. Every call above reads it and leaves
   * it whole; this hands its buffers to an Arrow consumer and gives the
   * result up, which is what makes it free rather than a copy of the
   * whole answer.
   *
   * It takes the result by rvalue for that reason: after this there is
   * nothing to read a second time, and a span or a string_view taken
   * before the call points at bytes that belong to the stream now. The
   * connection is where a node column's table name comes from and may
   * be null, and then the table is named after its id. */
  inline void to_arrow(Connection& conn, ArrowArrayStream* out,
                       std::uint64_t rows_per_batch = 0) &&;
  inline void to_arrow(ArrowArrayStream* out, std::uint64_t rows_per_batch = 0) &&;
  ///@}

#if ZU_HAS_EXPECTED
  ///@{
  /** The expected spelling. Each of these is the call of the same name
   * above it, doing the same work through the same code, and answering
   * a failure rather than throwing it. */
  [[nodiscard]] expected<std::string_view> try_name(std::uint32_t col) const {
    return detail::to_expected(name_impl(col));
  }
  [[nodiscard]] expected<std::uint32_t> try_column(std::string_view name) const {
    return detail::to_expected(column_impl(name));
  }
  [[nodiscard]] expected<Type> try_type(std::uint64_t row, std::uint32_t col) const {
    return detail::to_expected(type_impl(row, col));
  }
  [[nodiscard]] expected<std::span<const std::int64_t>> try_ints(std::uint32_t col) const {
    return detail::to_expected(ints_impl(col));
  }
  [[nodiscard]] expected<std::span<const double>> try_doubles(std::uint32_t col) const {
    return detail::to_expected(doubles_impl(col));
  }
  [[nodiscard]] expected<std::span<const std::uint64_t>> try_node_offsets(std::uint32_t col) const {
    return detail::to_expected(node_offsets_impl(col));
  }
  [[nodiscard]] expected<std::span<const std::uint8_t>> try_valid(std::uint32_t col) const {
    return detail::to_expected(valid_impl(col));
  }
  [[nodiscard]] expected<std::string_view> try_str(std::uint64_t row, std::uint32_t col) const {
    return detail::to_expected(str_impl(row, col));
  }
  [[nodiscard]] expected<Value> try_cell(std::uint64_t row, std::uint32_t col) const {
    return detail::to_expected(cell_impl(row, col));
  }
  [[nodiscard]] expected<ChunkExtent> try_chunk(std::uint64_t i) const {
    return detail::to_expected(chunk_impl(i));
  }
  [[nodiscard]] expected<std::span<const std::int64_t>> try_chunk_ints(std::uint64_t chunk,
                                                         std::uint32_t col) const {
    return detail::to_expected(chunk_ints_impl(chunk, col));
  }
  [[nodiscard]] expected<std::span<const double>> try_chunk_doubles(std::uint64_t chunk,
                                                      std::uint32_t col) const {
    return detail::to_expected(chunk_doubles_impl(chunk, col));
  }
  [[nodiscard]] expected<std::span<const std::uint64_t>> try_chunk_node_offsets(std::uint64_t chunk,
                                                                  std::uint32_t col) const {
    return detail::to_expected(chunk_node_offsets_impl(chunk, col));
  }
  [[nodiscard]] expected<std::span<const std::uint8_t>> try_chunk_valid(std::uint64_t chunk,
                                                          std::uint32_t col) const {
    return detail::to_expected(chunk_valid_impl(chunk, col));
  }
  [[nodiscard]] inline expected<void> try_to_arrow(Connection& conn, ArrowArrayStream* out,
                                     std::uint64_t rows_per_batch = 0) &&;
  ///@}
#endif

 private:
  friend class Row;

  detail::Outcome<std::string_view> name_impl(std::uint32_t col) const {
    const char* p = nullptr;
    std::size_t len = 0;
    if (auto e = detail::checked(zu_result_col_name(h_.get(), col, &p, &len),
                                 "zu_result_col_name")) {
      return std::move(*e);
    }
    return std::string_view(p == nullptr ? "" : p, p == nullptr ? 0 : len);
  }

  detail::Outcome<std::uint32_t> column_impl(std::string_view name) const {
    if (auto i = find(name)) {
      return *i;
    }
    std::string what = "no column named ";
    what += name;
    return Error::take(Status::misuse, nullptr, what);
  }

  detail::Outcome<Type> type_impl(std::uint64_t row, std::uint32_t col) const {
    std::int32_t out = 0;
    if (auto e = detail::checked(zu_result_cell_type(h_.get(), row, col, &out),
                                 "zu_result_cell_type")) {
      return std::move(*e);
    }
    return static_cast<Type>(out);
  }

  template <class T, class F>
  detail::Outcome<std::span<const T>> span_impl(F fn, std::uint32_t col, std::uint64_t rows,
                                                std::string_view what) const {
    const T* p = nullptr;
    const zu_status st = fn(h_.get(), col, &p);
    if (auto e = detail::checked(st, what)) {
      return std::move(*e);
    }
    if (st == ZU_DONE || p == nullptr) {
      return std::span<const T>{};
    }
    return std::span<const T>(p, rows);
  }

  detail::Outcome<std::span<const std::int64_t>> ints_impl(std::uint32_t col) const {
    return span_impl<std::int64_t>(zu_result_col_i64, col, rows(), "zu_result_col_i64");
  }
  detail::Outcome<std::span<const double>> doubles_impl(std::uint32_t col) const {
    return span_impl<double>(zu_result_col_f64, col, rows(), "zu_result_col_f64");
  }
  detail::Outcome<std::span<const std::uint64_t>> node_offsets_impl(std::uint32_t col) const {
    return span_impl<std::uint64_t>(zu_result_col_node_offset, col, rows(),
                                    "zu_result_col_node_offset");
  }
  detail::Outcome<std::span<const std::uint8_t>> valid_impl(std::uint32_t col) const {
    return span_impl<std::uint8_t>(zu_result_col_valid, col, rows(), "zu_result_col_valid");
  }

  detail::Outcome<std::string_view> str_impl(std::uint64_t row, std::uint32_t col) const {
    const char* p = nullptr;
    std::size_t len = 0;
    if (auto e = detail::checked(zu_result_cell_str(h_.get(), row, col, &p, &len),
                                 "zu_result_cell_str")) {
      return std::move(*e);
    }
    return std::string_view(p == nullptr ? "" : p, p == nullptr ? 0 : len);
  }

  detail::Outcome<Value> cell_impl(std::uint64_t row, std::uint32_t col) const {
    const zu_value* v = nullptr;
    if (auto e = detail::checked(zu_result_cell(h_.get(), row, col, &v), "zu_result_cell")) {
      return std::move(*e);
    }
    return Value(v);
  }

  detail::Outcome<ChunkExtent> chunk_impl(std::uint64_t i) const {
    ChunkExtent out;
    if (auto e = detail::checked(zu_result_chunk(h_.get(), i, &out.offset, &out.rows),
                                 "zu_result_chunk")) {
      return std::move(*e);
    }
    return out;
  }

  template <class T, class F>
  detail::Outcome<std::span<const T>> chunk_column_impl(F fn, std::uint64_t chunk,
                                                        std::uint32_t col,
                                                        std::string_view what) const {
    ChunkExtent extent;
    if (auto e = detail::checked(zu_result_chunk(h_.get(), chunk, &extent.offset, &extent.rows),
                                 "zu_result_chunk")) {
      return std::move(*e);
    }
    const T* p = nullptr;
    const zu_status st = fn(h_.get(), chunk, col, &p);
    if (auto e = detail::checked(st, what)) {
      return std::move(*e);
    }
    if (p == nullptr) {
      return std::span<const T>{};
    }
    return std::span<const T>(p, extent.rows);
  }

  detail::Outcome<std::span<const std::int64_t>> chunk_ints_impl(std::uint64_t chunk,
                                                                 std::uint32_t col) const {
    return chunk_column_impl<std::int64_t>(zu_result_chunk_col_i64, chunk, col,
                                           "zu_result_chunk_col_i64");
  }
  detail::Outcome<std::span<const double>> chunk_doubles_impl(std::uint64_t chunk,
                                                              std::uint32_t col) const {
    return chunk_column_impl<double>(zu_result_chunk_col_f64, chunk, col,
                                     "zu_result_chunk_col_f64");
  }
  detail::Outcome<std::span<const std::uint64_t>> chunk_node_offsets_impl(
      std::uint64_t chunk, std::uint32_t col) const {
    return chunk_column_impl<std::uint64_t>(zu_result_chunk_col_node_offset, chunk, col,
                                            "zu_result_chunk_col_node_offset");
  }
  detail::Outcome<std::span<const std::uint8_t>> chunk_valid_impl(std::uint64_t chunk,
                                                                  std::uint32_t col) const {
    return chunk_column_impl<std::uint8_t>(zu_result_chunk_col_valid, chunk, col,
                                           "zu_result_chunk_col_valid");
  }

  inline detail::Outcome<detail::Nothing> arrow_impl(zu_conn* conn, ArrowArrayStream* out,
                                                     std::uint64_t rows_per_batch);

  detail::Handle<zu_result, zu_result_free> h_;
  mutable std::vector<std::string_view> names_;
  mutable bool named_ = false;
};

inline std::uint32_t Row::size() const noexcept {
  return result_ == nullptr ? 0 : result_->cols();
}
inline Type Row::type(std::uint32_t col) const { return result_->type(row_, col); }
inline Type Row::type(std::string_view name) const {
  return result_->type(row_, result_->column(name));
}
inline bool Row::is_null(std::uint32_t col) const { return type(col) == Type::null; }
inline bool Row::is_null(std::string_view name) const { return type(name) == Type::null; }
inline Value Row::operator[](std::uint32_t col) const { return result_->cell(row_, col); }
inline Value Row::operator[](std::string_view name) const {
  return result_->cell(row_, result_->column(name));
}

namespace detail {
template <class T>
struct is_optional : std::false_type {};
template <class T>
struct is_optional<std::optional<T>> : std::true_type {
  using value_type = T;
};
}  // namespace detail

template <class T>
T Row::get(std::uint32_t col) const {
  if constexpr (detail::is_optional<T>::value) {
    using U = typename detail::is_optional<T>::value_type;
    if (is_null(col)) {
      return std::nullopt;
    }
    return get<U>(col);
  } else if constexpr (std::is_same_v<T, Value>) {
    return result_->cell(row_, col);
  } else if constexpr (std::is_same_v<T, std::string_view>) {
    return result_->str(row_, col);
  } else if constexpr (std::is_same_v<T, std::string>) {
    return std::string(result_->str(row_, col));
  } else if constexpr (std::is_same_v<T, std::span<const std::uint8_t>>) {
    return result_->cell(row_, col).as_bytes();
  } else if constexpr (std::is_same_v<T, std::vector<std::uint8_t>>) {
    const auto v = result_->cell(row_, col).as_bytes();
    return std::vector<std::uint8_t>(v.begin(), v.end());
  } else if constexpr (std::is_same_v<T, bool>) {
    return result_->cell(row_, col).as_bool();
  } else if constexpr (std::is_same_v<T, Temporal>) {
    return result_->cell(row_, col).as_temporal();
  } else if constexpr (std::is_same_v<T, Node>) {
    return result_->cell(row_, col).as_node();
  } else if constexpr (std::is_same_v<T, Rel>) {
    return result_->cell(row_, col).as_rel();
  } else if constexpr (std::is_floating_point_v<T>) {
    return static_cast<T>(result_->cell(row_, col).as_double());
  } else if constexpr (std::is_integral_v<T>) {
    const std::int64_t v = result_->cell(row_, col).as_int();
    if constexpr (!std::is_same_v<T, std::int64_t>) {
      /* A narrowing read is a read that can be wrong, and quietly
       * wrapping is how a row count comes back negative. */
      if (v < static_cast<std::int64_t>(std::numeric_limits<T>::min()) ||
          v > static_cast<std::int64_t>(std::numeric_limits<T>::max())) {
        detail::raise(Error::take(Status::misuse, nullptr,
                                  "the value does not fit the type it was read as"));
      }
    }
    return static_cast<T>(v);
  } else {
    static_assert(sizeof(T) == 0, "zu::Row::get does not read that type");
  }
}

template <class T>
T Row::get(std::string_view name) const {
  return get<T>(result_->column(name));
}

/* ---- statements ---- */

/** A prepared statement. Bindings live on it and survive execute, so a
 * loop rebinds only what changed, and binding a name again replaces its
 * value.
 *
 * bind returns the statement, so a call chain reads the way the
 * statement does. */
class Statement {
 public:
  Statement() = default;
  /** Adopts a zu_stmt and closes it at the end of the scope.
   * Connection::prepare hands one over already made. */
  explicit Statement(zu_stmt* s) noexcept : h_(s) {}

  ///@{
  /** The handle underneath and whether there is one, for a caller who
   * has to reach a zu_ call this header does not wrap. Reading through
   * raw() is fine; closing what it points at is not. */
  zu_stmt* raw() const noexcept { return h_.get(); }
  explicit operator bool() const noexcept { return static_cast<bool>(h_); }
  ///@}

  ///@{
  /** Binds one named parameter, and answers the statement so that a
   * chain of them reads the way the statement does. The name is the one
   * in the statement without its dollar. Binding a name the statement
   * does not have is a failure rather than a no-op, and binding one
   * twice replaces the first value. */
  Statement& bind(std::string_view name, std::int64_t v) {
    detail::unwrap_void(bind_int_impl(name, v));
    return *this;
  }
  Statement& bind(std::string_view name, int v) { return bind(name, std::int64_t{v}); }
  Statement& bind(std::string_view name, double v) {
    detail::unwrap_void(bind_double_impl(name, v));
    return *this;
  }
  Statement& bind(std::string_view name, bool v) {
    detail::unwrap_void(bind_bool_impl(name, v));
    return *this;
  }
  Statement& bind(std::string_view name, std::string_view v) {
    detail::unwrap_void(bind_str_impl(name, v));
    return *this;
  }
  Statement& bind(std::string_view name, const char* v) {
    return bind(name, std::string_view(v == nullptr ? "" : v));
  }
  Statement& bind(std::string_view name, Temporal v) {
    detail::unwrap_void(bind_temporal_impl(name, v));
    return *this;
  }
  Statement& bind(std::string_view name, std::nullptr_t) {
    detail::unwrap_void(bind_null_impl(name));
    return *this;
  }
  Statement& bind_null(std::string_view name) {
    detail::unwrap_void(bind_null_impl(name));
    return *this;
  }
  /** A parameter that may or may not be there, which is what a host
   * holding an optional actually has. */
  template <class T>
  Statement& bind(std::string_view name, const std::optional<T>& v) {
    return v ? bind(name, *v) : bind_null(name);
  }
  ///@}

  /** Runs it with what is bound now. The bindings survive, so a loop
   * that rebinds one parameter and executes again does not rebind the
   * rest. */
  Result execute() { return detail::unwrap(execute_impl()); }

#if ZU_HAS_EXPECTED
  ///@{
  /** The expected spelling. Each of these is the call of the same name
   * above it, doing the same work through the same code, and answering
   * a failure rather than throwing it. */
  [[nodiscard]] expected<void> try_bind(std::string_view name, std::int64_t v) {
    return detail::to_expected_void(bind_int_impl(name, v));
  }
  [[nodiscard]] expected<void> try_bind(std::string_view name, double v) {
    return detail::to_expected_void(bind_double_impl(name, v));
  }
  [[nodiscard]] expected<void> try_bind(std::string_view name, bool v) {
    return detail::to_expected_void(bind_bool_impl(name, v));
  }
  [[nodiscard]] expected<void> try_bind(std::string_view name, std::string_view v) {
    return detail::to_expected_void(bind_str_impl(name, v));
  }
  [[nodiscard]] expected<void> try_bind(std::string_view name, Temporal v) {
    return detail::to_expected_void(bind_temporal_impl(name, v));
  }
  [[nodiscard]] expected<void> try_bind_null(std::string_view name) {
    return detail::to_expected_void(bind_null_impl(name));
  }
  [[nodiscard]] expected<Result> try_execute() { return detail::to_expected(execute_impl()); }
  ///@}
#endif

 private:
  detail::Outcome<detail::Nothing> bind_int_impl(std::string_view name, std::int64_t v) {
    if (auto e = detail::checked(zu_bind_i64(h_.get(), name.data(), name.size(), v),
                                 "zu_bind_i64")) {
      return std::move(*e);
    }
    return detail::Nothing{};
  }
  detail::Outcome<detail::Nothing> bind_double_impl(std::string_view name, double v) {
    if (auto e = detail::checked(zu_bind_f64(h_.get(), name.data(), name.size(), v),
                                 "zu_bind_f64")) {
      return std::move(*e);
    }
    return detail::Nothing{};
  }
  detail::Outcome<detail::Nothing> bind_bool_impl(std::string_view name, bool v) {
    if (auto e = detail::checked(zu_bind_bool(h_.get(), name.data(), name.size(), v ? 1 : 0),
                                 "zu_bind_bool")) {
      return std::move(*e);
    }
    return detail::Nothing{};
  }
  detail::Outcome<detail::Nothing> bind_str_impl(std::string_view name, std::string_view v) {
    if (auto e = detail::checked(
            zu_bind_str(h_.get(), name.data(), name.size(), v.data(), v.size()), "zu_bind_str")) {
      return std::move(*e);
    }
    return detail::Nothing{};
  }
  detail::Outcome<detail::Nothing> bind_temporal_impl(std::string_view name, Temporal v) {
    if (auto e = detail::checked(zu_bind_temporal(h_.get(), name.data(), name.size(),
                                                  static_cast<std::int32_t>(v.kind), v.count,
                                                  v.offset),
                                 "zu_bind_temporal")) {
      return std::move(*e);
    }
    return detail::Nothing{};
  }
  detail::Outcome<detail::Nothing> bind_null_impl(std::string_view name) {
    if (auto e = detail::checked(zu_bind_null(h_.get(), name.data(), name.size()),
                                 "zu_bind_null")) {
      return std::move(*e);
    }
    return detail::Nothing{};
  }
  detail::Outcome<Result> execute_impl() {
    zu_result* out = nullptr;
    zu_error* err = nullptr;
    if (auto e = detail::checked(zu_execute(h_.get(), &out, &err), &err, "zu_execute")) {
      return std::move(*e);
    }
    return Result(out);
  }

  detail::Handle<zu_stmt, zu_stmt_close> h_;
};

/* ---- bulk paths ---- */

/** Rows on their way into a table that already exists.
 *
 * A row is written a value at a time, in the order the table declares
 * its columns, and ended by end_row. A refused value ends the row it
 * was in and nothing of that row is kept, so an appender is still
 * usable once the loop is fixed.
 *
 * The destructor writes what is still buffered, because rows that were
 * appended are rows the caller meant to write. What it cannot do is say
 * that the write failed, which is what close is for: a program that
 * cares whether the rows went in calls close and reads the count, and
 * one that wants them gone calls discard. */
class Appender {
 public:
  Appender() = default;
  /** Adopts a zu_appender and closes it at the end of the scope.
   * Connection::appender hands one over already made. */
  explicit Appender(zu_appender* a) noexcept : h_(a) {}

  ///@{
  /** The handle underneath and whether there is one, for a caller who
   * has to reach a zu_ call this header does not wrap. Reading through
   * raw() is fine; closing what it points at is not. */
  zu_appender* raw() const noexcept { return h_.get(); }
  explicit operator bool() const noexcept { return static_cast<bool>(h_); }
  ///@}

  ///@{
  /** One value into the row being written, in the order the table
   * declares its columns, and answering the appender so that a chain
   * reads as a row. A value the column will not take is a failure that
   * ends the row it was in, and nothing of that row is kept. */
  Appender& append(bool v) {
    detail::unwrap_void(append_bool_impl(v));
    return *this;
  }
  Appender& append(std::int64_t v) {
    detail::unwrap_void(append_int_impl(v));
    return *this;
  }
  Appender& append(int v) { return append(std::int64_t{v}); }
  Appender& append(double v) {
    detail::unwrap_void(append_double_impl(v));
    return *this;
  }
  Appender& append(std::string_view v) {
    detail::unwrap_void(append_str_impl(v));
    return *this;
  }
  Appender& append(const char* v) { return append(std::string_view(v == nullptr ? "" : v)); }
  Appender& append(std::span<const std::uint8_t> v) {
    detail::unwrap_void(append_bytes_impl(v));
    return *this;
  }
  Appender& append(Temporal v) {
    detail::unwrap_void(append_temporal_impl(v));
    return *this;
  }
  ///@}

  /** Ends the row being written, which is what makes it a row. */
  Appender& end_row() {
    detail::unwrap_void(end_row_impl());
    return *this;
  }

  /** One whole row, which is the shape most callers want: the values in
   * the order the table declares them, and the end of the row. */
  template <class... Ts>
  Appender& row(const Ts&... values) {
    (append(values), ...);
    return end_row();
  }

  /** One commit. When it returns the rows are durable and every later
   * statement sees them, and before it returns nothing sees anything. A
   * flush with nothing buffered touches no file. */
  void flush() { detail::unwrap_void(flush_impl()); }

  ///@{
  /** How many whole rows are waiting on this appender and how many it
   * has written so far. The two together are every row it has been
   * given; a row part way through is in neither. */
  std::uint64_t buffered() const { return detail::unwrap(count_impl(zu_appender_buffered)); }
  std::uint64_t committed() const { return detail::unwrap(count_impl(zu_appender_committed)); }
  ///@}

  ///@{
  /** The shape of the table this writes into, which is what a program
   * building rows from a map of names needs in order to put them in the
   * order append expects. */
  std::uint32_t cols() const { return detail::unwrap(cols_impl()); }

  std::string_view col_name(std::uint32_t col) const {
    return detail::unwrap(col_name_impl(col));
  }
  ///@}

  /** Throws away what is buffered and says how many rows that was. Rows
   * an earlier flush committed are committed and this does not reach
   * them. */
  std::uint64_t discard() { return detail::unwrap(discard_impl()); }

  /** Flushes what is left and spends the appender, answering the rows it
   * committed in all. Closing twice writes nothing the second time, so
   * a cleanup path may close what the load already did. */
  std::uint64_t close() { return detail::unwrap(close_impl()); }

#if ZU_HAS_EXPECTED
  ///@{
  /** The expected spelling. Each of these is the call of the same name
   * above it, doing the same work through the same code, and answering
   * a failure rather than throwing it. */
  [[nodiscard]] expected<void> try_append(bool v) { return detail::to_expected_void(append_bool_impl(v)); }
  [[nodiscard]] expected<void> try_append(std::int64_t v) { return detail::to_expected_void(append_int_impl(v)); }
  [[nodiscard]] expected<void> try_append(double v) { return detail::to_expected_void(append_double_impl(v)); }
  [[nodiscard]] expected<void> try_append(std::string_view v) {
    return detail::to_expected_void(append_str_impl(v));
  }
  [[nodiscard]] expected<void> try_append(std::span<const std::uint8_t> v) {
    return detail::to_expected_void(append_bytes_impl(v));
  }
  [[nodiscard]] expected<void> try_append(Temporal v) {
    return detail::to_expected_void(append_temporal_impl(v));
  }
  [[nodiscard]] expected<std::string_view> try_col_name(std::uint32_t col) const {
    return detail::to_expected(col_name_impl(col));
  }
  [[nodiscard]] expected<void> try_end_row() { return detail::to_expected_void(end_row_impl()); }
  [[nodiscard]] expected<void> try_flush() { return detail::to_expected_void(flush_impl()); }
  [[nodiscard]] expected<std::uint64_t> try_discard() { return detail::to_expected(discard_impl()); }
  [[nodiscard]] expected<std::uint64_t> try_close() { return detail::to_expected(close_impl()); }
  ///@}
#endif

 private:
  detail::Outcome<detail::Nothing> append_bool_impl(bool v) {
    zu_error* err = nullptr;
    if (auto e = detail::checked(zu_append_bool(h_.get(), v ? 1 : 0, &err), &err,
                                 "zu_append_bool")) {
      return std::move(*e);
    }
    return detail::Nothing{};
  }
  detail::Outcome<detail::Nothing> append_int_impl(std::int64_t v) {
    zu_error* err = nullptr;
    if (auto e = detail::checked(zu_append_i64(h_.get(), v, &err), &err, "zu_append_i64")) {
      return std::move(*e);
    }
    return detail::Nothing{};
  }
  detail::Outcome<detail::Nothing> append_double_impl(double v) {
    zu_error* err = nullptr;
    if (auto e = detail::checked(zu_append_f64(h_.get(), v, &err), &err, "zu_append_f64")) {
      return std::move(*e);
    }
    return detail::Nothing{};
  }
  detail::Outcome<detail::Nothing> append_str_impl(std::string_view v) {
    zu_error* err = nullptr;
    if (auto e = detail::checked(zu_append_str(h_.get(), v.data(), v.size(), &err), &err,
                                 "zu_append_str")) {
      return std::move(*e);
    }
    return detail::Nothing{};
  }
  detail::Outcome<detail::Nothing> append_bytes_impl(std::span<const std::uint8_t> v) {
    zu_error* err = nullptr;
    if (auto e = detail::checked(zu_append_bytes(h_.get(), v.data(), v.size(), &err), &err,
                                 "zu_append_bytes")) {
      return std::move(*e);
    }
    return detail::Nothing{};
  }
  detail::Outcome<detail::Nothing> append_temporal_impl(Temporal v) {
    zu_error* err = nullptr;
    if (auto e = detail::checked(
            zu_append_temporal(h_.get(), static_cast<std::int32_t>(v.kind), v.count, &err), &err,
            "zu_append_temporal")) {
      return std::move(*e);
    }
    return detail::Nothing{};
  }
  detail::Outcome<detail::Nothing> end_row_impl() {
    zu_error* err = nullptr;
    if (auto e = detail::checked(zu_append_end_row(h_.get(), &err), &err, "zu_append_end_row")) {
      return std::move(*e);
    }
    return detail::Nothing{};
  }
  detail::Outcome<detail::Nothing> flush_impl() {
    zu_error* err = nullptr;
    if (auto e = detail::checked(zu_appender_flush(h_.get(), &err), &err, "zu_appender_flush")) {
      return std::move(*e);
    }
    return detail::Nothing{};
  }
  detail::Outcome<std::string_view> col_name_impl(std::uint32_t col) const {
    std::size_t len = 0;
    const char* p = zu_appender_col_name(h_.get(), col, &len);
    if (p == nullptr) {
      return Error::take(Status::misuse, nullptr, "zu_appender_col_name: no such column");
    }
    return std::string_view(p, len);
  }
  detail::Outcome<std::uint32_t> cols_impl() const {
    std::uint32_t out = 0;
    if (auto e = detail::checked(zu_appender_cols(h_.get(), &out), "zu_appender_cols")) {
      return std::move(*e);
    }
    return out;
  }
  detail::Outcome<std::uint64_t> count_impl(zu_status (*fn)(zu_appender*, std::uint64_t*)) const {
    std::uint64_t out = 0;
    if (auto e = detail::checked(fn(h_.get(), &out), "zu_appender count")) {
      return std::move(*e);
    }
    return out;
  }
  detail::Outcome<std::uint64_t> discard_impl() {
    std::uint64_t out = 0;
    if (auto e = detail::checked(zu_appender_discard(h_.get(), &out), "zu_appender_discard")) {
      return std::move(*e);
    }
    return out;
  }
  detail::Outcome<std::uint64_t> close_impl() {
    std::uint64_t out = 0;
    zu_error* err = nullptr;
    if (auto e = detail::checked(zu_appender_close(h_.get(), &out, &err), &err,
                                 "zu_appender_close")) {
      return std::move(*e);
    }
    return out;
  }

  detail::Handle<zu_appender, zu_appender_free> h_;
};

/** How values get into a database that does not exist yet.
 *
 * Columnar, for the reason a result is: one call per column, not one
 * per cell. The order is fixed, create then table then columns and
 * edges then finish, and nothing reaches the file until finish, so a
 * load either happened or did not.
 *
 * The loader copies every array it is given, so a caller may free or
 * reuse its own buffers as soon as a call returns. */
class Loader {
 public:
  Loader() = default;
  /** Adopts a zu_loader and frees it at the end of the scope. create is
   * how one is normally got. */
  explicit Loader(zu_loader* l) noexcept : h_(l) {}

  /** Fails if the path exists, which is what a bulk load is: it builds a
   * database rather than adding to one. */
  [[nodiscard]] static Loader create(std::string_view path) { return detail::unwrap(create_impl(path)); }

  ///@{
  /** The handle underneath and whether there is one, for a caller who
   * has to reach a zu_ call this header does not wrap. Reading through
   * raw() is fine; freeing what it points at is not. */
  zu_loader* raw() const noexcept { return h_.get(); }
  explicit operator bool() const noexcept { return static_cast<bool>(h_); }
  ///@}

  /** The rows count is given rather than counted from the first column,
   * so a column with a value missing is an error and not a shorter
   * table. One table per loader. */
  Loader& table(std::string_view nodes, std::string_view edges, std::uint64_t rows) {
    detail::unwrap_void(table_impl(nodes, edges, rows));
    return *this;
  }

  /** Edges as the row each starts at and the row it ends at. Appends, so
   * call it as often as you like; the loader sorts and deduplicates at
   * finish. */
  Loader& edges(std::span<const std::uint32_t> from, std::span<const std::uint32_t> to) {
    detail::unwrap_void(edges_impl(from, to));
    return *this;
  }

  /** One column a call, named for what it holds rather than overloaded
   * on the element type. A column of flags and a column of small
   * integers are the same array to C++ and different tables to the
   * engine, and a name is a better place to settle that than an
   * overload nobody reads. */
  Loader& ints(std::string_view name, std::span<const std::int64_t> values) {
    detail::unwrap_void(col_ints_impl(name, values));
    return *this;
  }
  /** The same, for a column of floats. */
  Loader& doubles(std::string_view name, std::span<const double> values) {
    detail::unwrap_void(col_doubles_impl(name, values));
    return *this;
  }
  /** One int32 a row, any nonzero value true, which is what zu.h takes
   * and what a host holding a column of flags has. */
  Loader& bools(std::string_view name, std::span<const std::int32_t> values) {
    detail::unwrap_void(col_bools_impl(name, values));
    return *this;
  }
  /** Any range of anything a string_view can be made from, which is a
   * vector of std::string as readily as one of views. */
  template <detail::StringRange R>
  Loader& strings(std::string_view name, const R& values) {
    const auto views = detail::to_views(values);
    detail::unwrap_void(col_strs_impl(name, views));
    return *this;
  }
  /** A column of temporals as the counts alone, with the kind said once
   * for the whole column rather than carried on every value. The unit
   * follows the kind, the way Temporal::count does. */
  Loader& temporals(std::string_view name, TemporalKind kind,
                    std::span<const std::int64_t> values) {
    detail::unwrap_void(col_temporal_impl(name, kind, values));
    return *this;
  }

  /** Writes it all. The database is on disk when this returns, and
   * opening the same path reads it. */
  void finish() { detail::unwrap_void(finish_impl()); }

#if ZU_HAS_EXPECTED
  ///@{
  /** The expected spelling. Each of these is the call of the same name
   * above it, doing the same work through the same code, and answering
   * a failure rather than throwing it. */
  [[nodiscard]] static expected<Loader> try_create(std::string_view path) {
    return detail::to_expected(create_impl(path));
  }
  [[nodiscard]] expected<void> try_table(std::string_view nodes, std::string_view edges, std::uint64_t rows) {
    return detail::to_expected_void(table_impl(nodes, edges, rows));
  }
  [[nodiscard]] expected<void> try_edges(std::span<const std::uint32_t> from, std::span<const std::uint32_t> to) {
    return detail::to_expected_void(edges_impl(from, to));
  }
  [[nodiscard]] expected<void> try_ints(std::string_view name, std::span<const std::int64_t> values) {
    return detail::to_expected_void(col_ints_impl(name, values));
  }
  [[nodiscard]] expected<void> try_doubles(std::string_view name, std::span<const double> values) {
    return detail::to_expected_void(col_doubles_impl(name, values));
  }
  [[nodiscard]] expected<void> try_bools(std::string_view name, std::span<const std::int32_t> values) {
    return detail::to_expected_void(col_bools_impl(name, values));
  }
  template <detail::StringRange R>
  [[nodiscard]] expected<void> try_strings(std::string_view name, const R& values) {
    return detail::to_expected_void(col_strs_impl(name, detail::to_views(values)));
  }
  [[nodiscard]] expected<void> try_temporals(std::string_view name, TemporalKind kind,
                               std::span<const std::int64_t> values) {
    return detail::to_expected_void(col_temporal_impl(name, kind, values));
  }
  [[nodiscard]] expected<void> try_finish() { return detail::to_expected_void(finish_impl()); }
  ///@}
#endif

 private:
  static detail::Outcome<Loader> create_impl(std::string_view path) {
    zu_loader* out = nullptr;
    zu_error* err = nullptr;
    if (auto e = detail::checked(zu_loader_create(path.data(), path.size(), &out, &err), &err,
                                 "zu_loader_create")) {
      return std::move(*e);
    }
    return Loader(out);
  }
  detail::Outcome<detail::Nothing> table_impl(std::string_view nodes, std::string_view edges,
                                              std::uint64_t rows) {
    zu_error* err = nullptr;
    if (auto e = detail::checked(zu_loader_table(h_.get(), nodes.data(), nodes.size(), edges.data(),
                                                 edges.size(), rows, &err),
                                 &err, "zu_loader_table")) {
      return std::move(*e);
    }
    return detail::Nothing{};
  }
  detail::Outcome<detail::Nothing> edges_impl(std::span<const std::uint32_t> from,
                                              std::span<const std::uint32_t> to) {
    if (from.size() != to.size()) {
      return Error::take(Status::misuse, nullptr,
                         "an edge starts somewhere and ends somewhere, so the two arrays are "
                         "the same length");
    }
    zu_error* err = nullptr;
    if (auto e = detail::checked(
            zu_loader_edges(h_.get(), from.data(), to.data(), from.size(), &err), &err,
            "zu_loader_edges")) {
      return std::move(*e);
    }
    return detail::Nothing{};
  }
  detail::Outcome<detail::Nothing> col_ints_impl(std::string_view name,
                                                 std::span<const std::int64_t> values) {
    zu_error* err = nullptr;
    if (auto e = detail::checked(zu_loader_col_i64(h_.get(), name.data(), name.size(),
                                                   values.data(), values.size(), &err),
                                 &err, "zu_loader_col_i64")) {
      return std::move(*e);
    }
    return detail::Nothing{};
  }
  detail::Outcome<detail::Nothing> col_doubles_impl(std::string_view name,
                                                    std::span<const double> values) {
    zu_error* err = nullptr;
    if (auto e = detail::checked(zu_loader_col_f64(h_.get(), name.data(), name.size(),
                                                   values.data(), values.size(), &err),
                                 &err, "zu_loader_col_f64")) {
      return std::move(*e);
    }
    return detail::Nothing{};
  }
  detail::Outcome<detail::Nothing> col_bools_impl(std::string_view name,
                                                  std::span<const std::int32_t> values) {
    zu_error* err = nullptr;
    if (auto e = detail::checked(zu_loader_col_bool(h_.get(), name.data(), name.size(),
                                                    values.data(), values.size(), &err),
                                 &err, "zu_loader_col_bool")) {
      return std::move(*e);
    }
    return detail::Nothing{};
  }
  detail::Outcome<detail::Nothing> col_strs_impl(std::string_view name,
                                                 std::span<const std::string_view> values) {
    /* Two arrays because zu.h takes two, and building them here is what
     * lets a caller pass the string_views it already had. */
    std::vector<const char*> ptrs;
    std::vector<std::size_t> lens;
    ptrs.reserve(values.size());
    lens.reserve(values.size());
    for (const std::string_view v : values) {
      ptrs.push_back(v.data());
      lens.push_back(v.size());
    }
    zu_error* err = nullptr;
    if (auto e = detail::checked(zu_loader_col_str(h_.get(), name.data(), name.size(), ptrs.data(),
                                                   lens.data(), values.size(), &err),
                                 &err, "zu_loader_col_str")) {
      return std::move(*e);
    }
    return detail::Nothing{};
  }
  detail::Outcome<detail::Nothing> col_temporal_impl(std::string_view name, TemporalKind kind,
                                                     std::span<const std::int64_t> values) {
    zu_error* err = nullptr;
    if (auto e = detail::checked(
            zu_loader_col_temporal(h_.get(), name.data(), name.size(),
                                   static_cast<std::int32_t>(kind), values.data(), values.size(),
                                   &err),
            &err, "zu_loader_col_temporal")) {
      return std::move(*e);
    }
    return detail::Nothing{};
  }
  detail::Outcome<detail::Nothing> finish_impl() {
    zu_error* err = nullptr;
    if (auto e = detail::checked(zu_loader_finish(h_.get(), &err), &err, "zu_loader_finish")) {
      return std::move(*e);
    }
    return detail::Nothing{};
  }

  detail::Handle<zu_loader, zu_loader_free> h_;
};

/** Columns of the caller's own memory, named as a table of a connection
 * and read where they lie.
 *
 * Nothing is copied, at registration or at read. What the engine asks
 * for is that each buffer stays where it is, unwritten and unfreed,
 * until the release callback runs, which is after the last statement
 * reading the frame ends and not at the unregister that preceded it.
 *
 * The column calls take spans, so the count comes from the span and a
 * caller cannot pass a length that does not match the pointer. The
 * width and the signedness of an integer column come from its type,
 * which is the one thing C++ knows here that C did not. */
class Frame {
 public:
  Frame() = default;
  /** Adopts a zu_frame and frees it at the end of the scope. create is
   * how one is normally got. */
  explicit Frame(zu_frame* f) noexcept : h_(f) {}

  /** A frame over buffers the caller keeps alive itself. */
  [[nodiscard]] static Frame create(std::string_view name, std::uint64_t rows) {
    return detail::unwrap(create_impl(name, rows, {}));
  }
  /** A frame that says when the engine has finished with it. The
   * callback runs once, on a thread of the library's, and is where a
   * host that has to take a lock to let go of what it passed takes
   * it. */
  [[nodiscard]] static Frame create(std::string_view name, std::uint64_t rows, std::function<void()> release) {
    return detail::unwrap(create_impl(name, rows, std::move(release)));
  }
  /** The same, keeping something alive rather than running something: a
   * shared_ptr to whatever owns the buffers, dropped when the engine is
   * done. */
  [[nodiscard]] static Frame create(std::string_view name, std::uint64_t rows, std::shared_ptr<void> keepalive) {
    return create(name, rows, [held = std::move(keepalive)]() mutable { held.reset(); });
  }

  ///@{
  /** The handle underneath and whether there is one, for a caller who
   * has to reach a zu_ call this header does not wrap. Reading through
   * raw() is fine; freeing what it points at is not. */
  zu_frame* raw() const noexcept { return h_.get(); }
  explicit operator bool() const noexcept { return static_cast<bool>(h_); }
  ///@}

  /** An integer column, of any width this engine can widen from. Sixty
   * four signed bits at scale 1 is the lane it reads natively and the
   * column that costs nothing at all.
   *
   * scale is what one value is multiplied by to reach the unit its
   * meaning counts in: 1 for an integer and a date, 1000 for the
   * microseconds Arrow keeps a time in. */
  template <detail::NumericRange R>
  Frame& column(std::string_view name, const R& values, std::int64_t scale = 1,
                TemporalKind temporal = TemporalKind::plain) {
    detail::unwrap_void(numeric_impl(name, values, scale, temporal));
    return *this;
  }

  /** One bit a row, low bit of the first byte first, which is Arrow's
   * bitmap and this engine's alike. The count is the rows rather than
   * the bytes, because a bitmap of ten rows is two bytes and neither
   * number can be worked out from the other. */
  Frame& bools(std::string_view name, std::span<const std::uint8_t> bitmap, std::uint64_t count) {
    detail::unwrap_void(col_bool_impl(name, bitmap.data(), count));
    return *this;
  }

  ///@{
  /** Arrow's Utf8 with 32-bit offsets and its LargeUtf8 with 64-bit
   * ones. There are count + 1 offsets and the last is how much of data
   * is used. */
  Frame& strings(std::string_view name, std::span<const std::int32_t> offsets,
                 std::span<const char> data) {
    detail::unwrap_void(col_str_impl(name, offsets.data(), 0, data.data(), data.size(),
                                     offsets.empty() ? 0 : offsets.size() - 1));
    return *this;
  }
  Frame& strings(std::string_view name, std::span<const std::int64_t> offsets,
                 std::span<const char> data) {
    detail::unwrap_void(col_str_impl(name, offsets.data(), 1, data.data(), data.size(),
                                     offsets.empty() ? 0 : offsets.size() - 1));
    return *this;
  }
  ///@}

  /** Arrow's Utf8View: sixteen bytes a row over the buffers named by the
   * two arrays. A short string in that layout is already this engine's
   * own view, byte for byte. */
  Frame& views(std::string_view name, std::span<const std::byte> views_buffer,
               std::span<const void* const> buffers, std::span<const std::size_t> lens,
               std::uint64_t count) {
    detail::unwrap_void(col_view_impl(name, views_buffer.data(), buffers.data(), lens.data(),
                                      buffers.size(), count));
    return *this;
  }

#if ZU_HAS_EXPECTED
  ///@{
  /** The expected spelling. Each of these is the call of the same name
   * above it, doing the same work through the same code, and answering
   * a failure rather than throwing it. */
  [[nodiscard]] static expected<Frame> try_create(std::string_view name, std::uint64_t rows,
                                    std::function<void()> release = {}) {
    return detail::to_expected(create_impl(name, rows, std::move(release)));
  }
  template <detail::NumericRange R>
  [[nodiscard]] expected<void> try_column(std::string_view name, const R& values, std::int64_t scale = 1,
                            TemporalKind temporal = TemporalKind::plain) {
    return detail::to_expected_void(numeric_impl(name, values, scale, temporal));
  }
  [[nodiscard]] expected<void> try_bools(std::string_view name, std::span<const std::uint8_t> bitmap,
                           std::uint64_t count) {
    return detail::to_expected_void(col_bool_impl(name, bitmap.data(), count));
  }
  [[nodiscard]] expected<void> try_strings(std::string_view name, std::span<const std::int32_t> offsets,
                             std::span<const char> data) {
    return detail::to_expected_void(col_str_impl(name, offsets.data(), 0, data.data(), data.size(),
                                                 offsets.empty() ? 0 : offsets.size() - 1));
  }
  ///@}
#endif

 private:
  /** The one place the width and the signedness of a column are worked
   * out, which is a thing C++ knows here and C did not: the caller
   * passes the array it already had and the type of its elements says
   * what to tell the engine. A float column takes no scale and no
   * temporal, and passing either is a caller's mistake worth naming
   * rather than ignoring. */
  template <detail::NumericRange R>
  detail::Outcome<detail::Nothing> numeric_impl(std::string_view name, const R& values,
                                                std::int64_t scale, TemporalKind temporal) {
    using T = std::ranges::range_value_t<R>;
    const void* data = std::ranges::data(values);
    const auto count = static_cast<std::uint64_t>(std::ranges::size(values));
    if constexpr (std::floating_point<T>) {
      if (scale != 1 || temporal != TemporalKind::plain) {
        return Error::take(Status::misuse, nullptr,
                           "a column of floats carries no scale and no temporal meaning");
      }
      return col_float_impl(name, data, count, static_cast<std::int32_t>(sizeof(T) * 8));
    } else {
      return col_int_impl(name, data, count, static_cast<std::int32_t>(sizeof(T) * 8),
                          std::is_signed_v<T> ? 1 : 0, scale, temporal);
    }
  }

  static void release_trampoline(void* owner) noexcept {
    auto* fn = static_cast<std::function<void()>*>(owner);
    if (fn != nullptr) {
      if (*fn) {
        (*fn)();
      }
      delete fn;
    }
  }

  static detail::Outcome<Frame> create_impl(std::string_view name, std::uint64_t rows,
                                            std::function<void()> release) {
    std::unique_ptr<std::function<void()>> owner;
    if (release) {
      owner = std::make_unique<std::function<void()>>(std::move(release));
    }
    zu_frame* out = nullptr;
    zu_error* err = nullptr;
    const zu_status st =
        zu_frame_new(name.data(), name.size(), rows, owner.get(),
                     owner ? &Frame::release_trampoline : nullptr, &out, &err);
    if (auto e = detail::checked(st, &err, "zu_frame_new")) {
      /* The callback never runs for a frame that was never made, so
       * what was allocated for it is freed here rather than leaked. */
      return std::move(*e);
    }
    owner.release();
    return Frame(out);
  }

  detail::Outcome<detail::Nothing> col_int_impl(std::string_view name, const void* values,
                                                std::uint64_t count, std::int32_t bits,
                                                std::int32_t is_signed, std::int64_t scale,
                                                TemporalKind temporal) {
    zu_error* err = nullptr;
    if (auto e = detail::checked(
            zu_frame_col_int(h_.get(), name.data(), name.size(), values, count, bits, is_signed,
                             scale, static_cast<std::int32_t>(temporal), &err),
            &err, "zu_frame_col_int")) {
      return std::move(*e);
    }
    return detail::Nothing{};
  }
  detail::Outcome<detail::Nothing> col_float_impl(std::string_view name, const void* values,
                                                  std::uint64_t count, std::int32_t bits) {
    zu_error* err = nullptr;
    if (auto e = detail::checked(
            zu_frame_col_float(h_.get(), name.data(), name.size(), values, count, bits, &err), &err,
            "zu_frame_col_float")) {
      return std::move(*e);
    }
    return detail::Nothing{};
  }
  detail::Outcome<detail::Nothing> col_bool_impl(std::string_view name, const void* bitmap,
                                                 std::uint64_t count) {
    zu_error* err = nullptr;
    if (auto e = detail::checked(
            zu_frame_col_bool(h_.get(), name.data(), name.size(), bitmap, count, &err), &err,
            "zu_frame_col_bool")) {
      return std::move(*e);
    }
    return detail::Nothing{};
  }
  detail::Outcome<detail::Nothing> col_str_impl(std::string_view name, const void* offsets,
                                                std::int32_t wide, const void* data,
                                                std::size_t data_len, std::uint64_t count) {
    zu_error* err = nullptr;
    if (auto e = detail::checked(zu_frame_col_str(h_.get(), name.data(), name.size(), offsets, wide,
                                                  data, data_len, count, &err),
                                 &err, "zu_frame_col_str")) {
      return std::move(*e);
    }
    return detail::Nothing{};
  }
  detail::Outcome<detail::Nothing> col_view_impl(std::string_view name, const void* views,
                                                 const void* const* data, const std::size_t* lens,
                                                 std::size_t buffers, std::uint64_t count) {
    zu_error* err = nullptr;
    if (auto e = detail::checked(zu_frame_col_view(h_.get(), name.data(), name.size(), views, data,
                                                   lens, buffers, count, &err),
                                 &err, "zu_frame_col_view")) {
      return std::move(*e);
    }
    return detail::Nothing{};
  }

  detail::Handle<zu_frame, zu_frame_free> h_;
};

/* ---- connections ---- */

class Transaction;

/** How a database is opened. Zero means the default in every field, so a
 * default-built Config opens the same database as none at all. */
class Config {
 public:
  Config() { zu_config_init(&c_); }

  /** How much the executor may hold at once, in bytes. 0 is no limit.
   * A statement that would need more fails rather than being killed by
   * the operating system. */
  Config& memory_limit(std::size_t bytes) {
    c_.memory_limit = bytes;
    return *this;
  }
  /** Query workers. 0 lets the executor pick, 1 is sequential. */
  Config& threads(std::size_t n) {
    c_.threads = n;
    return *this;
  }
  /** Refuses every write on connections opened with it, which is what a
   * reporting process wants: the refusal comes from the engine rather
   * than from a rule somebody has to remember. */
  Config& read_only(bool yes = true) {
    c_.read_only = yes ? 1 : 0;
    return *this;
  }

  /** One option by name, which is what a program forwarding a user's
   * option map has. The keys are memory_limit, threads and read_only,
   * and an unrecognized one is refused and named. */
  Config& set(std::string_view key, std::string_view value) {
    detail::unwrap_void(set_impl(key, value));
    return *this;
  }

#if ZU_HAS_EXPECTED
  ///@{
  /** The expected spelling. Each of these is the call of the same name
   * above it, doing the same work through the same code, and answering
   * a failure rather than throwing it. */
  [[nodiscard]] expected<void> try_set(std::string_view key, std::string_view value) {
    return detail::to_expected_void(set_impl(key, value));
  }
  ///@}
#endif

  /** The zu_config underneath, for a caller who has to reach a zu_ call
   * this header does not wrap. It is a member rather than a handle, so
   * it lives as long as the Config does and there is nothing to free. */
  const zu_config* raw() const noexcept { return &c_; }

 private:
  detail::Outcome<detail::Nothing> set_impl(std::string_view key, std::string_view value) {
    zu_error* err = nullptr;
    if (auto e = detail::checked(
            zu_config_set(&c_, key.data(), key.size(), value.data(), value.size(), &err), &err,
            "zu_config_set")) {
      return std::move(*e);
    }
    return detail::Nothing{};
  }

  zu_config c_{};
};

/** A path and a configuration that have been checked against a real
 * file. It holds no descriptor and no cache, so it is thread-safe and
 * shareable, and a host that queries from four threads opens one of
 * these and connects four times.
 *
 * Closing it does not close the connections opened from it: each holds
 * its own file handle, and this releases only the path and the
 * configuration. */
class Database {
 public:
  Database() = default;
  /** Adopts a zu_database and closes it at the end of the scope. open,
   * create and memory are how one is normally got. */
  explicit Database(zu_database* db) noexcept : h_(db) {}

  /** Opens what is at the path, which has to be there. */
  [[nodiscard]] static Database open(std::string_view path, const Config& cfg = Config{}) {
    return detail::unwrap(open_impl(path, cfg));
  }
  /** The path must not exist. A create that opened what it found there
   * would be the call that quietly writes into somebody else's data. */
  [[nodiscard]] static Database create(std::string_view path, const Config& cfg = Config{}) {
    return detail::unwrap(create_impl(path, cfg));
  }
  /** A database that never touches the filesystem. Every call makes one
   * of its own: two connections on one handle are two views of one
   * graph, and two handles share nothing. */
  [[nodiscard]] static Database memory(const Config& cfg = Config{}) { return detail::unwrap(memory_impl(cfg)); }

  ///@{
  /** The handle underneath and whether there is one, for a caller who
   * has to reach a zu_ call this header does not wrap. Reading through
   * raw() is fine; closing what it points at is not. */
  zu_database* raw() const noexcept { return h_.get(); }
  explicit operator bool() const noexcept { return static_cast<bool>(h_); }
  ///@}

  /** True for one made by memory, which is the case where path is a
   * name rather than something to open. */
  bool is_memory() const { return zu_database_is_memory(h_.get()) == ZU_OK; }

  /** What this process calls the database, which for one in memory is a
   * name rather than a path: it is what an error message needs and not
   * something to open. */
  std::string_view path() const { return detail::unwrap(path_impl()); }

  /** A connection of its own, which is what a thread needs. The
   * database may be shared between threads and a connection may not, so
   * four threads call this four times. */
  inline Connection connect() const;

#if ZU_HAS_EXPECTED
  ///@{
  /** The expected spelling. Each of these is the call of the same name
   * above it, doing the same work through the same code, and answering
   * a failure rather than throwing it. */
  [[nodiscard]] static expected<Database> try_open(std::string_view path, const Config& cfg = Config{}) {
    return detail::to_expected(open_impl(path, cfg));
  }
  [[nodiscard]] static expected<Database> try_create(std::string_view path, const Config& cfg = Config{}) {
    return detail::to_expected(create_impl(path, cfg));
  }
  [[nodiscard]] static expected<Database> try_memory(const Config& cfg = Config{}) {
    return detail::to_expected(memory_impl(cfg));
  }
  [[nodiscard]] expected<std::string_view> try_path() const { return detail::to_expected(path_impl()); }
  [[nodiscard]] inline expected<Connection> try_connect() const;
  ///@}
#endif

 private:
  friend class Connection;

  static detail::Outcome<Database> open_impl(std::string_view path, const Config& cfg) {
    zu_database* out = nullptr;
    zu_error* err = nullptr;
    if (auto e = detail::checked(
            zu_database_open(path.data(), path.size(), cfg.raw(), &out, &err), &err,
            "zu_database_open")) {
      return std::move(*e);
    }
    return Database(out);
  }
  static detail::Outcome<Database> create_impl(std::string_view path, const Config& cfg) {
    zu_database* out = nullptr;
    zu_error* err = nullptr;
    if (auto e = detail::checked(
            zu_database_create(path.data(), path.size(), cfg.raw(), &out, &err), &err,
            "zu_database_create")) {
      return std::move(*e);
    }
    return Database(out);
  }
  static detail::Outcome<Database> memory_impl(const Config& cfg) {
    zu_database* out = nullptr;
    zu_error* err = nullptr;
    if (auto e = detail::checked(zu_database_memory(cfg.raw(), &out, &err), &err,
                                 "zu_database_memory")) {
      return std::move(*e);
    }
    return Database(out);
  }
  detail::Outcome<std::string_view> path_impl() const {
    const char* p = nullptr;
    std::size_t len = 0;
    if (auto e = detail::checked(zu_database_path(h_.get(), &p, &len), "zu_database_path")) {
      return std::move(*e);
    }
    return std::string_view(p == nullptr ? "" : p, p == nullptr ? 0 : len);
  }
  inline detail::Outcome<Connection> connect_impl() const;

  detail::Handle<zu_database, zu_database_close> h_;
};

/** The state that cannot be shared: a file handle, the caches, and the
 * plans compiled against a catalog.
 *
 * A connection may move between threads but must not be used from two
 * at once, and a call that finds one already in use answers rather than
 * corrupting a cache, which arrives here as ConcurrentError. The one
 * exception is interrupt, which is meant to be called from another
 * thread while a statement runs. */
class Connection {
 public:
  Connection() = default;
  /** Adopts a zu_conn and closes it at the end of the scope. open,
   * create, memory and Database::connect are how one is normally got. */
  explicit Connection(zu_conn* c) noexcept : h_(c) {}

  ///@{
  /** Moves, and does not copy. A connection is one thread's, so a copy
   * would be the shape that lets two threads share one, and the moved
   * from connection is empty rather than closed: destroying it is fine
   * and using it throws. The assignment clears the progress callback
   * this connection had first, because that callback is registered with
   * the library and outlives the handle otherwise. */
  Connection(Connection&&) noexcept = default;
  Connection& operator=(Connection&& o) noexcept {
    if (this != &o) {
      clear_progress_quietly();
      h_ = std::move(o.h_);
      watch_ = std::move(o.watch_);
    }
    return *this;
  }
  ///@}
  ~Connection() { clear_progress_quietly(); }

  ///@{
  /** One database with the default configuration, one connection on it,
   * and nothing else to keep track of. open wants the path to be there,
   * create wants it not to be, and memory touches no file at all. A
   * program that wants a configuration, or more than one connection,
   * opens a Database instead. */
  [[nodiscard]] static Connection open(std::string_view path) { return detail::unwrap(open_impl(path)); }
  [[nodiscard]] static Connection create(std::string_view path) { return detail::unwrap(create_impl(path)); }
  [[nodiscard]] static Connection memory() { return detail::unwrap(memory_impl()); }
  ///@}

  ///@{
  /** The handle underneath and whether there is one, for a caller who
   * has to reach a zu_ call this header does not wrap. Reading through
   * raw() is fine; closing what it points at is not. */
  zu_conn* raw() const noexcept { return h_.get(); }
  explicit operator bool() const noexcept { return static_cast<bool>(h_); }
  ///@}

  /** A second connection on the database this one is already on, made
   * without a path. The switches and the read-only setting come across;
   * the caches, the interrupt and the transaction do not, because those
   * are what makes it a connection of its own. */
  Connection duplicate() { return detail::unwrap(duplicate_impl()); }

  /** Runs one statement and answers the whole of what it read. There
   * are no parameters here on purpose: a statement with a value in it
   * is built by prepare and bind rather than by pasting text. */
  Result query(std::string_view q) { return detail::unwrap(query_impl(q)); }

  /** Parses and plans one statement, ready to be bound and run. This is
   * where a syntax error comes back, so a program that prepares its
   * statements at start-up hears about them then rather than under
   * load. */
  Statement prepare(std::string_view q) { return detail::unwrap(prepare_impl(q)); }

  /** Stops the statement running on this connection at the next boundary
   * the executor checks. Nothing failed: the connection keeps its plans
   * and its warm caches and runs the next statement normally, which is
   * the difference between this and closing it.
   *
   * This is the one call here meant to be made from another thread
   * while the connection is in use. */
  void interrupt() { detail::unwrap_void(interrupt_impl()); }

  /** How many rows the running statement has read out of storage,
   * counted from zero at each statement. Rows read rather than rows
   * answered, because the statement a user is waiting on is exactly the
   * one reading a hundred million rows to answer one. */
  std::uint64_t rows_read() const { return detail::unwrap(rows_read_impl()); }

  /** Asks to be called back every interval while a statement runs, with
   * the rows read and the time since it started. Returning false stops
   * the statement exactly as interrupt would.
   *
   * The callback runs on a thread of the library's, one per statement,
   * never two at once and never after the call it belongs to has
   * returned. What follows from that is that whatever it captures has
   * to be usable from another thread, and that it must not call back
   * into this library on the connection it is reporting on. */
  using Progress = std::function<bool(std::uint64_t rows, std::chrono::milliseconds elapsed)>;
  /** Sets the arrangement above. Calling it again replaces the watcher;
   * clear_progress takes it back. */
  void on_progress(std::chrono::milliseconds every, Progress watcher) {
    detail::unwrap_void(set_progress_impl(every, std::move(watcher)));
  }
  /** Takes the arrangement back. A statement already running keeps the
   * one it started with. */
  void clear_progress() { detail::unwrap_void(set_progress_impl({}, nullptr)); }

  ///@{
  /** Several statements as one: what they wrote is kept by commit or
   * unmade by rollback, and nothing between the two is visible to
   * another connection until the commit publishes it.
   *
   * Every statement outside one is already a transaction of its own, so
   * this does not turn transactions on.
   *
   * transaction() is the one to reach for: it answers a guard that
   * rolls back if it goes out of scope without a commit, which is what
   * makes a throw halfway through leave nothing behind. It is
   * [[nodiscard]] because a guard nobody kept is a transaction that
   * ends on the next line.
   *
   * begin, commit and rollback are the same three calls without the
   * guard, for a host putting a scope of its own around them, and
   * in_transaction says whether one is open. */
  [[nodiscard]] inline Transaction transaction(bool read_only = false);
  void begin(bool read_only = false) { detail::unwrap_void(begin_impl(read_only)); }
  void commit() { detail::unwrap_void(commit_impl()); }
  void rollback() { detail::unwrap_void(rollback_impl()); }
  bool in_transaction() const { return detail::unwrap(in_transaction_impl()); }
  ///@}

  /** Rows on their way into a table that already exists. Opening it is
   * where a table nothing declares and a read-only connection are
   * refused, rather than at the first flush a million rows later. */
  Appender appender(std::string_view table) { return detail::unwrap(appender_impl(table)); }

  /** Names the frame as a table of this connection. Does not spend the
   * handle, so registering it on two connections registers the same
   * memory twice. */
  void register_frame(Frame& f) { detail::unwrap_void(register_impl(f)); }
  /** Drops one, answering whether there was one under that name. */
  bool unregister_frame(std::string_view name) { return detail::unwrap(unregister_impl(name)); }
  /** What is registered, in sorted order. The names are copied, because
   * the pointers the ABI hands out are good only until the next count
   * call and a vector of views into them would be a trap. */
  std::vector<std::string> registered() const { return detail::unwrap(registered_impl()); }

  /** What the table id in a node or a rel is called, and nothing when no
   * table has that id. A node is a table and an offset and nothing else,
   * which is what makes it cheap, so this is the call that turns one
   * back into something a person reads.
   *
   * Nothing rather than a failure, because an id no table has is an
   * answer to the question. Node and rel tables share one id space, so
   * an id read off a rel and an id read off a node are asked for the
   * same way.
   *
   * A copy, unlike the strings that come off a result. The pointer the
   * ABI hands back is good only until the next one of these on the same
   * connection, and a view with that lifetime is a dangling read one
   * line later rather than a saving. */
  std::optional<std::string> table_name(std::uint32_t table) const {
    return detail::unwrap(table_name_impl(table));
  }

#if ZU_HAS_EXPECTED
  ///@{
  /** The expected spelling. Each of these is the call of the same name
   * above it, doing the same work through the same code, and answering
   * a failure rather than throwing it. */
  [[nodiscard]] static expected<Connection> try_open(std::string_view path) {
    return detail::to_expected(open_impl(path));
  }
  [[nodiscard]] static expected<Connection> try_create(std::string_view path) {
    return detail::to_expected(create_impl(path));
  }
  [[nodiscard]] static expected<Connection> try_memory() { return detail::to_expected(memory_impl()); }
  [[nodiscard]] expected<Connection> try_duplicate() { return detail::to_expected(duplicate_impl()); }
  [[nodiscard]] expected<Result> try_query(std::string_view q) { return detail::to_expected(query_impl(q)); }
  [[nodiscard]] expected<Statement> try_prepare(std::string_view q) {
    return detail::to_expected(prepare_impl(q));
  }
  [[nodiscard]] expected<void> try_interrupt() { return detail::to_expected_void(interrupt_impl()); }
  [[nodiscard]] expected<std::uint64_t> try_rows_read() const { return detail::to_expected(rows_read_impl()); }
  [[nodiscard]] expected<void> try_on_progress(std::chrono::milliseconds every, Progress watcher) {
    return detail::to_expected_void(set_progress_impl(every, std::move(watcher)));
  }
  [[nodiscard]] inline expected<Transaction> try_transaction(bool read_only = false);
  [[nodiscard]] expected<void> try_begin(bool read_only = false) {
    return detail::to_expected_void(begin_impl(read_only));
  }
  [[nodiscard]] expected<void> try_commit() { return detail::to_expected_void(commit_impl()); }
  [[nodiscard]] expected<void> try_rollback() { return detail::to_expected_void(rollback_impl()); }
  [[nodiscard]] expected<bool> try_in_transaction() const { return detail::to_expected(in_transaction_impl()); }
  [[nodiscard]] expected<Appender> try_appender(std::string_view table) {
    return detail::to_expected(appender_impl(table));
  }
  [[nodiscard]] expected<void> try_register_frame(Frame& f) { return detail::to_expected_void(register_impl(f)); }
  [[nodiscard]] expected<bool> try_unregister_frame(std::string_view name) {
    return detail::to_expected(unregister_impl(name));
  }
  [[nodiscard]] expected<std::vector<std::string>> try_registered() const {
    return detail::to_expected(registered_impl());
  }
  [[nodiscard]] expected<std::optional<std::string>> try_table_name(std::uint32_t table) const {
    return detail::to_expected(table_name_impl(table));
  }
  ///@}
#endif

 private:
  friend class Database;
  friend class Transaction;
  friend class Result;

  struct Watch {
    Progress fn;
  };

  static int on_progress_trampoline(void* user_data, std::uint64_t rows,
                                    std::uint64_t ms) noexcept {
    auto* w = static_cast<Watch*>(user_data);
    if (w == nullptr || !w->fn) {
      return 1;
    }
    /* An exception thrown here would cross a C frame, so it stops the
     * statement instead, which is the one thing this callback is
     * allowed to say. */
    try {
      return w->fn(rows, std::chrono::milliseconds{ms}) ? 1 : 0;
    } catch (...) {
      return 0;
    }
  }

  void clear_progress_quietly() noexcept {
    if (h_ && watch_) {
      zu_conn_set_progress(h_.get(), nullptr, nullptr, 0);
    }
    watch_.reset();
  }

  static detail::Outcome<Connection> open_impl(std::string_view path) {
    zu_conn* out = nullptr;
    zu_error* err = nullptr;
    if (auto e = detail::checked(zu_open(path.data(), path.size(), &out, &err), &err, "zu_open")) {
      return std::move(*e);
    }
    return Connection(out);
  }
  static detail::Outcome<Connection> create_impl(std::string_view path) {
    zu_conn* out = nullptr;
    zu_error* err = nullptr;
    if (auto e = detail::checked(zu_create(path.data(), path.size(), &out, &err), &err,
                                 "zu_create")) {
      return std::move(*e);
    }
    return Connection(out);
  }
  static detail::Outcome<Connection> memory_impl() {
    zu_conn* out = nullptr;
    zu_error* err = nullptr;
    if (auto e = detail::checked(zu_memory(&out, &err), &err, "zu_memory")) {
      return std::move(*e);
    }
    return Connection(out);
  }
  detail::Outcome<Connection> duplicate_impl() {
    zu_conn* out = nullptr;
    zu_error* err = nullptr;
    if (auto e = detail::checked(zu_conn_duplicate(h_.get(), &out, &err), &err,
                                 "zu_conn_duplicate")) {
      return std::move(*e);
    }
    return Connection(out);
  }
  detail::Outcome<Result> query_impl(std::string_view q) {
    zu_result* out = nullptr;
    zu_error* err = nullptr;
    if (auto e = detail::checked(zu_query(h_.get(), q.data(), q.size(), &out, &err), &err,
                                 "zu_query")) {
      return std::move(*e);
    }
    return Result(out);
  }
  detail::Outcome<Statement> prepare_impl(std::string_view q) {
    zu_stmt* out = nullptr;
    zu_error* err = nullptr;
    if (auto e = detail::checked(zu_prepare(h_.get(), q.data(), q.size(), &out, &err), &err,
                                 "zu_prepare")) {
      return std::move(*e);
    }
    return Statement(out);
  }
  detail::Outcome<detail::Nothing> interrupt_impl() {
    if (auto e = detail::checked(zu_conn_interrupt(h_.get()), "zu_conn_interrupt")) {
      return std::move(*e);
    }
    return detail::Nothing{};
  }
  detail::Outcome<std::uint64_t> rows_read_impl() const {
    std::uint64_t out = 0;
    if (auto e = detail::checked(zu_conn_rows_read(h_.get(), &out), "zu_conn_rows_read")) {
      return std::move(*e);
    }
    return out;
  }
  detail::Outcome<detail::Nothing> set_progress_impl(std::chrono::milliseconds every,
                                                     Progress watcher) {
    std::unique_ptr<Watch> next;
    if (watcher) {
      next = std::make_unique<Watch>(Watch{std::move(watcher)});
    }
    const zu_status st = zu_conn_set_progress(
        h_.get(), next ? &Connection::on_progress_trampoline : nullptr, next.get(),
        static_cast<std::uint64_t>(every.count()));
    if (auto e = detail::checked(st, "zu_conn_set_progress")) {
      /* The arrangement did not change, so the one already in place is
       * still the one the engine holds and next goes away here. */
      return std::move(*e);
    }
    /* Only now, because until the call returned the engine could still
     * have been holding the old one. */
    watch_ = std::move(next);
    return detail::Nothing{};
  }
  detail::Outcome<detail::Nothing> begin_impl(bool read_only) {
    zu_error* err = nullptr;
    if (auto e = detail::checked(zu_begin(h_.get(), read_only ? 1 : 0, &err), &err, "zu_begin")) {
      return std::move(*e);
    }
    return detail::Nothing{};
  }
  detail::Outcome<detail::Nothing> commit_impl() {
    zu_error* err = nullptr;
    if (auto e = detail::checked(zu_commit(h_.get(), &err), &err, "zu_commit")) {
      return std::move(*e);
    }
    return detail::Nothing{};
  }
  detail::Outcome<detail::Nothing> rollback_impl() {
    zu_error* err = nullptr;
    if (auto e = detail::checked(zu_rollback(h_.get(), &err), &err, "zu_rollback")) {
      return std::move(*e);
    }
    return detail::Nothing{};
  }
  detail::Outcome<bool> in_transaction_impl() const {
    std::int32_t out = 0;
    if (auto e = detail::checked(zu_conn_in_transaction(h_.get(), &out),
                                 "zu_conn_in_transaction")) {
      return std::move(*e);
    }
    return out != 0;
  }
  detail::Outcome<Appender> appender_impl(std::string_view table) {
    zu_appender* out = nullptr;
    zu_error* err = nullptr;
    if (auto e = detail::checked(
            zu_appender_open(h_.get(), table.data(), table.size(), &out, &err), &err,
            "zu_appender_open")) {
      return std::move(*e);
    }
    return Appender(out);
  }
  detail::Outcome<detail::Nothing> register_impl(Frame& f) {
    zu_error* err = nullptr;
    if (auto e = detail::checked(zu_conn_register(h_.get(), f.raw(), &err), &err,
                                 "zu_conn_register")) {
      return std::move(*e);
    }
    return detail::Nothing{};
  }
  detail::Outcome<bool> unregister_impl(std::string_view name) {
    std::int32_t out = 0;
    zu_error* err = nullptr;
    if (auto e = detail::checked(
            zu_conn_unregister(h_.get(), name.data(), name.size(), &out, &err), &err,
            "zu_conn_unregister")) {
      return std::move(*e);
    }
    return out != 0;
  }
  detail::Outcome<std::vector<std::string>> registered_impl() const {
    std::uint64_t n = 0;
    if (auto e = detail::checked(zu_conn_registered_count(h_.get(), &n),
                                 "zu_conn_registered_count")) {
      return std::move(*e);
    }
    std::vector<std::string> out;
    out.reserve(n);
    for (std::uint64_t i = 0; i < n; ++i) {
      std::size_t len = 0;
      const char* p = zu_conn_registered_name(h_.get(), i, &len);
      if (p == nullptr) {
        break;
      }
      out.emplace_back(p, len);
    }
    return out;
  }
  detail::Outcome<std::optional<std::string>> table_name_impl(std::uint32_t table) const {
    /* The only call on a connection with no status to return, so the
     * closed handle it would otherwise read as a null pointer is caught
     * here rather than by the engine. */
    if (!h_) {
      return Error::take(Status::misuse, nullptr, "zu_conn_table_name");
    }
    std::size_t len = 0;
    const char* p = zu_conn_table_name(h_.get(), table, &len);
    if (p == nullptr) {
      return std::optional<std::string>{};
    }
    return std::optional<std::string>(std::in_place, p, len);
  }

  detail::Handle<zu_conn, zu_conn_close> h_;
  std::unique_ptr<Watch> watch_;
};

/** A transaction as a scope. Committing is a call, because a commit that
 * happened because a scope ended is a commit nobody wrote down; rolling
 * back is the destructor, because the path that leaves a transaction
 * open is the path that threw.
 *
 * It asks the connection whether a transaction is still running before
 * it rolls one back, since the body may have ended it by sending
 * COMMIT itself. */
class Transaction {
 public:
  Transaction() = default;
  /** Takes charge of a transaction the connection has already begun.
   * Connection::transaction is what builds one; this does not begin
   * anything itself. */
  explicit Transaction(Connection& conn) noexcept : conn_(&conn) {}

  ///@{
  /** Moves, and does not copy, because two guards over one transaction
   * would be two rollbacks. The moved from guard owns nothing and its
   * destructor does nothing, and assigning over a guard that still owns
   * one rolls that one back first. */
  Transaction(Transaction&& o) noexcept : conn_(std::exchange(o.conn_, nullptr)) {}
  Transaction& operator=(Transaction&& o) noexcept {
    if (this != &o) {
      undo();
      conn_ = std::exchange(o.conn_, nullptr);
    }
    return *this;
  }
  Transaction(const Transaction&) = delete;
  Transaction& operator=(const Transaction&) = delete;
  ///@}

  ~Transaction() { undo(); }

  ///@{
  /** Ends the transaction, one way or the other, and spends the guard
   * so that the destructor has nothing left to undo. Either one on a
   * guard that has already been spent does nothing. */
  void commit() {
    Connection* c = std::exchange(conn_, nullptr);
    if (c != nullptr) {
      c->commit();
    }
  }
  void rollback() {
    Connection* c = std::exchange(conn_, nullptr);
    if (c != nullptr) {
      c->rollback();
    }
  }
  ///@}

  /** True while this scope still owns a transaction. */
  explicit operator bool() const noexcept { return conn_ != nullptr; }

 private:
  void undo() noexcept {
    Connection* c = std::exchange(conn_, nullptr);
    if (c == nullptr) {
      return;
    }
    /* A destructor that threw while another exception was in flight
     * would take the process down, and there is nothing left to report
     * to in any case: the transaction is being abandoned. */
    auto running = c->in_transaction_impl();
    if (running.ok() && running.value()) {
      auto undone = c->rollback_impl();
      (void)undone;
    }
  }

  Connection* conn_ = nullptr;
};

inline Transaction Connection::transaction(bool read_only) {
  detail::unwrap_void(begin_impl(read_only));
  return Transaction(*this);
}

#if ZU_HAS_EXPECTED
inline expected<Transaction> Connection::try_transaction(bool read_only) {
  auto started = begin_impl(read_only);
  if (!started.ok()) {
    return std::unexpected(std::move(started.error()));
  }
  return Transaction(*this);
}
#endif

inline detail::Outcome<Connection> Database::connect_impl() const {
  zu_conn* out = nullptr;
  zu_error* err = nullptr;
  if (auto e = detail::checked(zu_connect(h_.get(), &out, &err), &err, "zu_connect")) {
    return std::move(*e);
  }
  return Connection(out);
}

inline Connection Database::connect() const { return detail::unwrap(connect_impl()); }

#if ZU_HAS_EXPECTED
inline expected<Connection> Database::try_connect() const {
  return detail::to_expected(connect_impl());
}
#endif

inline detail::Outcome<detail::Nothing> Result::arrow_impl(zu_conn* conn, ArrowArrayStream* out,
                                                           std::uint64_t rows_per_batch) {
  zu_result* r = h_.release();
  zu_error* err = nullptr;
  const zu_status st = zu_result_arrow(conn, &r, rows_per_batch, out, &err);
  /** The call writes NULL back on every path, the failing ones included,
   * because the buffers were on their way out before anything could
   * refuse. Nothing is left to free either way. */
  if (auto e = detail::checked(st, &err, "zu_result_arrow")) {
    return std::move(*e);
  }
  return detail::Nothing{};
}

inline void Result::to_arrow(Connection& conn, ArrowArrayStream* out,
                             std::uint64_t rows_per_batch) && {
  detail::unwrap_void(arrow_impl(conn.raw(), out, rows_per_batch));
}

inline void Result::to_arrow(ArrowArrayStream* out, std::uint64_t rows_per_batch) && {
  detail::unwrap_void(arrow_impl(nullptr, out, rows_per_batch));
}

#if ZU_HAS_EXPECTED
inline expected<void> Result::try_to_arrow(Connection& conn, ArrowArrayStream* out,
                                           std::uint64_t rows_per_batch) && {
  return detail::to_expected_void(arrow_impl(conn.raw(), out, rows_per_batch));
}
#endif

/* ---- printing ---- */

namespace detail {

/** A double as a person reads it.
 *
 * Fifteen significant digits rather than the seventeen that round-trip
 * every double exactly, because this is the printing section: 0.1
 * should print as 0.1 and not as 0.10000000000000001, and a caller who
 * needs the bits back has as_double and is not scraping them out of a
 * log line.
 *
 * snprintf rather than std::to_chars, which is the better tool and is
 * C++17. The floating point half of to_chars landed in the standard
 * libraries years after the integer half, and this header promises to
 * compile at the C++20 floor rather than on the subset of C++20
 * toolchains that happen to have shipped it. */
inline std::string printed(double d) {
  char buf[32];
  const int n = std::snprintf(buf, sizeof buf, "%.15g", d);
  if (n <= 0) {
    return "nan";
  }
  const auto len = static_cast<std::size_t>(n);
  return std::string(buf, len < sizeof buf ? len : sizeof buf - 1);
}

}  // namespace detail

///@{
/** All of this is for a person to read: a log line, a test failure, a
 * debugger watch. A program that wants the bits calls the accessor.
 *
 * to_string is the whole of it, and the std::formatter specializations
 * at the foot of this header are one line each over it. That way the
 * C++20 floor gets the same text as C++23 without needing \<format\>
 * to be there, and the two spellings cannot come to disagree about
 * what a value looks like, for the same reason the throwing and try_
 * halves are one line over one implementation.
 *
 * The enums answer a view of a string literal, which costs nothing and
 * needs no allocation to print a status in a hot path. The rest build a
 * string, because there is nothing to point at otherwise.
 *
 * Every one of these is an overload of a single name rather than
 * to_string_status and to_string_node, so a generic caller writes
 * to_string(x) and argument dependent lookup finds it. */
inline std::string_view to_string(Status s) noexcept {
  switch (s) {
    case Status::ok: return "ok";
    case Status::done: return "done";
    case Status::error: return "error";
    case Status::misuse: return "misuse";
    case Status::misuse_concurrent: return "misuse_concurrent";
    case Status::misuse_closed: return "misuse_closed";
    case Status::interrupted: return "interrupted";
    case Status::conflict: return "conflict";
    case Status::corrupt: return "corrupt";
    case Status::unsupported: return "unsupported";
    case Status::io: return "io";
  }
  /* Not unreachable. The ABI numbers these and a library built from a
   * later zu.h than this header can hand back one it has never heard
   * of, which should print as a mystery rather than fall off the end of
   * the function. */
  return "unknown";
}

inline std::string_view to_string(Severity s) noexcept {
  switch (s) {
    case Severity::success: return "success";
    case Severity::no_data: return "no_data";
    case Severity::warning: return "warning";
    case Severity::informational: return "informational";
    case Severity::exception: return "exception";
  }
  return "unknown";
}

/** The names the API model uses, not the C++ spellings. A column of
 * whole numbers is an INT everywhere else a reader will meet it, and a
 * printer that called it `integer` because that is what the enumerator
 * had to be named would be teaching a vocabulary nothing else speaks. */
inline std::string_view to_string(Type t) noexcept {
  switch (t) {
    case Type::null: return "null";
    case Type::boolean: return "bool";
    case Type::integer: return "int";
    case Type::floating: return "float";
    case Type::string: return "str";
    case Type::node: return "node";
    case Type::rel: return "rel";
    case Type::list: return "list";
    case Type::path: return "path";
    case Type::temporal: return "temporal";
    case Type::record: return "record";
    case Type::graph: return "graph";
    case Type::binding_table: return "binding_table";
    case Type::bytes: return "bytes";
  }
  return "unknown";
}

inline std::string_view to_string(TemporalKind k) noexcept {
  switch (k) {
    case TemporalKind::date: return "date";
    case TemporalKind::local_time: return "local_time";
    case TemporalKind::zoned_time: return "zoned_time";
    case TemporalKind::local_datetime: return "local_datetime";
    case TemporalKind::zoned_datetime: return "zoned_datetime";
    case TemporalKind::duration_year_month: return "duration_year_month";
    case TemporalKind::duration_day_time: return "duration_day_time";
    case TemporalKind::plain: return "plain";
  }
  return "unknown";
}

/** Line and column, and not the offset. The offset is for a tool that
 * is going to index into the statement; a person reading a failure
 * wants the two numbers their editor shows them. */
inline std::string to_string(Position p) {
  return "line " + std::to_string(p.line) + ", column " + std::to_string(p.column);
}

inline std::string to_string(Node n) {
  return "node " + std::to_string(n.table) + ":" + std::to_string(n.offset);
}

inline std::string to_string(Rel r) {
  return "rel " + std::to_string(r.table) + ":" + std::to_string(r.src) + "->" +
         std::to_string(r.dst);
}

/** The kind first, because the count means nothing without it: 19000
 * is a date in 2022 and a duration of nineteen microseconds, and the
 * only thing that tells them apart is the word in front. */
inline std::string to_string(Temporal t) {
  std::string out(to_string(t.kind));
  out += ' ';
  out += std::to_string(t.count);
  if (t.offset != 0) {
    out += t.offset > 0 ? " +" : " ";
    out += std::to_string(t.offset);
  }
  return out;
}

/** A failure on one line, which is what a log wants. Error::report is
 * the other spelling, three lines with a caret under the column, for
 * the program that is showing somebody a statement to fix.
 *
 * The code and the place go in front of the message, because that is
 * the order a reader wants them in: what class of thing went wrong,
 * where, and then what the engine had to say about it. A condition with
 * neither prints as the message alone rather than as an empty prefix
 * and a colon. */
inline std::string to_string(const Error& e) {
  std::string out;
  if (const auto code = e.code(); code.has_value()) {
    out += *code;
  }
  if (const auto at = e.position(); at.has_value()) {
    if (!out.empty()) {
      out += ' ';
    }
    out += "at " + to_string(*at);
  }
  if (!out.empty()) {
    out += ": ";
  }
  out += e.message();
  return out;
}

/** A cell on one line.
 *
 * Dispatched on the type the value says it is rather than on a call
 * that could refuse, so printing a cell is not a thing that throws
 * where the printing is the last thing left working.
 *
 * The shapes a scalar cannot hold print as what they are and how many
 * they hold. A list printed elementwise is a different function and a
 * recursive one, and the caller who wants it has elements() and a range
 * that composes with std::views. */
inline std::string to_string(const Value& v) {
  if (v.raw() == nullptr) {
    return "<none>";
  }
  switch (v.type()) {
    case Type::null: return "null";
    case Type::boolean: return v.as_bool() ? "true" : "false";
    case Type::integer: return std::to_string(v.as_int());
    case Type::floating: return detail::printed(v.as_double());
    case Type::string: return std::string(v.as_string());
    case Type::node: return to_string(v.as_node());
    case Type::rel: return to_string(v.as_rel());
    case Type::temporal: return to_string(v.as_temporal());
    case Type::bytes: return std::to_string(v.as_bytes().size()) + " bytes";
    default: break;
  }
  return std::string(to_string(v.type())) + " of " + std::to_string(v.size());
}
///@}

}  // namespace zu

#if ZU_HAS_FORMAT
/** std::format over the same text.
 *
 * Defines std::formatter for the ten types zu::to_string prints:
 * Status, Severity, Type, TemporalKind, Position, Node, Rel, Temporal,
 * Error and Value. Each specialization is one line over the to_string
 * overload of the same type, so std::format("{}", v) and
 * zu::to_string(v) are the same bytes by construction rather than by
 * two pieces of code being kept in step.
 *
 * Each of these inherits formatter<string_view>, so the whole standard
 * format spec arrives with it and none of it had to be written here:
 * {:>20} pads a status the way it pads any other string, and a bad spec
 * is rejected at compile time by the base class rather than accepted
 * and ignored.
 *
 * \<format\> was already included at the top of this header and
 * ZU_HAS_FORMAT was already defined, and nothing used either, which is
 * a header claiming a capability it did not have.
 *
 * No operator<< to go with it, on purpose. \<ostream\> is one of the
 * heaviest headers in the standard library and it would land in every
 * translation unit that includes this one whether it prints or not, for
 * a wrapper whose first line is that it includes zu.h and calls nothing
 * else. A caller who wants a stream writes
 *
 *   os << zu::to_string(e);
 *
 * which is one call, no dependency, and the same bytes.
 *
 * The text goes in a named local first. to_string answers a std::string
 * for most of these, and a view of a temporary is a view of nothing
 * once the full expression it was built in has ended. */
#define ZU_FORMATTER(TYPE)                                                    \
  template <>                                                                 \
  struct std::formatter<TYPE> : std::formatter<std::string_view> {            \
    template <class Context>                                                  \
    auto format(const TYPE& v, Context& ctx) const {                          \
      const auto text = zu::to_string(v);                                     \
      return std::formatter<std::string_view>::format(std::string_view(text), \
                                                      ctx);                   \
    }                                                                         \
  }

ZU_FORMATTER(zu::Status);
ZU_FORMATTER(zu::Severity);
ZU_FORMATTER(zu::Type);
ZU_FORMATTER(zu::TemporalKind);
ZU_FORMATTER(zu::Position);
ZU_FORMATTER(zu::Node);
ZU_FORMATTER(zu::Rel);
ZU_FORMATTER(zu::Temporal);
ZU_FORMATTER(zu::Error);
ZU_FORMATTER(zu::Value);

#undef ZU_FORMATTER
#endif

#endif /* ZU_HPP */
