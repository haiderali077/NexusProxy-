# NexusProxy Tunnel Protocol

## Purpose

NexusProxy uses one persistent TCP connection between the developer client and
the edge. TCP only provides an ordered stream of bytes, so this protocol divides
that stream into frames. Frames tell both programs whether the bytes open a
public stream, carry application data, close a stream, or manage the tunnel.

This document defines protocol version 1. The frame types are declared in
`include/nexus/protocol.hpp`, implemented in
`src/protocol/protocol.cpp`, and checked by
`tests/unit/protocol_tests.cpp`.

## Byte order and encoding

- Multi-byte integers use network byte order (big-endian).
- One-byte fields do not need byte-order conversion.
- Text payloads use UTF-8 and never include a trailing null byte.
- Payloads are raw bytes and may contain `\0` or any other byte.
- Code must encode and decode fields explicitly. It must not send a C++ struct
  directly because compiler padding and alignment are not part of the protocol.

## Frame layout

Every frame begins with this fixed 12-byte header:

| Offset | Size | Field | Meaning |
| ---: | ---: | --- | --- |
| 0 | 1 byte | Version | Must be `1`. |
| 1 | 1 byte | Message type | One of the values defined below. |
| 2 | 2 bytes | Flags | Reserved for future use; must be `0`. |
| 4 | 4 bytes | Stream ID | `0` for tunnel control, nonzero for a public stream. |
| 8 | 4 bytes | Payload length | Number of payload bytes after the header. |

The payload immediately follows the header. A frame's total size is therefore
`12 + payload length` bytes.

## Size limit

The maximum payload is 65,536 bytes (64 KiB). A sender must split larger data
into multiple `DATA` frames. A receiver must reject a larger declared length
before allocating memory for the payload.

The maximum complete frame size is 65,548 bytes.

## Message types

| Value | Name | Direction | Stream ID | Payload |
| ---: | --- | --- | --- | --- |
| 1 | `REGISTER` | Client to edge | `0` | Optional requested tunnel name, 0–63 bytes. |
| 2 | `REGISTER_OK` | Edge to client | `0` | Accepted tunnel name, 0–63 bytes. |
| 3 | `OPEN` | Edge to client | Nonzero | Empty. |
| 4 | `DATA` | Either direction | Nonzero | 1–65,536 raw bytes. |
| 5 | `CLOSE` | Either direction | Nonzero | Empty. |
| 6 | `PING` | Either direction | `0` | Exactly 8 opaque bytes. |
| 7 | `PONG` | Either direction | `0` | The same 8 bytes received in `PING`. |
| 8 | `AUTH` | Client to edge | `0` | 32–256 opaque token bytes. |
| 9 | `AUTH_OK` | Edge to client | `0` | Empty. |
| 10 | `AUTH_FAILED` | Edge to client | `0` | Empty. |

Message type `0` is invalid. Unknown message types are protocol errors.

### `REGISTER`

The client sends `REGISTER` immediately after opening the tunnel. During the
current single-tunnel stage, its payload may be empty. Named tunnel routing can
later use the optional name. A non-empty name may contain ASCII letters,
numbers, and hyphens only.

The edge must not accept stream frames before registration succeeds.

### `REGISTER_OK`

The edge sends `REGISTER_OK` after accepting registration. The client must not
process `OPEN` or `DATA` frames before receiving it. If registration fails, the
edge closes the tunnel; version 1 does not define an error frame.

### `OPEN`

The edge creates a new nonzero stream ID when it accepts a public connection,
then sends `OPEN`. The client uses that ID to associate the public connection
with a new connection to the configured local service.

### `DATA`

`DATA` carries the unmodified application bytes for one stream. The edge uses
it for public request bytes, and the client uses it for local response bytes.
Several `DATA` frames still represent one continuous TCP byte stream; frame
boundaries are not application-message boundaries.

### `CLOSE`

Either side sends `CLOSE` when its socket for that stream closes or cannot
continue. The receiver closes its matching socket and removes the stream. Each
side sends at most one `CLOSE` for a stream. Version 1 closes the whole stream
and does not represent TCP half-close separately.

### `PING` and `PONG`

`PING` checks whether an idle tunnel is still responsive. Its payload is an
opaque 8-byte value chosen by the sender. The receiver returns the exact value
in `PONG`. Heartbeat timing will be added during the reliability phase.

### `AUTH`, `AUTH_OK`, and `AUTH_FAILED`

After completing TLS, the client sends exactly one `AUTH` frame before any
registration, heartbeat, or stream frame. Its payload is an opaque token between
32 and 256 bytes. TLS protects the token in transit; implementations must never
write it to logs.

The edge compares the token with its configured secret. It returns an empty
`AUTH_OK` frame on success. On failure it may return empty `AUTH_FAILED`, then
closes the TLS connection. The response deliberately provides no detail about
which part of authentication failed.

The edge must not create public routing state or accept public streams until
authentication succeeds.

## Stream ID rules

- Stream ID `0` is reserved for tunnel-level control messages.
- The edge assigns public stream IDs; the client never chooses them.
- IDs increase from `1` and are not reused during one tunnel connection.
- `OPEN` must introduce an unused stream ID before `DATA` or `CLOSE` uses it.
- Receiving a stream frame for an unknown or already closed ID is an error.
- If the edge exhausts the 32-bit ID space, it closes and recreates the tunnel
  instead of reusing an ID.

## Tunnel sequence

