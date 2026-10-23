# NexusProxy Tunnel Protocol

## Purpose

NexusProxy uses one persistent TCP connection between the developer client and
the edge. TCP only provides an ordered stream of bytes, so this protocol divides
that stream into frames. Frames tell both programs whether the bytes open a
public stream, carry application data, close a stream, or manage the tunnel.

This document defines protocol version 1. It is a design specification; the
encoder and decoder will be implemented separately.

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

1. Add shared frame types and constants in `protocol.hpp`.
2. Encode complete frames in `protocol.cpp`.
3. Add a stateful decoder that accepts arbitrary byte chunks.
4. Unit-test split frames, combined frames, binary payloads, and invalid input.
5. Replace the current one-byte `'O'` signal with `REGISTER`, `REGISTER_OK`, and
   `OPEN`.
6. Wrap relayed bytes in `DATA` frames and replace socket-only closure with
   `CLOSE` frames.

## Version 1 limitations

- The protocol is not encrypted or authenticated yet. It is not safe to expose
  to untrusted networks until the security phase is complete.
- Registration rejection has no detailed error response.
- Stream priority, compression, and half-close are not supported.
- Queue limits and backpressure belong to the multiplexing phase.
