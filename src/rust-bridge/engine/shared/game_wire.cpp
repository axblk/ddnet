#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>
#if __cplusplus >= 202002L
#include <ranges>
#endif

#ifdef __clang__
#pragma clang diagnostic ignored "-Wdollar-in-identifier-extension"
#endif // __clang__

namespace rust {
inline namespace cxxbridge1 {
// #include "rust/cxx.h"

#ifndef CXXBRIDGE1_PANIC
#define CXXBRIDGE1_PANIC
template <typename Exception>
void panic [[noreturn]] (const char *msg);
#endif // CXXBRIDGE1_PANIC

namespace {
template <typename T>
class impl;
} // namespace

class Opaque;

template <typename T>
::std::size_t size_of();
template <typename T>
::std::size_t align_of();

#ifndef CXXBRIDGE1_RUST_SLICE
#define CXXBRIDGE1_RUST_SLICE
namespace detail {
template <bool>
struct copy_assignable_if {};

template <>
struct copy_assignable_if<false> {
  copy_assignable_if() noexcept = default;
  copy_assignable_if(const copy_assignable_if &) noexcept = default;
  copy_assignable_if &operator=(const copy_assignable_if &) & noexcept = delete;
  copy_assignable_if &operator=(copy_assignable_if &&) & noexcept = default;
};
} // namespace detail

template <typename T>
class Slice final
    : private detail::copy_assignable_if<std::is_const<T>::value> {
public:
  using value_type = T;

  Slice() noexcept;
  Slice(T *, std::size_t count) noexcept;

  template <typename C>
  explicit Slice(C &c) : Slice(c.data(), c.size()) {}

  Slice &operator=(const Slice<T> &) & noexcept = default;
  Slice &operator=(Slice<T> &&) & noexcept = default;

  T *data() const noexcept;
  std::size_t size() const noexcept;
  std::size_t length() const noexcept;
  bool empty() const noexcept;

  T &operator[](std::size_t n) const noexcept;
  T &at(std::size_t n) const;
  T &front() const noexcept;
  T &back() const noexcept;

  Slice(const Slice<T> &) noexcept = default;
  ~Slice() noexcept = default;

  class iterator;
  iterator begin() const noexcept;
  iterator end() const noexcept;

  void swap(Slice &) noexcept;

private:
  class uninit;
  Slice(uninit) noexcept;
  friend impl<Slice>;
  friend void sliceInit(void *, const void *, std::size_t) noexcept;
  friend void *slicePtr(const void *) noexcept;
  friend std::size_t sliceLen(const void *) noexcept;

  std::array<std::uintptr_t, 2> repr;
};

#ifdef __cpp_deduction_guides
template <typename C>
explicit Slice(C &c)
    -> Slice<std::remove_reference_t<decltype(*std::declval<C>().data())>>;
#endif // __cpp_deduction_guides

template <typename T>
class Slice<T>::iterator final {
public:
#if __cplusplus >= 202002L
  using iterator_category = std::contiguous_iterator_tag;
#else
  using iterator_category = std::random_access_iterator_tag;
#endif
  using value_type = T;
  using difference_type = std::ptrdiff_t;
  using pointer = typename std::add_pointer<T>::type;
  using reference = typename std::add_lvalue_reference<T>::type;

  reference operator*() const noexcept;
  pointer operator->() const noexcept;
  reference operator[](difference_type) const noexcept;

  iterator &operator++() noexcept;
  iterator operator++(int) noexcept;
  iterator &operator--() noexcept;
  iterator operator--(int) noexcept;

  iterator &operator+=(difference_type) noexcept;
  iterator &operator-=(difference_type) noexcept;
  iterator operator+(difference_type) const noexcept;
  friend inline iterator operator+(difference_type lhs, iterator rhs) noexcept {
    return rhs + lhs;
  }
  iterator operator-(difference_type) const noexcept;
  difference_type operator-(const iterator &) const noexcept;

  bool operator==(const iterator &) const noexcept;
  bool operator!=(const iterator &) const noexcept;
  bool operator<(const iterator &) const noexcept;
  bool operator<=(const iterator &) const noexcept;
  bool operator>(const iterator &) const noexcept;
  bool operator>=(const iterator &) const noexcept;

private:
  friend class Slice;
  void *pos;
  std::size_t stride;
};

#if __cplusplus >= 202002L
static_assert(std::ranges::contiguous_range<rust::Slice<const uint8_t>>);
static_assert(std::contiguous_iterator<rust::Slice<const uint8_t>::iterator>);
#endif

template <typename T>
Slice<T>::Slice() noexcept {
  sliceInit(this, reinterpret_cast<void *>(align_of<T>()), 0);
}

template <typename T>
Slice<T>::Slice(T *s, std::size_t count) noexcept {
  assert(s != nullptr || count == 0);
  sliceInit(this,
            s == nullptr && count == 0
                ? reinterpret_cast<void *>(align_of<T>())
                : const_cast<typename std::remove_const<T>::type *>(s),
            count);
}

template <typename T>
T *Slice<T>::data() const noexcept {
  return reinterpret_cast<T *>(slicePtr(this));
}

template <typename T>
std::size_t Slice<T>::size() const noexcept {
  return sliceLen(this);
}

template <typename T>
std::size_t Slice<T>::length() const noexcept {
  return this->size();
}

template <typename T>
bool Slice<T>::empty() const noexcept {
  return this->size() == 0;
}

template <typename T>
T &Slice<T>::operator[](std::size_t n) const noexcept {
  assert(n < this->size());
  auto ptr = static_cast<char *>(slicePtr(this)) + size_of<T>() * n;
  return *reinterpret_cast<T *>(ptr);
}

template <typename T>
T &Slice<T>::at(std::size_t n) const {
  if (n >= this->size()) {
    panic<std::out_of_range>("rust::Slice index out of range");
  }
  return (*this)[n];
}

template <typename T>
T &Slice<T>::front() const noexcept {
  assert(!this->empty());
  return (*this)[0];
}

template <typename T>
T &Slice<T>::back() const noexcept {
  assert(!this->empty());
  return (*this)[this->size() - 1];
}

template <typename T>
typename Slice<T>::iterator::reference
Slice<T>::iterator::operator*() const noexcept {
  return *static_cast<T *>(this->pos);
}

template <typename T>
typename Slice<T>::iterator::pointer
Slice<T>::iterator::operator->() const noexcept {
  return static_cast<T *>(this->pos);
}

template <typename T>
typename Slice<T>::iterator::reference Slice<T>::iterator::operator[](
    typename Slice<T>::iterator::difference_type n) const noexcept {
  auto ptr = static_cast<char *>(this->pos) + this->stride * n;
  return *reinterpret_cast<T *>(ptr);
}

template <typename T>
typename Slice<T>::iterator &Slice<T>::iterator::operator++() noexcept {
  this->pos = static_cast<char *>(this->pos) + this->stride;
  return *this;
}

template <typename T>
typename Slice<T>::iterator Slice<T>::iterator::operator++(int) noexcept {
  auto ret = iterator(*this);
  this->pos = static_cast<char *>(this->pos) + this->stride;
  return ret;
}

template <typename T>
typename Slice<T>::iterator &Slice<T>::iterator::operator--() noexcept {
  this->pos = static_cast<char *>(this->pos) - this->stride;
  return *this;
}

template <typename T>
typename Slice<T>::iterator Slice<T>::iterator::operator--(int) noexcept {
  auto ret = iterator(*this);
  this->pos = static_cast<char *>(this->pos) - this->stride;
  return ret;
}

template <typename T>
typename Slice<T>::iterator &Slice<T>::iterator::operator+=(
    typename Slice<T>::iterator::difference_type n) noexcept {
  this->pos = static_cast<char *>(this->pos) + this->stride * n;
  return *this;
}

template <typename T>
typename Slice<T>::iterator &Slice<T>::iterator::operator-=(
    typename Slice<T>::iterator::difference_type n) noexcept {
  this->pos = static_cast<char *>(this->pos) - this->stride * n;
  return *this;
}

template <typename T>
typename Slice<T>::iterator Slice<T>::iterator::operator+(
    typename Slice<T>::iterator::difference_type n) const noexcept {
  auto ret = iterator(*this);
  ret.pos = static_cast<char *>(this->pos) + this->stride * n;
  return ret;
}

template <typename T>
typename Slice<T>::iterator Slice<T>::iterator::operator-(
    typename Slice<T>::iterator::difference_type n) const noexcept {
  auto ret = iterator(*this);
  ret.pos = static_cast<char *>(this->pos) - this->stride * n;
  return ret;
}

template <typename T>
typename Slice<T>::iterator::difference_type
Slice<T>::iterator::operator-(const iterator &other) const noexcept {
  auto diff = std::distance(static_cast<char *>(other.pos),
                            static_cast<char *>(this->pos));
  return diff / static_cast<typename Slice<T>::iterator::difference_type>(
                    this->stride);
}

template <typename T>
bool Slice<T>::iterator::operator==(const iterator &other) const noexcept {
  return this->pos == other.pos;
}

template <typename T>
bool Slice<T>::iterator::operator!=(const iterator &other) const noexcept {
  return this->pos != other.pos;
}

template <typename T>
bool Slice<T>::iterator::operator<(const iterator &other) const noexcept {
  return this->pos < other.pos;
}

template <typename T>
bool Slice<T>::iterator::operator<=(const iterator &other) const noexcept {
  return this->pos <= other.pos;
}

template <typename T>
bool Slice<T>::iterator::operator>(const iterator &other) const noexcept {
  return this->pos > other.pos;
}

template <typename T>
bool Slice<T>::iterator::operator>=(const iterator &other) const noexcept {
  return this->pos >= other.pos;
}

template <typename T>
typename Slice<T>::iterator Slice<T>::begin() const noexcept {
  iterator it;
  it.pos = slicePtr(this);
  it.stride = size_of<T>();
  return it;
}

template <typename T>
typename Slice<T>::iterator Slice<T>::end() const noexcept {
  iterator it = this->begin();
  it.pos = static_cast<char *>(it.pos) + it.stride * this->size();
  return it;
}

template <typename T>
void Slice<T>::swap(Slice &rhs) noexcept {
  std::swap(*this, rhs);
}
#endif // CXXBRIDGE1_RUST_SLICE

#ifndef CXXBRIDGE1_RUST_BITCOPY_T
#define CXXBRIDGE1_RUST_BITCOPY_T
struct unsafe_bitcopy_t final {
  explicit unsafe_bitcopy_t() = default;
};
#endif // CXXBRIDGE1_RUST_BITCOPY_T

#ifndef CXXBRIDGE1_RUST_VEC
#define CXXBRIDGE1_RUST_VEC
template <typename T>
class Vec final {
public:
  using value_type = T;

  Vec() noexcept;
  Vec(std::initializer_list<T>);
  Vec(const Vec &);
  Vec(Vec &&) noexcept;
  ~Vec() noexcept;

  Vec &operator=(Vec &&) & noexcept;
  Vec &operator=(const Vec &) &;

  std::size_t size() const noexcept;
  bool empty() const noexcept;
  const T *data() const noexcept;
  T *data() noexcept;
  std::size_t capacity() const noexcept;

  const T &operator[](std::size_t n) const noexcept;
  const T &at(std::size_t n) const;
  const T &front() const noexcept;
  const T &back() const noexcept;

  T &operator[](std::size_t n) noexcept;
  T &at(std::size_t n);
  T &front() noexcept;
  T &back() noexcept;

  void reserve(std::size_t new_cap);
  void push_back(const T &value);
  void push_back(T &&value);
  template <typename... Args>
  void emplace_back(Args &&...args);
  void truncate(std::size_t len);
  void clear();

  using iterator = typename Slice<T>::iterator;
  iterator begin() noexcept;
  iterator end() noexcept;

  using const_iterator = typename Slice<const T>::iterator;
  const_iterator begin() const noexcept;
  const_iterator end() const noexcept;
  const_iterator cbegin() const noexcept;
  const_iterator cend() const noexcept;

  void swap(Vec &) noexcept;

  Vec(unsafe_bitcopy_t, const Vec &) noexcept;

private:
  void reserve_total(std::size_t new_cap) noexcept;
  void set_len(std::size_t len) noexcept;
  void drop() noexcept;

  friend void swap(Vec &lhs, Vec &rhs) noexcept { lhs.swap(rhs); }

  std::array<std::uintptr_t, 3> repr;
};

template <typename T>
Vec<T>::Vec(std::initializer_list<T> init) : Vec{} {
  this->reserve_total(init.size());
  std::move(init.begin(), init.end(), std::back_inserter(*this));
}

template <typename T>
Vec<T>::Vec(const Vec &other) : Vec() {
  this->reserve_total(other.size());
  std::copy(other.begin(), other.end(), std::back_inserter(*this));
}

template <typename T>
Vec<T>::Vec(Vec &&other) noexcept : repr(other.repr) {
  new (&other) Vec();
}

template <typename T>
Vec<T>::~Vec() noexcept {
  this->drop();
}

template <typename T>
Vec<T> &Vec<T>::operator=(Vec &&other) & noexcept {
  this->drop();
  this->repr = other.repr;
  new (&other) Vec();
  return *this;
}

template <typename T>
Vec<T> &Vec<T>::operator=(const Vec &other) & {
  if (this != &other) {
    this->drop();
    new (this) Vec(other);
  }
  return *this;
}

template <typename T>
bool Vec<T>::empty() const noexcept {
  return this->size() == 0;
}

template <typename T>
T *Vec<T>::data() noexcept {
  return const_cast<T *>(const_cast<const Vec<T> *>(this)->data());
}

template <typename T>
const T &Vec<T>::operator[](std::size_t n) const noexcept {
  assert(n < this->size());
  auto data = reinterpret_cast<const char *>(this->data());
  return *reinterpret_cast<const T *>(data + n * size_of<T>());
}

template <typename T>
const T &Vec<T>::at(std::size_t n) const {
  if (n >= this->size()) {
    panic<std::out_of_range>("rust::Vec index out of range");
  }
  return (*this)[n];
}

template <typename T>
const T &Vec<T>::front() const noexcept {
  assert(!this->empty());
  return (*this)[0];
}

template <typename T>
const T &Vec<T>::back() const noexcept {
  assert(!this->empty());
  return (*this)[this->size() - 1];
}

template <typename T>
T &Vec<T>::operator[](std::size_t n) noexcept {
  assert(n < this->size());
  auto data = reinterpret_cast<char *>(this->data());
  return *reinterpret_cast<T *>(data + n * size_of<T>());
}

template <typename T>
T &Vec<T>::at(std::size_t n) {
  if (n >= this->size()) {
    panic<std::out_of_range>("rust::Vec index out of range");
  }
  return (*this)[n];
}

template <typename T>
T &Vec<T>::front() noexcept {
  assert(!this->empty());
  return (*this)[0];
}

template <typename T>
T &Vec<T>::back() noexcept {
  assert(!this->empty());
  return (*this)[this->size() - 1];
}

template <typename T>
void Vec<T>::reserve(std::size_t new_cap) {
  this->reserve_total(new_cap);
}

template <typename T>
void Vec<T>::push_back(const T &value) {
  this->emplace_back(value);
}

template <typename T>
void Vec<T>::push_back(T &&value) {
  this->emplace_back(std::move(value));
}

template <typename T>
template <typename... Args>
void Vec<T>::emplace_back(Args &&...args) {
  auto size = this->size();
  this->reserve_total(size + 1);
  ::new (reinterpret_cast<T *>(reinterpret_cast<char *>(this->data()) +
                               size * size_of<T>()))
      T(std::forward<Args>(args)...);
  this->set_len(size + 1);
}

template <typename T>
void Vec<T>::clear() {
  this->truncate(0);
}

template <typename T>
typename Vec<T>::iterator Vec<T>::begin() noexcept {
  return Slice<T>(this->data(), this->size()).begin();
}

template <typename T>
typename Vec<T>::iterator Vec<T>::end() noexcept {
  return Slice<T>(this->data(), this->size()).end();
}

template <typename T>
typename Vec<T>::const_iterator Vec<T>::begin() const noexcept {
  return this->cbegin();
}

template <typename T>
typename Vec<T>::const_iterator Vec<T>::end() const noexcept {
  return this->cend();
}

template <typename T>
typename Vec<T>::const_iterator Vec<T>::cbegin() const noexcept {
  return Slice<const T>(this->data(), this->size()).begin();
}

template <typename T>
typename Vec<T>::const_iterator Vec<T>::cend() const noexcept {
  return Slice<const T>(this->data(), this->size()).end();
}

template <typename T>
void Vec<T>::swap(Vec &rhs) noexcept {
  using std::swap;
  swap(this->repr, rhs.repr);
}

template <typename T>
Vec<T>::Vec(unsafe_bitcopy_t, const Vec &bits) noexcept : repr(bits.repr) {}
#endif // CXXBRIDGE1_RUST_VEC

#ifndef CXXBRIDGE1_IS_COMPLETE
#define CXXBRIDGE1_IS_COMPLETE
namespace detail {
namespace {
template <typename T, typename = std::size_t>
struct is_complete : std::false_type {};
template <typename T>
struct is_complete<T, decltype(sizeof(T))> : std::true_type {};
} // namespace
} // namespace detail
#endif // CXXBRIDGE1_IS_COMPLETE

#ifndef CXXBRIDGE1_LAYOUT
#define CXXBRIDGE1_LAYOUT
class layout {
  template <typename T>
  friend std::size_t size_of();
  template <typename T>
  friend std::size_t align_of();
  template <typename T>
  static typename std::enable_if<std::is_base_of<Opaque, T>::value,
                                 std::size_t>::type
  do_size_of() {
    return T::layout::size();
  }
  template <typename T>
  static typename std::enable_if<!std::is_base_of<Opaque, T>::value,
                                 std::size_t>::type
  do_size_of() {
    return sizeof(T);
  }
  template <typename T>
  static
      typename std::enable_if<detail::is_complete<T>::value, std::size_t>::type
      size_of() {
    return do_size_of<T>();
  }
  template <typename T>
  static typename std::enable_if<std::is_base_of<Opaque, T>::value,
                                 std::size_t>::type
  do_align_of() {
    return T::layout::align();
  }
  template <typename T>
  static typename std::enable_if<!std::is_base_of<Opaque, T>::value,
                                 std::size_t>::type
  do_align_of() {
    return alignof(T);
  }
  template <typename T>
  static
      typename std::enable_if<detail::is_complete<T>::value, std::size_t>::type
      align_of() {
    return do_align_of<T>();
  }
};

template <typename T>
std::size_t size_of() {
  return layout::size_of<T>();
}

template <typename T>
std::size_t align_of() {
  return layout::align_of<T>();
}
#endif // CXXBRIDGE1_LAYOUT

namespace detail {
template <typename T, typename = void *>
struct operator_new {
  void *operator()(::std::size_t sz) { return ::operator new(sz); }
};

template <typename T>
struct operator_new<T, decltype(T::operator new(sizeof(T)))> {
  void *operator()(::std::size_t sz) { return T::operator new(sz); }
};
} // namespace detail

template <typename T>
union MaybeUninit {
  T value;
  void *operator new(::std::size_t sz) { return detail::operator_new<T>{}(sz); }
  MaybeUninit() {}
  ~MaybeUninit() {}
};
} // namespace cxxbridge1
} // namespace rust