```text
client                         edge
  |------ AUTH ---------------->|
  |<----- AUTH_OK ---------------|
  |------ REGISTER ------------>|
  |<----- REGISTER_OK -----------|
  |                              |
  |       public user connects   |
  |<----- OPEN(stream 1) --------|
  |<----- DATA(stream 1) --------|  public request
  |------ DATA(stream 1) ------->|  local response
  |<---- or ---- CLOSE --------->|  either side may close
```

The registration exchange happens once per tunnel. `OPEN`, `DATA`, and `CLOSE`
may later be interleaved for many stream IDs on that same tunnel.

## Streaming decoder behavior

One `recv()` call is not guaranteed to contain one frame. The decoder must:

1. Append received bytes to an input buffer.
2. Wait until at least 12 bytes are available.
3. Decode and validate the header.
4. Reject an oversized payload before reserving space for it.
5. Wait until the full payload is available.
6. Emit one complete frame and remove its bytes from the input buffer.
7. Continue decoding while another complete frame remains.

This handles both a frame split across several `recv()` calls and several
frames delivered by one `recv()` call.

## Protocol errors

The receiver closes the tunnel when it sees:

- a version other than `1`;
- an unknown message type;
- nonzero reserved flags;
- a payload over 65,536 bytes;
- a payload length that is invalid for its message type;
- the wrong stream ID or direction for a message;
- a stream frame before registration;
- a duplicate `OPEN`; or
- `DATA` or `CLOSE` for an unknown stream.

Closing on an invalid frame keeps version 1 simple and prevents both sides from
continuing with different interpretations of the byte stream.

## Implementation order

1. Add shared frame types and constants in `include/nexus/protocol.hpp`.
2. Encode complete frames in `src/protocol/protocol.cpp`.
3. Add a stateful decoder that accepts arbitrary byte chunks.
4. Unit-test split frames, combined frames, binary payloads, and invalid input.
5. Replace the current one-byte `'O'` signal with `REGISTER`, `REGISTER_OK`, and
   `OPEN`.
6. Wrap relayed bytes in `DATA` frames and replace socket-only closure with
   `CLOSE` frames.

## Version 1 limitations

- The frame format does not encrypt or authenticate itself. The current client
  and edge protect the tunnel with TLS and token authentication at the
  transport layer.
- Registration rejection has no detailed error response.
- Stream priority, compression, and half-close are not supported.
- Queue limits and backpressure belong to the multiplexing phase.

---

# Implementation Learning Guide

This section explains the current protocol code from first principles. Reading
it once will not replace practice, but it gives you the concepts and vocabulary
needed to explain the design, modify it safely, and reproduce the main parts in
an interview.

## 1. The whole idea in one picture

The C++ program works with a convenient object:

```cpp
Frame{
    MessageType::Data,
    42,
    {0x48, 0x69}
}
```

TCP cannot send a `Frame` object directly. It only sends bytes. The encoder
turns the object into a stable wire representation:

```text
Frame object --encodeFrame()--> 12-byte header + payload bytes
```

The receiver may get those bytes in pieces or may get several frames at once.
The streaming decoder collects the bytes and rebuilds complete `Frame` objects:

```text
recv() chunks --> FrameDecoder::append() --> FrameDecoder::nextFrame()
```

These two transformations are called:

- **Serialization:** converting structured data into bytes.
- **Deserialization:** converting bytes back into structured data.

## 2. What each file owns

### `include/nexus/protocol.hpp`

The header is the public interface. It contains the names and declarations that
the client, edge, implementation, and tests are allowed to use:

- protocol constants;
- `MessageType`;
- `Frame`;
- `DecodeStatus`;
- the `encodeFrame()` declaration; and
- the `FrameDecoder` class declaration.

The header does not contain the main algorithms. A source file can include the
header without needing to know how the algorithms work.

### `src/protocol/protocol.cpp`

The implementation file defines the functions declared by the header. It owns:

- byte offsets inside the header;
- integer-to-byte and byte-to-integer helpers;
- validation rules;
- frame encoding; and
- stateful frame decoding.

### `tests/unit/protocol_tests.cpp`

The test file is a separate executable. It constructs known inputs, runs the
protocol code, and reports failure when the observed result differs from the
expected result. It does not open a network socket, so failures are isolated
from the client and server.

Build and run all registered unit tests with:

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

`ctest` runs test executables registered by CMake and reports a nonzero status
when any test fails. To compile this protocol test directly without CMake:

```bash
c++ -std=c++20 -Wall -Wextra -Wpedantic -Iinclude \
  src/protocol/protocol.cpp tests/unit/protocol_tests.cpp -o protocol_tests
./protocol_tests
```

- `c++` runs the C++ compiler and linker driver.
- `-std=c++20` selects the language version needed by `std::span`.
- `-Wall -Wextra -Wpedantic` enables useful compiler warnings.
- Both `.cpp` files are compiled and linked into one executable.
- `-o protocol_tests` selects the output filename.
- `./protocol_tests` runs that file from the current directory.

## 3. How headers and source files work

### `#pragma once`

```cpp
#pragma once
```

The preprocessor handles lines beginning with `#` before normal compilation.
`#pragma once` tells it to include this header only once in each `.cpp` file.
Without that protection, repeated includes could define the same type more than
once.

### `#include`

```cpp
#include "nexus/protocol.hpp"
#include <vector>
```

Quotes normally mean a project header should be searched for first. Angle
brackets normally mean a standard-library or system header.

