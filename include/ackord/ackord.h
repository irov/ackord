// clang-format Language: C
#ifndef ACKORD_ACKORD_H
#define ACKORD_ACKORD_H

#include <stddef.h>
#include <stdint.h>

typedef enum ackord_status {
    ACKORD_OK = 0,
    ACKORD_DUPLICATE,
    ACKORD_NOT_FOUND,
    ACKORD_INVALID,
    ACKORD_NO_MEMORY,
    ACKORD_FULL,
    ACKORD_WRONG_SESSION,
    ACKORD_OUT_OF_WINDOW,
    ACKORD_STALE,
    ACKORD_CONFLICT,
    ACKORD_BUSY,
    ACKORD_SUSPENDED,
    ACKORD_RETIRED,
    ACKORD_HANDLER_FAILED,
    ACKORD_FAULTED
} ackord_status;

typedef enum ackord_ordering {
    ACKORD_ORDER_BY_STEP = 0,
    ACKORD_ORDER_BY_SEQUENCE
} ackord_ordering;

typedef struct ackord_allocator {
    void * ( *allocate )( void * _user, size_t _size );
    void ( *deallocate )( void * _user, void * _memory );
    void * user;
} ackord_allocator;

/* All limits and both allocator callbacks must be supplied by the application. */
typedef struct ackord_config {
    size_t max_clients;
    size_t max_records;
    uint32_t max_payload_size;
    uint32_t max_result_size;
    /* 1..64 unresolved sequence numbers beyond each client's completed prefix. */
    uint32_t sequence_window;
    ackord_allocator allocator;
} ackord_config;

typedef struct ackord_command {
    uint64_t session;
    uint32_t client;
    uint64_t sequence;
    uint64_t step;
    uint32_t type;
    const void * payload;
    uint32_t payload_size;
} ackord_command;

typedef struct ackord_confirmation {
    uint64_t session;
    uint32_t client;
    uint64_t sequence;
    uint64_t step;
    int32_t status;
    const void * result;
    uint32_t result_size;
} ackord_confirmation;

/* The library supplies result/capacity. Set status and result_size; never replace
 * result or capacity. A nonzero application status is a terminal rejection, not
 * a retry. INT32_MIN is reserved and must not be supplied by a handler. */
typedef struct ackord_outcome {
    int32_t status;
    void * result;
    uint32_t result_size;
    uint32_t result_capacity;
} ackord_outcome;

typedef struct ackord_journal_entry {
    uint64_t index;
    ackord_command command;
    ackord_confirmation confirmation;
} ackord_journal_entry;

typedef enum ackord_command_state {
    ACKORD_COMMAND_PENDING = 1,
    ACKORD_COMMAND_COMPLETED
} ackord_command_state;

typedef struct ackord_command_view {
    ackord_command_state state;
    ackord_command command;
    /* Zeroed for a pending command. All byte views borrow server storage. */
    ackord_confirmation confirmation;
} ackord_command_view;

typedef struct ackord_server ackord_server;
typedef struct ackord_client ackord_client;
typedef void ( *ackord_handler )( void * _user, const ackord_command * _commands, ackord_outcome * _outcomes, size_t _count );
/* Return ACKORD_OK to commit the batch. Any other return faults the server. */
typedef ackord_status ( *ackord_checked_handler )( void * _user, const ackord_command * _commands, ackord_outcome * _outcomes, size_t _count );

ackord_status ackord_server_create( const ackord_config * _config, ackord_server ** _out );
void ackord_server_destroy( ackord_server * _server );
/* A fresh session clears retired IDs and faults. Repeating the current ID is a
 * no-op returning ACKORD_DUPLICATE, including while faulted. */
ackord_status ackord_server_reset( ackord_server * _server, uint64_t _session );
/* Defaults to BY_STEP. Change only with no pending commands or processing history.
 * BY_SEQUENCE waits for each client's next sequence without blocking others;
 * due stream heads are chosen by step, client, then sequence. */
