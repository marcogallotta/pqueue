# Raw-buffer Queue API Design

## Problem

The current API uses `std::string` for all record passing. On Arduino targets
with weak or absent heap allocators, `std::string` causes problems at the call
site — not just internally. The goal is to let callers enqueue and peek records
using caller-owned buffers, with no required caller-side heap allocation on the
hot path. Internal allocations are not eliminated by this task.

## Scope

- Add raw-buffer overloads to `Queue` and `Outbox`
- Introduce `pqueue::Span` and `pqueue::MutableSpan` for a clean call-site API
- Keep the existing `std::string` API intact and first-class (no breaking changes, no deprecations)
- Do not change the internal storage representation (`std::string` stays as the
  internal record container — eliminating internal allocations is a separate,
  larger task)

---

## New types: `pqueue::Span` and `pqueue::MutableSpan`

```cpp
// types.h
struct Span {
    const uint8_t* data = nullptr;
    size_t         len  = 0;

    Span() = default;
    Span(const uint8_t* d, size_t l) : data(d), len(l) {}
    Span(const char* s, size_t l)    : data(reinterpret_cast<const uint8_t*>(s)), len(l) {}
};

struct MutableSpan {
    uint8_t* data = nullptr;
    size_t   len  = 0;

    MutableSpan() = default;
    MutableSpan(uint8_t* d, size_t l) : data(d), len(l) {}
};
```

No ownership, no allocation. Callers on platforms with `std::string` can use
the existing API unchanged.

`Span(const char* s, size_t l)` is provided for convenience when working with
character arrays, but length must always be explicit. No null-terminated
convenience constructor is provided; record payloads are opaque bytes, not text,
and null-termination assumptions break binary safety.

---

## `Queue` additions

**Enqueue:**
```cpp
Status enqueue(Span record);
```
The existing `enqueue(const std::string&)` becomes a one-line wrapper:
```cpp
Status Queue::enqueue(const std::string& r) {
    return enqueue(Span(reinterpret_cast<const uint8_t*>(r.data()), r.size()));
}
```

**Peek size:**
```cpp
Status peekSize(size_t& out);
```
Returns the byte length of the front record without reading its payload. Useful
for callers with small or variable stack buffers who cannot assume
`recordSizeBytes`. Cheap: the size is already in RAM (`records_.front().payloadBytes`),
no I/O.

**Peek:**
```cpp
Status peek(MutableSpan out, size_t& written);
```
Writes the front record's payload into `out.data[0..written-1]`. Returns
`RecordTooLarge` if the stored record exceeds `out.len`.

**Null-buffer behaviour:** `out.len > 0 && out.data == nullptr` returns
`InvalidArgument`. `out.len == 0 && out.data == nullptr` is accepted only if
the stored record is empty (i.e. empty records are permitted by the store).

The existing `peek(std::string&)` queries size first, then delegates:
```cpp
Status Queue::peek(std::string& out) {
    size_t n = 0;
    auto st = peekSize(n);
    if (!st.ok()) return st;
    out.resize(n);
    return peek(MutableSpan(reinterpret_cast<uint8_t*>(&out[0]), out.size()), n);
}
```

Typical caller pattern with a fixed stack buffer:
```cpp
uint8_t buf[512];
size_t n;
if (queue.peek(pqueue::MutableSpan(buf, sizeof(buf)), n).ok()) {
    send(buf, n);
    queue.pop();
}
```

Typical caller pattern when size is not known in advance:
```cpp
size_t n;
queue.peekSize(n);
uint8_t* buf = myAlloc(n);
queue.peek(pqueue::MutableSpan(buf, n), n);
```

`pop()` has no record payload and needs no change.

---

## `Outbox` additions

**Submit:**
```cpp
SubmitResult submit(Span payload);
```
The existing `submit(const std::string&)` becomes a wrapper.

**SendCallback:**

Add a raw variant alongside the existing one:
```cpp
using RawSendCallback = SendResult (*)(void* context, Span payload, const RetryState& retry);
```

`Outbox` gets a second constructor accepting `RawSendCallback` instead of
`SendCallback`. Only one callback variant is configured per instance; dispatch
cost is negligible. Internally, the drain loop calls whichever was registered.

`OutboxPayloadValidator` gets the same treatment:
```cpp
using RawOutboxPayloadValidator = bool (*)(void* context, Span payload, ValidationIssue& issue);
```

---

## Binary payload support

A useful side effect of the raw API: it makes explicit that records are opaque
bytes, not text. Callers can store payloads containing NUL bytes, binary-encoded
structs, or packed integers without wrapping in a string type. The existing
`std::string` API already treats payloads as opaque bytes (the `SendCallback`
doc notes this), but the `Span`/`MutableSpan` API makes it self-evident at the
call site.

---

## What does NOT change

- Internal record representation stays as `std::string`. No internal allocations are eliminated.
- `std::string` overloads remain first-class. Not deprecated.
- `rewriteFront` (private, Outbox-internal) stays `std::string`-based.
- `RecordVisitor` (private) stays `std::string`-based.
- The `http::Outbox` layer is unaffected.
- No format change, no version bump.

---

## Decisions

| Question | Decision |
|---|---|
| Mutable buffer type for peek | `MutableSpan` — symmetrical with `Span`, avoids raw triple |
| `peekSize` | Add now — cheap API surface, avoids callers always sizing to `recordSizeBytes` |
| `std::string` overloads | Keep first-class, no deprecation at 1.0 |
| Heap allocation claim | "No required caller-side heap allocation" — internal `std::string` allocations remain |