Each included standard header provides a specific tool:

| Header | What this code gets from it |
| --- | --- |
| `<cstddef>` | `std::size_t`, the unsigned type used for sizes and indexes. |
| `<cstdint>` | Fixed-width integer types such as `std::uint8_t`. |
| `<span>` | A non-owning view over consecutive elements. |
| `<string>` | An owning sequence of characters for decoder errors. |
| `<string_view>` | A lightweight, non-owning view of text. |
| `<vector>` | A resizable, owning, contiguous array. |
| `<algorithm>` | `std::all_of()`, used to validate every name byte. |
| `<stdexcept>` | `std::runtime_error`, used by the test runner. |
| `<iostream>` | `std::cout` and `std::cerr` for test output. |

Including `nexus/protocol.hpp` first in `src/protocol/protocol.cpp` is useful:
it proves the header declares all types needed by its own public interface
instead of accidentally depending on another include.

### Declaration versus definition

This line in the header is a declaration:

```cpp
bool encodeFrame(const Frame& frame, std::vector<std::uint8_t>& output);
```

It tells the compiler the function's name, arguments, and return type. The body
in `src/protocol/protocol.cpp` is the definition. The linker later connects
calls from other translation units to that definition.

A **translation unit** is roughly one `.cpp` file after all of its includes have
been expanded.

## 4. Namespaces and name lookup

```cpp
namespace nexus::protocol {
```

A namespace groups names and avoids collisions. The full name of `Frame` is:

```cpp
nexus::protocol::Frame
```

The `::` operator selects a name inside a namespace, class, or enum.

Inside `src/protocol/protocol.cpp` there is another namespace:

```cpp
namespace {
```

This unnamed namespace gives its contents internal linkage. Helpers such as
`appendUint32()` are private to that translation unit and cannot collide with a
same-named function in another `.cpp` file.

The tests use declarations such as:

```cpp
using nexus::protocol::Frame;
```

This makes `Frame` available by its short name inside the test file. It does not
copy or redefine the type.

## 5. Constants and integer types

```cpp
inline constexpr std::uint8_t kProtocolVersion = 1;
inline constexpr std::size_t kFrameHeaderSize = 12;
inline constexpr std::uint32_t kMaxPayloadSize = 65'536;
```

### `constexpr`

`constexpr` says the value is a compile-time constant when used in a valid
constant expression. These protocol values never change while the program runs.

### `inline` on a variable

Because the constants are defined in a header, several `.cpp` files may include
them. C++17 inline variables allow those identical definitions without causing
a multiple-definition linker error.

### Fixed-width integers

- `std::uint8_t` is an unsigned integer with exactly 8 bits when the platform
  provides it. It represents one byte in this protocol.
- `std::uint16_t` holds the 16-bit flags field.
- `std::uint32_t` holds stream IDs and payload lengths.
- `std::size_t` is the standard type for in-memory sizes and container indexes.

The protocol uses fixed widths because the wire layout must be identical on
different machines. `int` is not used for wire fields because its size is not
the protocol's contract.

The apostrophe in `65'536` is a digit separator. It improves readability and
does not change the value.

## 6. Strongly typed message values

```cpp
enum class MessageType : std::uint8_t {
    Register = 1,
    RegisterOk = 2,
    Open = 3,
    Data = 4,
    Close = 5,
    Ping = 6,
    Pong = 7,
    Authenticate = 8,
    AuthenticateOk = 9,
    AuthenticateFailed = 10,
};
```

`enum class` creates a scoped, strongly typed enumeration:

- Scoped means the code writes `MessageType::Data`, not only `Data`.
- Strongly typed means it does not silently behave like an integer.
- `: std::uint8_t` selects the underlying storage type.

The explicit numeric values are part of the wire protocol. Reordering the
source lines must not silently change them.

Converting an enum to its byte value is explicit:

```cpp
const auto rawType = static_cast<std::uint8_t>(frame.type);
```

`static_cast<T>(value)` requests a checked-at-compile-time conversion to `T`.
It documents that the conversion is intentional.

## 7. The `Frame` structure

```cpp
struct Frame {
    MessageType type{MessageType::Data};
    std::uint32_t streamId{};
    std::vector<std::uint8_t> payload;
};
```

A `struct` groups related values. Its members are public by default.

The braces after members are default member initializers:

- `type` defaults to `MessageType::Data`.
- `streamId{}` value-initializes the integer to zero.
- `payload` uses `std::vector`'s default constructor and starts empty.

The type is also an aggregate, so tests can create one directly:

```cpp
Frame{MessageType::Data, 42, {0x00, 0xFF}}
```

This is list initialization. The values initialize members in declaration
order: type, stream ID, then payload.

`std::vector<std::uint8_t>` owns its bytes. It tracks its size, grows when
needed, and can safely contain `0x00`. It is not a C string and has no special
null terminator rule.

## 8. References, ownership, and views

### Constant reference input

```cpp
const Frame& frame
```

`&` in a declaration means reference. The function uses the caller's object
without copying it. `const` means the function cannot modify that object through
this reference.

### Non-constant reference output

```cpp
std::vector<std::uint8_t>& output
```

This also avoids a copy, but it is not `const`, so `encodeFrame()` can clear and
fill the caller's vector. The Boolean return value separately reports success.

### `std::span`

```cpp
std::span<const std::uint8_t> bytes
```

