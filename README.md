# Ackord

Ackord is a C11 library for ordered command processing, acknowledgements, and
in-memory journaling. It provides server and client APIs with bounded queues,
duplicate detection, and synchronous batch processing.

## Build

Requires CMake 3.24 or newer and a C11 compiler.

```sh
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug --config Debug
ctest --test-dir build/debug --build-config Debug --output-on-failure
```

For a Release build, use `build/release` and `Release` in place of `build/debug`
and `Debug`.

The build options default to `ON` for standalone builds and `OFF` when Ackord is
included as a subdirectory:

| Option | Purpose |
| --- | --- |
| `ACKORD_BUILD_TESTS` | Build the API and session tests. |
| `ACKORD_BUILD_EXAMPLES` | Build the counter example. |
| `ACKORD_INSTALL` | Generate installation and package files. |

## Integration

Add Ackord to a build with its CMake target:

```cmake
add_subdirectory(ackord)
target_link_libraries(app PRIVATE ackord::ackord)
```

For an installed package:

```sh
cmake --install build/debug --config Debug --prefix build/install
```

```cmake
find_package(ackord CONFIG REQUIRED)
target_link_libraries(app PRIVATE ackord::ackord)
```

Set `CMAKE_PREFIX_PATH` to the installation prefix when locating the package.
Include `<ackord/ackord.h>` from C or `<ackord/ackord.hpp>` from C++.

## Configuration

Supply queue limits and both allocator callbacks when creating a server or
client:

```c
ackord_config config = {
    .max_clients = 16,
    .max_records = 1024,
    .max_payload_size = 256,
    .max_result_size = 256,
    .sequence_window = 64,
    .allocator = { allocate, deallocate, context }
};
```

`allocate(user, size)` returns suitably aligned memory or `NULL`.
`deallocate(user, memory)` receives a pointer returned by that allocator.
Keep the callbacks and their context valid until the object is destroyed.
An incomplete configuration returns `ACKORD_INVALID`.

Server creation reserves command, result, processing, and identity-index storage.
Client creation reserves pending records and payload storage; `max_result_size`
does not increase its storage, and a zero payload limit needs no byte buffer.
Submitting and processing commands do not allocate. Retained commands use a
bounded hash index, and free record slots are reused without scanning the queue.

## Command flow

1. Create a server and client with `ackord_server_create` and
   `ackord_client_create`. Start a nonzero session with their reset functions
   and register a nonzero client ID with `ackord_server_register`.
2. Call `ackord_client_submit` to copy a payload into the pending queue and assign
   its sequence number. Iterate `ackord_client_pending_next` from sequence zero
   to collect pending commands for sending or retrying.
3. Pass received commands to `ackord_server_submit`. Each command carries its
   session, client, sequence, scheduled step, type, and payload.
4. Call `ackord_server_process_checked` with the current step and a batch handler.
   The handler fills each command's outcome and returns `ACKORD_OK` to complete
   the batch. It is called even for an empty batch.
5. Read results with `ackord_server_confirmation` and apply them with
   `ackord_client_confirm` to release pending commands.

The [counter example](examples/counter.c) demonstrates command submission,
processing, a result, acknowledgement, and a retry that does not increment
the counter twice. The complete API is in [ackord.h](include/ackord/ackord.h).

## Ordering and acknowledgements

| Ordering mode | Execution order |
| --- | --- |
| `ACKORD_ORDER_BY_STEP` | Scheduled step, client ID, then sequence. This is the default. |
| `ACKORD_ORDER_BY_SEQUENCE` | Each client's next consecutive sequence must be present and due. Eligible commands are ordered by step, client ID, then sequence. |

Select a mode before submitting commands:

```c
ackord_server_set_ordering( server, ACKORD_ORDER_BY_SEQUENCE );
```

In sequence mode, a gap or future command blocks only its own client. The mode
can change only before pending commands or processing history exist, and is
retained across session resets.

A confirmation means processing completed with a result or terminal refusal.
The cumulative confirmation advances only through consecutive completed
sequences. Missing and future commands remain unconfirmed when a later command
completes.

`ackord_server_completed_mask` supplies selective completion bits beyond that
prefix; bit zero represents `prefix + 1`. Apply both with
`ackord_client_confirm_progress`. Progress updates are validated before any
pending command is released.

## Duplicates and journal

An identical retained command returns `ACKORD_DUPLICATE` and is not executed
again. Changed contents for the same identity return `ACKORD_CONFLICT`.

`ackord_server_lookup` reads a retained pending or completed command using its
session, client, and sequence. Pending confirmations are zeroed. An unknown
identity returns `ACKORD_NOT_FOUND`.

Read completed journal entries with `ackord_server_journal_next`, then release
them explicitly with `ackord_server_journal_trim`. Trimming also removes cached
results. A retry whose completed identity is tracked but whose record was
trimmed returns `ACKORD_STALE` and is not executed again.

Journal lookup is constant time for any cursor. Reading all retained entries and
trimming a prefix take time proportional to the number of entries visited.

## Sessions and streams

A fresh session clears peers, pending commands, journal entries, and faults.
Use a fresh nonzero session ID for each new session.

Repeating the current server session ID returns `ACKORD_DUPLICATE` and preserves
its state, including faults. Repeating a client's current session and ID
preserves its queue and next sequence.

`ackord_server_suspend` preserves a stream's pending commands and completion
progress. Suspended streams reject submissions and are excluded from processing.
`ackord_server_resume` continues the same stream.

`ackord_server_unregister` cancels pending work and permanently retires the client
ID for that session. Completed records remain readable until trimmed or reset.
Retired IDs retain their slots, so `max_clients` limits the total distinct client
IDs in a session. A fresh session releases those slots.

## Handler outcomes

The handler sets each outcome's `status` and `result_size`. Status zero
conventionally means success; other values are terminal refusals.
`INT32_MIN` is reserved. Write result bytes into the supplied buffer without
replacing its pointer or capacity.

Confirmations and journal entries become visible after the handler returns
successfully and every outcome is validated. `ackord_server_process` is also
available for handlers without a return value.

A failed handler or invalid outcome returns `ACKORD_HANDLER_FAILED` and faults
the server. The batch stays pending and publishes no confirmations or journal
entries. Earlier completed commands remain readable. Further processing and
mutation return `ACKORD_FAULTED` until a reset with a fresh session ID.

Handler side effects are not rolled back. Do not mutate or destroy the server
from its handler, or let an exception unwind through the C API.

## Limits and ownership

- `ackord_client_clear` discards pending commands and clears the client identity
  while retaining its allocations. Reset the client before submitting again.
  Reset may reuse the previous session and client ID and starts at sequence one;
  coordinate this with the server's participant and session lifecycle. Repeated
  reset without clearing preserves pending commands and returns
  `ACKORD_DUPLICATE`.
- `sequence_window` is between 1 and 64 unresolved sequence numbers beyond the
  completed prefix. Commands outside the window return `ACKORD_OUT_OF_WINDOW`.
- Pending commands and completed journal entries share `max_records` slots.
  A full queue returns `ACKORD_FULL` without accepting another command.
- Payload and result sizes and capacities use `uint32_t`, up to `UINT32_MAX`
  bytes. Allocator sizes and collection counts use `size_t`. Payload and result
  limits may individually be zero if their sum is nonzero.
- Command and result views borrow library storage. Copy bytes that must outlive
  record removal, trimming, reset, or destruction.
- All access, including reads, must be serialized by the caller. A step may be
  processed repeatedly while paused.
- The journal is held in memory. Confirmations do not imply durable storage,
  and command history is not recovered after a process restart.

## License

[MIT](LICENSE).