ackord_status ackord_server_set_ordering( ackord_server * _server, ackord_ordering _ordering );
ackord_status ackord_server_register( ackord_server * _server, uint32_t _client );
/* Suspension preserves pending commands and acknowledgement progress. Suspended
 * streams reject submissions and are excluded from either processing mode. */
ackord_status ackord_server_suspend( ackord_server * _server, uint32_t _client );
ackord_status ackord_server_resume( ackord_server * _server, uint32_t _client );
/* Client identifiers must not be reused in a session. Cancels pending commands;
 * completed journal entries remain readable until explicitly trimmed. Retired
 * IDs keep their bounded peer slot until reset; max_clients bounds distinct
 * stream IDs per session. */
ackord_status ackord_server_unregister( ackord_server * _server, uint32_t _client );
ackord_status ackord_server_submit( ackord_server * _server, const ackord_command * _command );
/* Returns a retained pending/completed command, STALE for a trimmed completed
 * identity, or NOT_FOUND. Checks all three identity fields; clears out on error. */
ackord_status ackord_server_lookup( const ackord_server * _server, uint64_t _session, uint32_t _client, uint64_t _sequence, ackord_command_view * _out );
/* Calls handler once, including for an empty batch. No mutation/reentry from the
 * handler. No allocation occurs during processing. May reuse step while paused. */
ackord_status ackord_server_process( ackord_server * _server, uint64_t _step, ackord_handler _handler, void * _user );
/* Handler failure or an invalid outcome publishes no part of the batch and
 * blocks further mutation/processing until reset with a fresh session ID.
 * Ackord cannot roll back application state changed by the handler. */
ackord_status ackord_server_process_checked( ackord_server * _server, uint64_t _step, ackord_checked_handler _handler, void * _user );
ackord_status ackord_server_confirmed( const ackord_server * _server, uint32_t _client, uint64_t * _sequence );
ackord_status ackord_server_completed_mask( const ackord_server * _server, uint32_t _client, uint64_t * _mask );
ackord_status ackord_server_confirmation( const ackord_server * _server, uint32_t _client, uint64_t _sequence, ackord_confirmation * _out );
ackord_status ackord_server_journal_next( const ackord_server * _server, uint64_t _afterIndex, ackord_journal_entry * _out );
ackord_status ackord_server_journal_trim( ackord_server * _server, uint64_t _throughIndex );

ackord_status ackord_client_create( const ackord_config * _config, ackord_client ** _out );
void ackord_client_destroy( ackord_client * _client );
/* Clear identity and pending commands without releasing allocated storage. */
ackord_status ackord_client_clear( ackord_client * _client );
/* Repeating the current session/id preserves the queue and returns DUPLICATE. */
ackord_status ackord_client_reset( ackord_client * _client, uint64_t _session, uint32_t _id );
ackord_status ackord_client_submit( ackord_client * _client, uint64_t _step, uint32_t _type, const void * _payload, uint32_t _payloadSize, ackord_command * _out );
/* Borrowed views, valid until the corresponding record is removed/reset/destroyed.
 * Iterate from sequence 0 to build a transmission or retransmission batch. */
ackord_status ackord_client_pending_next( const ackord_client * _client, uint64_t _afterSequence, ackord_command * _out );
size_t ackord_client_pending_count( const ackord_client * _client );
ackord_status ackord_client_confirm( ackord_client * _client, const ackord_confirmation * _confirmation );
/* Validates cumulative/selective progress without removing pending commands. */
ackord_status ackord_client_validate_progress( const ackord_client * _client, uint64_t _session, uint32_t _id, uint64_t _sequence, uint64_t _completedMask );
ackord_status ackord_client_confirm_progress( ackord_client * _client, uint64_t _session, uint32_t _id, uint64_t _sequence, uint64_t _completedMask );
ackord_status ackord_client_confirm_through( ackord_client * _client, uint64_t _session, uint32_t _id, uint64_t _sequence );

#endif