A span is a pointer-and-length view over consecutive elements. It does not own
or copy those bytes. `const` prevents modification through the span. The caller
must keep the underlying memory alive for the duration of the function call.

`append()` immediately copies the viewed bytes into `buffer_`, so the decoder
does not retain a dangling span after the call returns.

### `std::string_view`

`error()` returns `std::string_view`, a non-owning view of `error_`. This avoids
copying the error text. The returned view stays valid only while the decoder
object is alive and its `error_` string is not changed.

## 9. Classes and access control

```cpp
class FrameDecoder {
public:
    void append(...);
    DecodeStatus nextFrame(...);
    std::string_view error() const;

private:
    std::vector<std::uint8_t> buffer_;
    std::string error_;
};
```

A class combines behavior with state:

- `public` members form the interface callers may use.
- `private` members are implementation details callers cannot access directly.
- The trailing underscore is a naming convention for data members.

The `const` after `error()` means calling that member function does not modify
the observable state of the decoder:

```cpp
std::string_view FrameDecoder::error() const
```

The `FrameDecoder::` prefix on a definition says the function belongs to the
`FrameDecoder` class.

## 10. Writing integers in network byte order

Network byte order is big-endian: the most significant byte comes first.

Consider the 32-bit value `0x01020304`. The encoded bytes must be:

```text
01 02 03 04
```

The encoder extracts each byte with shifts and masks:

```cpp
static_cast<std::uint8_t>((value >> 24) & 0xFF)
```

Read it from inside out:

1. `value >> 24` shifts the desired high byte into the lowest position.
2. `& 0xFF` keeps only the lowest eight bits.
3. `static_cast<std::uint8_t>` stores those bits as one byte.

Important operators:

- `>>` shifts bits to the right.
- `<<` shifts bits to the left.
- `&` between values is bitwise AND and keeps selected bits.
- `|` is bitwise OR and combines non-overlapping groups of bits.
- `0xFF` is hexadecimal for binary `11111111`, or decimal 255.

The decoder reverses the process:

```cpp
(static_cast<std::uint32_t>(input[offset]) << 24) |
(static_cast<std::uint32_t>(input[offset + 1]) << 16) |
(static_cast<std::uint32_t>(input[offset + 2]) << 8) |
 static_cast<std::uint32_t>(input[offset + 3]);
```

Each byte is widened to 32 bits before shifting. Widening first prevents losing
bits that cannot fit in an 8-bit value. OR combines the four positioned bytes
into one integer.

Writing fields manually also avoids two unsafe assumptions:

- the host computer might use a different byte order; and
- a compiler may insert padding into a C++ struct.

## 11. Validation rules

`isKnownMessageType()` checks that a raw byte lies between the first and last
currently assigned type values. This works because version 1 assigns the ten
types consecutive values from 1 through 10. If future values contain gaps, this
helper should become an explicit `switch`.

`validateFields()` returns an empty `std::string_view` when fields are valid and
an explanatory view when they are not. Its `switch` chooses rules based on the
message type.

Several `case` labels may share one implementation:

```cpp
case MessageType::Open:
case MessageType::Close:
```

There is no `break` because every path inside the case returns from the whole
function. `return {}` constructs an empty `std::string_view`.

The main structural rules are:

- registration and heartbeat frames use control stream ID `0`;
- stream frames use a nonzero stream ID;
- `OPEN` and `CLOSE` have empty payloads;
- `DATA` has at least one byte;
- `PING` and `PONG` have exactly eight bytes; and
- `AUTH` contains 32–256 bytes while its responses are empty; and
- no payload exceeds 65,536 bytes.

`isValidTunnelName()` uses `std::all_of()`:

```cpp
std::all_of(payload.begin(), payload.end(), [](std::uint8_t byte) {
    // Return true when this byte is allowed.
});
```

`begin()` and `end()` describe a half-open range: the first element is included,
and the end position is one past the last element. `std::all_of()` returns true
only if the lambda returns true for every byte. An empty range is considered
valid, which matches the optional empty tunnel name.

The `[]` starts a lambda expression. Empty brackets mean the lambda captures no
variables from its surrounding scope.

## 12. `encodeFrame()` step by step

The encoder's contract is:

```cpp
bool encodeFrame(const Frame& frame, std::vector<std::uint8_t>& output);
```

- Return `true` and fill `output` for a valid frame.
- Return `false` and leave `output` empty for an invalid frame.

### Step 1: Clear old output

```cpp
output.clear();
```

`clear()` removes elements but may keep allocated capacity for reuse. Clearing
first means a failed encoding cannot accidentally leave old bytes that a caller
might send.

### Step 2: Validate before narrowing

```cpp
const auto rawType = static_cast<std::uint8_t>(frame.type);
if (!isKnownMessageType(rawType) || frame.payload.size() > kMaxPayloadSize) {
    return false;
}
```

`auto` asks the compiler to infer the type from the initializer. Here it becomes
`std::uint8_t`.

`!` means logical NOT. `||` means logical OR and short-circuits: if the left
condition is already true, the right condition does not need to run.

`frame.payload.size()` returns `std::size_t`. The maximum check happens before
converting the size to `std::uint32_t`, so a huge value cannot be silently
truncated during conversion.

### Step 3: Apply message-specific rules

```cpp
const auto payloadLength = static_cast<std::uint32_t>(frame.payload.size());
if (!validateFields(frame.type, frame.streamId, payloadLength).empty()) {
    return false;
}
```