#if __cplusplus >= 201402L
#define CXX_DEFAULT_VALUE(value) = value
#else
#define CXX_DEFAULT_VALUE(value)
#endif

namespace GameWire {
  enum class FrameType : ::std::uint8_t;
  enum class DecodeStatus : ::std::uint8_t;
  struct Frame;
  struct MapHeader;
  struct Resume;
  struct DatagramMessage;
  struct Datagram;
}

namespace GameWire {
#ifndef CXXBRIDGE1_ENUM_GameWire$FrameType
#define CXXBRIDGE1_ENUM_GameWire$FrameType
enum class FrameType : ::std::uint8_t {
  ClientHello = 0,
  ServerHello = 1,
  Message = 2,
  Disconnect = 3,
  Resume = 4,
  MapHeader = 5,
};
#endif // CXXBRIDGE1_ENUM_GameWire$FrameType

#ifndef CXXBRIDGE1_ENUM_GameWire$DecodeStatus
#define CXXBRIDGE1_ENUM_GameWire$DecodeStatus
enum class DecodeStatus : ::std::uint8_t {
  Ok = 0,
  NeedMore = 1,
  Invalid = 2,
};
#endif // CXXBRIDGE1_ENUM_GameWire$DecodeStatus

#ifndef CXXBRIDGE1_STRUCT_GameWire$Frame
#define CXXBRIDGE1_STRUCT_GameWire$Frame
// A frame at the start of a buffer, `size` bytes long.
struct Frame final {
  ::GameWire::DecodeStatus status;
  ::std::uint64_t frame_type CXX_DEFAULT_VALUE(0);
  bool skippable CXX_DEFAULT_VALUE(false);
  ::std::size_t payload_offset CXX_DEFAULT_VALUE(0);
  ::std::size_t payload_size CXX_DEFAULT_VALUE(0);
  ::std::size_t size CXX_DEFAULT_VALUE(0);

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_GameWire$Frame

#ifndef CXXBRIDGE1_STRUCT_GameWire$MapHeader
#define CXXBRIDGE1_STRUCT_GameWire$MapHeader
struct MapHeader final {
  bool valid CXX_DEFAULT_VALUE(false);
  ::std::uint64_t size CXX_DEFAULT_VALUE(0);
  ::std::uint32_t crc CXX_DEFAULT_VALUE(0);
  ::std::array<::std::uint8_t, 32> sha256;
  ::std::size_t name_offset CXX_DEFAULT_VALUE(0);
  ::std::size_t name_size CXX_DEFAULT_VALUE(0);

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_GameWire$MapHeader

#ifndef CXXBRIDGE1_STRUCT_GameWire$Resume
#define CXXBRIDGE1_STRUCT_GameWire$Resume
struct Resume final {
  bool valid CXX_DEFAULT_VALUE(false);
  ::std::uint64_t session_id CXX_DEFAULT_VALUE(0);
  ::std::size_t token_offset CXX_DEFAULT_VALUE(0);
  ::std::size_t token_size CXX_DEFAULT_VALUE(0);

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_GameWire$Resume

#ifndef CXXBRIDGE1_STRUCT_GameWire$DatagramMessage
#define CXXBRIDGE1_STRUCT_GameWire$DatagramMessage
struct DatagramMessage final {
  ::std::size_t offset CXX_DEFAULT_VALUE(0);
  ::std::size_t size CXX_DEFAULT_VALUE(0);

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_GameWire$DatagramMessage

#ifndef CXXBRIDGE1_STRUCT_GameWire$Datagram
#define CXXBRIDGE1_STRUCT_GameWire$Datagram
struct Datagram final {
  bool valid CXX_DEFAULT_VALUE(false);
  ::std::uint64_t sequence CXX_DEFAULT_VALUE(0);
  ::rust::Vec<::GameWire::DatagramMessage> messages;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_GameWire$Datagram

extern "C" {
void GameWire$cxxbridge1$195$encode_frame(::GameWire::FrameType frame_type, ::rust::Slice<::std::uint8_t const> payload, ::rust::Vec<::std::uint8_t> *return$) noexcept;

void GameWire$cxxbridge1$195$encode_client_hello(bool sixup, ::std::size_t max_datagram_size, ::rust::Slice<::std::uint8_t const> nonce, ::rust::Slice<::std::uint8_t const> resume_token, ::rust::Vec<::std::uint8_t> *return$) noexcept;

void GameWire$cxxbridge1$195$encode_datagram(::std::uint64_t sequence, ::rust::Slice<::std::uint8_t const> message, ::rust::Vec<::std::uint8_t> *return$) noexcept;

void GameWire$cxxbridge1$195$decode_frame(::rust::Slice<::std::uint8_t const> data, ::GameWire::Frame *return$) noexcept;

void GameWire$cxxbridge1$195$decode_map_stream_start(::rust::Slice<::std::uint8_t const> data, ::GameWire::Frame *return$) noexcept;

::std::size_t GameWire$cxxbridge1$195$decode_server_hello(::rust::Slice<::std::uint8_t const> payload, bool sixup) noexcept;

void GameWire$cxxbridge1$195$decode_map_header(::rust::Slice<::std::uint8_t const> payload, ::GameWire::MapHeader *return$) noexcept;

void GameWire$cxxbridge1$195$decode_resume(::rust::Slice<::std::uint8_t const> payload, ::GameWire::Resume *return$) noexcept;

void GameWire$cxxbridge1$195$decode_datagram(::rust::Slice<::std::uint8_t const> data, ::GameWire::Datagram *return$) noexcept;
} // extern "C"

// Empty if the payload does not fit the frame.
::rust::Vec<::std::uint8_t> encode_frame(::GameWire::FrameType frame_type, ::rust::Slice<::std::uint8_t const> payload) noexcept {
  ::rust::MaybeUninit<::rust::Vec<::std::uint8_t>> return$;
  GameWire$cxxbridge1$195$encode_frame(frame_type, payload, &return$.value);
  return ::std::move(return$.value);
}

// The start of the control stream a client opens, empty on error.
::rust::Vec<::std::uint8_t> encode_client_hello(bool sixup, ::std::size_t max_datagram_size, ::rust::Slice<::std::uint8_t const> nonce, ::rust::Slice<::std::uint8_t const> resume_token) noexcept {
  ::rust::MaybeUninit<::rust::Vec<::std::uint8_t>> return$;
  GameWire$cxxbridge1$195$encode_client_hello(sixup, max_datagram_size, nonce, resume_token, &return$.value);
  return ::std::move(return$.value);
}

// Empty if the message does not fit a datagram.
::rust::Vec<::std::uint8_t> encode_datagram(::std::uint64_t sequence, ::rust::Slice<::std::uint8_t const> message) noexcept {
  ::rust::MaybeUninit<::rust::Vec<::std::uint8_t>> return$;
  GameWire$cxxbridge1$195$encode_datagram(sequence, message, &return$.value);
  return ::std::move(return$.value);
}

::GameWire::Frame decode_frame(::rust::Slice<::std::uint8_t const> data) noexcept {
  ::rust::MaybeUninit<::GameWire::Frame> return$;
  GameWire$cxxbridge1$195$decode_frame(data, &return$.value);
  return ::std::move(return$.value);
}

// The map header frame after the header of a map stream.
::GameWire::Frame decode_map_stream_start(::rust::Slice<::std::uint8_t const> data) noexcept {
  ::rust::MaybeUninit<::GameWire::Frame> return$;
  GameWire$cxxbridge1$195$decode_map_stream_start(data, &return$.value);
  return ::std::move(return$.value);
}

// The datagram size the server accepts, 0 if the hello is not one this
// client can talk to.
::std::size_t decode_server_hello(::rust::Slice<::std::uint8_t const> payload, bool sixup) noexcept {
  return GameWire$cxxbridge1$195$decode_server_hello(payload, sixup);
}

::GameWire::MapHeader decode_map_header(::rust::Slice<::std::uint8_t const> payload) noexcept {
  ::rust::MaybeUninit<::GameWire::MapHeader> return$;
  GameWire$cxxbridge1$195$decode_map_header(payload, &return$.value);
  return ::std::move(return$.value);
}

::GameWire::Resume decode_resume(::rust::Slice<::std::uint8_t const> payload) noexcept {
  ::rust::MaybeUninit<::GameWire::Resume> return$;
  GameWire$cxxbridge1$195$decode_resume(payload, &return$.value);
  return ::std::move(return$.value);
}

::GameWire::Datagram decode_datagram(::rust::Slice<::std::uint8_t const> data) noexcept {
  ::rust::MaybeUninit<::GameWire::Datagram> return$;
  GameWire$cxxbridge1$195$decode_datagram(data, &return$.value);
  return ::std::move(return$.value);
}
} // namespace GameWire

extern "C" {
void cxxbridge1$rust_vec$GameWire$DatagramMessage$new(::rust::Vec<::GameWire::DatagramMessage> const *ptr) noexcept;
void cxxbridge1$rust_vec$GameWire$DatagramMessage$drop(::rust::Vec<::GameWire::DatagramMessage> *ptr) noexcept;
::std::size_t cxxbridge1$rust_vec$GameWire$DatagramMessage$len(::rust::Vec<::GameWire::DatagramMessage> const *ptr) noexcept;
::std::size_t cxxbridge1$rust_vec$GameWire$DatagramMessage$capacity(::rust::Vec<::GameWire::DatagramMessage> const *ptr) noexcept;
::GameWire::DatagramMessage const *cxxbridge1$rust_vec$GameWire$DatagramMessage$data(::rust::Vec<::GameWire::DatagramMessage> const *ptr) noexcept;
void cxxbridge1$rust_vec$GameWire$DatagramMessage$reserve_total(::rust::Vec<::GameWire::DatagramMessage> *ptr, ::std::size_t new_cap) noexcept;
void cxxbridge1$rust_vec$GameWire$DatagramMessage$set_len(::rust::Vec<::GameWire::DatagramMessage> *ptr, ::std::size_t len) noexcept;
void cxxbridge1$rust_vec$GameWire$DatagramMessage$truncate(::rust::Vec<::GameWire::DatagramMessage> *ptr, ::std::size_t len) noexcept;
} // extern "C"

namespace rust {
inline namespace cxxbridge1 {
template <>
Vec<::GameWire::DatagramMessage>::Vec() noexcept {
  cxxbridge1$rust_vec$GameWire$DatagramMessage$new(this);
}
template <>
void Vec<::GameWire::DatagramMessage>::drop() noexcept {
  return cxxbridge1$rust_vec$GameWire$DatagramMessage$drop(this);
}
template <>
::std::size_t Vec<::GameWire::DatagramMessage>::size() const noexcept {
  return cxxbridge1$rust_vec$GameWire$DatagramMessage$len(this);
}
template <>
::std::size_t Vec<::GameWire::DatagramMessage>::capacity() const noexcept {
  return cxxbridge1$rust_vec$GameWire$DatagramMessage$capacity(this);
}
template <>
::GameWire::DatagramMessage const *Vec<::GameWire::DatagramMessage>::data() const noexcept {
  return cxxbridge1$rust_vec$GameWire$DatagramMessage$data(this);
}
template <>
void Vec<::GameWire::DatagramMessage>::reserve_total(::std::size_t new_cap) noexcept {
  return cxxbridge1$rust_vec$GameWire$DatagramMessage$reserve_total(this, new_cap);
}
template <>
void Vec<::GameWire::DatagramMessage>::set_len(::std::size_t len) noexcept {
  return cxxbridge1$rust_vec$GameWire$DatagramMessage$set_len(this, len);
}
template <>
void Vec<::GameWire::DatagramMessage>::truncate(::std::size_t len) {
  return cxxbridge1$rust_vec$GameWire$DatagramMessage$truncate(this, len);
}
} // namespace cxxbridge1
} // namespace rust