`.empty()` asks whether a container or view has zero elements. A nonempty error
message means validation failed.

The next condition uses `&&`, logical AND. Its right side runs only if the frame
is a registration type requiring name validation:

```cpp
if (requiresTunnelNameValidation(frame.type) &&
    !isValidTunnelName(frame.payload)) {
    return false;
}
```

### Step 4: Build the exact output

```cpp
output.reserve(kFrameHeaderSize + frame.payload.size());
```

`reserve()` changes capacity, not size. It asks the vector to allocate enough
space in advance so repeated `push_back()` calls are less likely to reallocate.
It does not create initialized elements. By contrast, `resize()` changes the
number of elements and would create bytes immediately.

The header is then written in field order:

```cpp
output.push_back(kProtocolVersion);
output.push_back(rawType);
appendUint16(output, 0);
appendUint32(output, frame.streamId);
appendUint32(output, payloadLength);
```

`push_back()` adds one element to the end. The helper calls add the multi-byte
fields. Finally:

```cpp
output.insert(output.end(), frame.payload.begin(), frame.payload.end());
```

This inserts the half-open payload range at the output's end.

### Concrete encoded example

For:

```cpp
Frame{MessageType::Data, 42, {0x00, 0xFF}}
```

the bytes are:

```text
01 04 00 00 00 00 00 2A 00 00 00 02 00 FF
```

| Bytes | Meaning |
| --- | --- |
| `01` | Protocol version 1. |
| `04` | `DATA` message type. |
| `00 00` | Reserved flags are zero. |
| `00 00 00 2A` | Stream ID 42. |
| `00 00 00 02` | Payload length 2. |
| `00 FF` | The two payload bytes. |

The encoded size is 14 bytes: 12 header bytes plus 2 payload bytes.

## 13. Why the decoder must keep state

TCP is a byte stream. It does not preserve the boundaries of calls to `send()`.
If the sender writes one 17-byte frame, the receiver might observe:

```text
recv #1: 5 bytes
recv #2: 8 bytes
recv #3: 4 bytes
```

It might also receive two frames together:

```text
recv #1: complete OPEN frame + complete DATA frame
```

Therefore, a local variable inside one `recv()` handler is not enough. The
decoder object owns `buffer_`, which survives across calls.

## 14. `FrameDecoder::append()` step by step

```cpp
void FrameDecoder::append(std::span<const std::uint8_t> bytes) {
    if (!error_.empty()) {
        return;
    }
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
}
```

If the decoder has already found malformed data, it ignores later input. This is
a **sticky error**. The caller should report the error and close the tunnel.

Trying to scan for a new frame after corruption would be unsafe because any
payload byte could coincidentally look like a version or message-type byte.

For a healthy decoder, `insert()` copies every supplied byte to the end of the
owned buffer. Earlier incomplete bytes remain at the front.

## 15. `FrameDecoder::nextFrame()` step by step

`nextFrame()` is a small state machine. It does not block or call `recv()`; it
only examines bytes already supplied through `append()`.

### State 1: Existing error

```cpp
if (!error_.empty()) {
    return DecodeStatus::ProtocolError;
}
```

This preserves the sticky error behavior.

### State 2: Incomplete header

```cpp
if (buffer_.size() < kFrameHeaderSize) {
    return DecodeStatus::NeedMoreData;
}
```

The code never reads a header offset until all 12 header bytes exist. This
prevents out-of-bounds access.

### State 3: Validate header identity

The decoder checks:

1. protocol version;
2. raw message type; and
3. reserved flags.

The raw type is validated before converting it to `MessageType`. This matters
because a network peer can send any byte, including values that the enum does
not define.

### State 4: Read integer fields

```cpp
const std::uint32_t streamId = readUint32(buffer_, kStreamIdOffset);
const std::uint32_t payloadLength =
    readUint32(buffer_, kPayloadLengthOffset);
```

The named offsets make the wire layout visible instead of scattering unexplained
numbers such as `4` and `8` throughout the decoder.

### State 5: Validate before trusting the length

```cpp
const std::string_view fieldError =
    validateFields(type, streamId, payloadLength);
```

The maximum and per-message lengths are checked immediately after the fixed
header is available. The decoder does not allocate a payload based on an
untrusted oversized length.

### State 6: Wait for the complete frame

```cpp
const std::size_t frameSize =
    kFrameHeaderSize + static_cast<std::size_t>(payloadLength);
if (buffer_.size() < frameSize) {
    return DecodeStatus::NeedMoreData;
}
```

A valid header is not necessarily a complete frame. Returning
`NeedMoreData` preserves the buffer until the remaining payload arrives.

### State 7: Validate payload contents

Tunnel-name characters cannot be checked until the full payload exists. The
decoder creates a temporary span pointing at the payload bytes and validates
them without making an extra copy.

Pointer arithmetic appears here:

```cpp
buffer_.data() + kFrameHeaderSize
```

`data()` returns a pointer to the first vector element. Adding 12 moves the
pointer to the first payload byte. The span also receives `payloadLength`, so it
knows the valid range.

### State 8: Produce and consume one frame

```cpp
output.type = type;
output.streamId = streamId;
output.payload.assign(payloadBegin, payloadEnd);
buffer_.erase(buffer_.begin(), payloadEnd);
```

`assign()` replaces the output payload with a copy of the decoded payload range.
`erase()` removes the processed header and payload but keeps bytes belonging to
later frames.

The decoder returns exactly one frame per call. This gives the caller a simple
loop and allows several buffered frames to be handled in order.

## 16. How a caller should use the decoder

The client and server follow this pattern after `recv()` returns a positive byte
count:

```cpp
decoder.append(std::span<const std::uint8_t>(buffer, bytesReceived));

while (true) {
    Frame frame;
    const DecodeStatus status = decoder.nextFrame(frame);

    if (status == DecodeStatus::FrameReady) {
        handleFrame(frame);
        continue;
    }

    if (status == DecodeStatus::ProtocolError) {
        std::cerr << decoder.error() << '\n';
        close(tunnelSocket);
    }

    break; // NeedMoreData or ProtocolError
}
```

This is shortened example code. The real `apps/client_main.cpp` and
`apps/edge_main.cpp` use the same append-and-drain loop with their stream maps
and cleanup rules.

The three statuses mean:

| Status | Meaning | Caller action |
| --- | --- | --- |
| `FrameReady` | One complete valid frame was removed from the buffer. | Handle it and call again. |
| `NeedMoreData` | No error, but the next frame is incomplete. | Return to `poll()`/`recv()`. |
| `ProtocolError` | The buffered stream violates the protocol. | Log the reason and close the tunnel. |

## 17. Structural validation versus connection state

The current decoder validates the structure of one frame:

- version;
- known type;
- flags;
- stream ID being zero or nonzero as required;
- payload length; and
- registration-name characters.

It does not yet know whether the program is acting as the client or edge, and it
does not store the set of open stream IDs. Therefore, later integration code
must still reject:

- messages traveling in the wrong direction;
- `DATA` before `OPEN`;
- duplicate `OPEN` frames;
- frames for unknown or closed streams; and
- `PONG` data that does not match an outstanding `PING`.

This separation is intentional. Byte parsing belongs to the protocol decoder;
connection lifecycle belongs to the client and edge state machines.

## 18. Complexity and performance

For a payload of `n` bytes:

- Encoding is `O(n)` because the payload is copied once.
- Decoding a frame is `O(n)` because its payload is copied into `Frame`.
- `buffer_.erase(buffer_.begin(), payloadEnd)` may also move remaining bytes,
  making repeated front erases inefficient when many frames are buffered.

The current implementation is clear and correct for the learning milestone.
If measurement later shows front erasing is expensive, common improvements are:

- keep a read offset and occasionally compact the vector;
- use a ring buffer; or
- use a segmented buffer.

Do not optimize this before profiling shows it matters.

The decoder itself does not expose a separate total-buffer setting. The frame
length limit bounds one declared payload to 64 KiB, while the relay module now
places explicit limits on queued socket output and pauses producers when the
shared tunnel queue approaches its limit.

## 19. How the test program works

The tests deliberately avoid a testing framework so their mechanics remain
visible.

### `expect()`

```cpp
void expect(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}
```

If a condition is false, `throw` stops the current test and creates an exception
containing the failure message. `std::string(message)` makes an owning string
from the non-owning view because `runtime_error` stores its message.

### `try` and `catch`

`main()` runs every test inside a `try` block. A failed expectation transfers
control to:

```cpp
catch (const std::exception& error)
```

The catch parameter is a constant reference, avoiding an exception-object copy
and preserving polymorphic behavior. `error.what()` returns its message. The
program returns `1` for failure and `0` when every test finishes.

### Round-trip testing

A round trip checks:

```text
original Frame -> encode -> decode -> resulting Frame
```

`expectSameFrame()` compares the type, stream ID, and payload. `std::vector`
equality checks both size and each element in order.

### Empty payload test

`REGISTER` allows an empty payload. The test verifies that its encoded size is
exactly the 12-byte header and that decoding preserves the empty vector.

### Binary payload test

The payload contains:

```text
00 01 7F 80 FF
```

This proves the protocol uses explicit lengths rather than stopping at `0x00`
as a C string would. Values above `0x7F` also prove the data is treated as
unsigned bytes instead of text characters.

### Split-frame test

The test constructs a 17-byte frame and appends it in three parts. It expects
`NeedMoreData` after the first two parts and `FrameReady` only after the last.

This expression creates a view into part of a vector:

```cpp
std::span<const std::uint8_t>(bytes.data() + 5, 8)
```

- `bytes.data() + 5` points at element 5.
- `8` is the number of elements in the view.
- No bytes are copied merely to create the span.

### Combined-frame test

Two encoded vectors are joined with `insert()`, then supplied through one
`append()`. Two calls to `nextFrame()` must return the two original frames in
order. A third call must return `NeedMoreData`.

### Malformed-input tests

Tests first create a valid encoding, then deliberately mutate individual header
bytes. This isolates one rule at a time:

- byte 0 tests version rejection;
- byte 1 tests unknown message rejection;
- bytes 2–3 test reserved flags;
- bytes 8–11 test the payload length.

`writeUint32()` exists only in the test file to place a deliberately invalid
length in network byte order.

### Sanitizers

Configure a separate sanitized build tree and run every registered test with:

```bash
cmake -S . -B build-sanitized -DNEXUS_ENABLE_SANITIZERS=ON
cmake --build build-sanitized
ctest --test-dir build-sanitized --output-on-failure
```

The equivalent direct compiler command for the protocol test alone is:

```bash
c++ -std=c++20 -Wall -Wextra -Wpedantic -Iinclude \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  src/protocol/protocol.cpp tests/unit/protocol_tests.cpp -o protocol_tests
./protocol_tests
```

AddressSanitizer helps find invalid memory access and lifetime bugs.
UndefinedBehaviorSanitizer helps find operations for which C++ defines no valid
behavior, such as certain invalid shifts or overflows.

Sanitizers improve confidence but do not prove the absence of all bugs.

## 20. Syntax and operator reference for this code

| Syntax | Meaning here |
| --- | --- |
| `#include` | Makes declarations from another header available. |
| `#pragma once` | Prevents repeated inclusion of one header per translation unit. |
| `namespace a::b` | Places names inside nested namespaces. |
| `namespace {}` | Gives helpers translation-unit-only visibility. |
| `std::` | Selects a name from the C++ standard-library namespace. |
| `using a::b` | Makes one existing name available by a shorter name. |
| `constexpr` | Declares a value usable at compile time. |
| `inline` | Allows a header-defined entity to appear identically in several translation units. |
| `const` | Prevents modification through that name or reference. |
| `auto` | Asks the compiler to infer a type from the initializer. |
| `enum class` | Creates a scoped, strongly typed set of named values. |
| `struct` | Defines a type whose members are public by default. |
| `class` | Defines a type whose members are private by default. |
| `public:` | Begins the caller-visible part of a class. |
| `private:` | Begins the class-only implementation state. |
| `&` in a declaration | Creates a reference instead of a copy. |
| `&` in an expression | Performs bitwise AND. |
| `&&` | Logical AND with short-circuit evaluation. |
| `|` | Performs bitwise OR. |
| `||` | Logical OR with short-circuit evaluation. |
| `!` | Negates a Boolean condition. |
| `!=` / `==` | Compare values for inequality/equality. |
| `<`, `>`, `<=` | Compare ordered values. |
| `<<` / `>>` | Shift bits left or right. |
| `static_cast<T>(x)` | Explicitly converts `x` to type `T`. |
| `object.member` | Accesses a member through an object or reference. |
| `pointer + n` | Points `n` elements after the original pointer. |
| `container[index]` | Accesses an element without bounds checking. |
| `{...}` | Performs list initialization or defines a block. |
| `{}` as a value | Value-initializes, often producing zero or an empty object. |
| `()` | Calls a function or groups an expression. |
| `[](...) {}` | Defines a lambda expression. |
| `;` | Ends a declaration or statement. |
| `return` | Ends a function and optionally provides its result. |
| `if` / `else` | Selects code based on conditions. |
| `switch` / `case` | Selects code based on an enum value. |
| `try` / `catch` | Runs code and handles thrown exceptions. |
| `throw` | Reports an exceptional failure and transfers control. |

Literal notation used by the code:

- `42` is an ordinary decimal integer literal.
- `65'536` is decimal with a readability separator.
- `0xFF` is a hexadecimal integer literal.
- `'h'` is a character literal representing one character value.
- `"message"` is a string literal containing a terminating null byte in its
  compiled storage.
- `\0` represents a zero byte and `\n` represents a newline escape sequence.
- `//` begins a comment that continues to the end of the source line.

Important function syntax:

```cpp
ReturnType functionName(ParameterType parameterName) {
    // body
}
```

The return type tells callers what result to expect. Parameters receive inputs
or references to caller-owned objects. Local variables exist until their scope's
closing brace. A temporary such as `Frame{MessageType::Open, 1, {}}` passed to a
`const Frame&` remains alive through that function call.

`main()` is the program entry point. Returning zero conventionally means
success; a nonzero result means failure. `std::cout` writes normal output,
`std::cerr` writes diagnostic output, and `std::endl` writes a newline and
flushes the stream.

## 21. Common mistakes this implementation avoids

### Sending a C++ struct directly

Incorrect idea:

```cpp
send(socket, &frame, sizeof(frame), 0);
```

`Frame` contains a `std::vector`, whose object stores bookkeeping rather than
the payload inline. Struct padding, object representation, pointer values, and
host byte order also make this unusable as a wire format.

### Assuming one `recv()` equals one frame

TCP may split or combine bytes. Always preserve incomplete bytes and repeatedly
decode complete frames.

### Allocating before validating the length

A peer controls header bytes. Reject `payloadLength > kMaxPayloadSize` before
using that value to reserve or allocate payload memory.

### Treating payload data as text

`DATA` may contain `0x00` and invalid UTF-8. Use a byte vector and explicit
length, not `strlen()` or null termination.

### Forgetting network byte order

Writing a native integer's memory representation makes the protocol dependent
on the sender's CPU. Define one byte order and encode it explicitly.

### Confusing `reserve()` with `resize()`

`reserve(100)` creates capacity for 100 elements but leaves size unchanged.
`resize(100)` makes the vector contain 100 elements.

### Continuing after a malformed frame

Without an explicit synchronization marker, the next byte cannot safely be
assumed to start a frame. Report the protocol error and close the tunnel.

### Storing a span or string view too long

Both are non-owning. They become invalid when the referenced storage dies or is
reallocated. Copy data when it must outlive the view.

### Performing shifts in an 8-bit type

Convert each input byte to `std::uint32_t` before shifting it left. Otherwise,
the desired bits may not fit in the original type.

## 22. Interview questions and strong answers

### Why does a TCP application need framing?

TCP provides an ordered stream, not application message boundaries. A receiver
may observe partial messages or several messages in one read. Framing adds
metadata—here a fixed header and payload length—so the receiver can recover
complete logical messages.

### How does this decoder handle a partial frame?

It appends each received chunk to an owned buffer. It waits for the fixed header,
parses the payload length, then waits for `header size + payload length` bytes.
Until then it returns `NeedMoreData` without discarding anything.

### How does it handle several frames in one read?

It removes only the bytes belonging to the first decoded frame. Remaining bytes
stay buffered, and the caller repeatedly invokes `nextFrame()` until it returns
`NeedMoreData`.

### Why use network byte order?

Different CPU architectures may store multi-byte integers differently. A fixed
big-endian wire order ensures every participant interprets the same bytes as the
same number.

### Why not transmit a packed struct?

Packed structs still create portability and maintenance risks involving layout,
alignment, compiler behavior, byte order, and nontrivial members. Explicit
serialization makes every wire byte intentional and testable.

### Why validate length before allocation?

The length comes from an untrusted peer. Allocating first could enable memory
exhaustion or overflow. This decoder rejects lengths above 64 KiB immediately
after reading the 12-byte header.

### Why use `enum class`?

It scopes enumerator names and prevents accidental implicit conversion to or
from integers. Wire conversion remains explicit at the serialization boundary.

### What is the difference between `vector` and `span` here?

`vector` owns resizable storage. `span` is a temporary non-owning pointer-and-size
view. `Frame` and `FrameDecoder` own long-lived bytes with vectors; `append()`
accepts a span so callers can provide any contiguous byte buffer without a
preliminary copy.

### Why return a status enum instead of only `bool`?

The decoder has three meaningful outcomes: a frame is ready, more bytes are
needed, or the input is invalid. A Boolean could represent only two and would
make incomplete data easy to confuse with an error.

### Why is a protocol error sticky?

After malformed framing, the receiver cannot reliably know where a later frame
starts. Keeping the decoder failed prevents ambiguous or attacker-controlled
resynchronization.

### What is the time complexity?

Encoding and payload copying are linear in payload size. The current decoder's
front erase may move remaining buffered bytes, so workloads containing many
coalesced frames could incur extra linear movement. A read offset or ring buffer
would improve that if profiling justified it.

### Does the decoder completely secure the protocol?

No. It validates individual frame structure. The connection layer must enforce
direction, registration state, active stream IDs, queue limits, timeouts, TLS,
and authentication.

### Why can a payload contain `\0`?

The payload is represented by a byte vector with an explicit size. `\0` only
has special terminating meaning in C-style strings; it is an ordinary byte in
binary data.

### What is the difference between `clear()`, `reserve()`, and `resize()`?

`clear()` removes elements, `reserve()` increases capacity without adding
elements, and `resize()` changes the number of elements.

### Why return `std::string_view` from `error()`?

The decoder already owns the error string. Returning a view avoids a copy while
still preventing callers from modifying it. The caller must not keep the view
after the decoder is destroyed or its error string changes.

## 23. How to reproduce the encoder in an interview

Start with the contract, not bit manipulation:

1. Define the fixed header fields and maximum payload.
2. Clear the output so failure cannot leak stale data.
3. Validate the type, stream ID, and payload size.
4. Reserve `header size + payload size` bytes.
5. Append one-byte fields directly.
6. Append multi-byte integers in big-endian order.
7. Append exactly the payload's bytes.
8. Return success.

Pseudocode:

```text
encode(frame, output):
    clear output
    if frame fields are invalid:
        return false

    reserve HEADER_SIZE + payload.size
    append VERSION
    append message type
    append flags as big-endian 16-bit integer
    append stream ID as big-endian 32-bit integer
    append payload length as big-endian 32-bit integer
    append payload
    return true
```

## 24. How to reproduce the decoder in an interview

Explain that the decoder is incremental and stateful:

1. Keep an owned byte buffer across calls.
2. Append every new chunk.
3. If fewer than 12 bytes exist, request more.
4. Parse and validate the fixed header.
5. Reject oversized or structurally invalid lengths immediately.
6. Compute the full frame size safely.
7. If the full payload is missing, request more.
8. Copy one frame into the output.
9. Consume exactly that frame's bytes.
10. Let the caller ask again for additional buffered frames.

Pseudocode:

```text
nextFrame(output):
    if decoder already failed:
        return ProtocolError

    if buffer has less than HEADER_SIZE:
        return NeedMoreData

    parse header
    if header is invalid:
        remember error
        return ProtocolError

    total = HEADER_SIZE + payload_length
    if buffer has less than total:
        return NeedMoreData

    copy type, stream ID, and payload into output
    remove exactly total bytes from buffer
    return FrameReady
```

## 25. What to study or implement next

To make this knowledge stick, try these exercises without copying the solution:

1. Write `appendUint32()` on paper for `0x12345678` and list the four bytes.
2. Recreate `readUint32()` and explain why each cast happens before its shift.
3. Add a test for the largest valid 65,536-byte `DATA` payload.
4. Add tests for invalid tunnel-name characters and invalid heartbeat lengths.
5. Replace front erasing with a read offset while keeping all tests passing.
6. Integrate the decoder into one side of the tunnel using the status loop above.
7. Add connection-state validation for `OPEN`, `DATA`, and `CLOSE` ordering.

The current project task is narrower: assign each accepted public connection a
unique nonzero stream ID. That ID will eventually be placed in `OPEN`, `DATA`,
and `CLOSE` frames so multiplexed connections do not mix their bytes.
