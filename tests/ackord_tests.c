#include "ackord_test_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK( expression )                                                    \
    do {                                                                       \
        if( !( expression ) ) {                                                \
            fprintf( stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression ); \
            return 0;                                                          \
        }                                                                      \
    } while( 0 )

typedef struct test_context {
    ackord_server * server;
    size_t calls;
    size_t commands;
    uint64_t order[32];
    int reentryOk;
} test_context;

static void __ackord_handle( void * _user, const ackord_command * _commands, ackord_outcome * _outcomes, size_t _count ) {
    test_context * context = _user;

    ++context->calls;
    context->reentryOk = ackord_server_reset( context->server, 2 ) == ACKORD_BUSY;
    for( size_t index = 0; index < _count; ++index ) {
        context->order[context->commands++] = _commands[index].sequence;
        _outcomes[index].status = _commands[index].type == 99 ? 7 : 0;
        memcpy( _outcomes[index].result, &_commands[index].sequence, sizeof( uint64_t ) );
        _outcomes[index].result_size = sizeof( uint64_t );
    }
}

static int __ackord_test_server( void ) {
    ackord_config config = __ackord_test_config();
    ackord_server * server = NULL;
    ackord_command command = { 1, 1, 1, 10, 0, "x", 1 };
    ackord_confirmation confirmation;
    ackord_journal_entry entry;
    test_context context = { 0 };
    uint64_t confirmed = 99;
    uint64_t result = 0;
    uint64_t mask = 0;
    config.max_records = 4;
    CHECK( ackord_server_create( &config, &server ) == ACKORD_OK );
    context.server = server;
    CHECK( ackord_server_reset( server, 1 ) == ACKORD_OK );
    CHECK( ackord_server_register( server, 1 ) == ACKORD_OK );
    CHECK( ackord_server_register( server, 1 ) == ACKORD_DUPLICATE );
    CHECK( ackord_server_submit( server, &command ) == ACKORD_OK );
    CHECK( ackord_server_submit( server, &command ) == ACKORD_DUPLICATE );
    command.payload = "y";
    CHECK( ackord_server_submit( server, &command ) == ACKORD_CONFLICT );
    command.payload = "x";
    command.sequence = 2;
    command.step = 0;
    command.type = 99;
    CHECK( ackord_server_submit( server, &command ) == ACKORD_OK );
    CHECK( ackord_server_process( server, 0, __ackord_handle, &context ) == ACKORD_OK );
    CHECK( context.commands == 1 && context.order[0] == 2 && context.reentryOk );
    CHECK( ackord_server_confirmed( server, 1, &confirmed ) == ACKORD_OK && confirmed == 0 );
    CHECK( ackord_server_completed_mask( server, 1, &mask ) == ACKORD_OK && mask == 2 );
    CHECK( ackord_server_confirmation( server, 1, 2, &confirmation ) == ACKORD_OK );
    CHECK( confirmation.status == 7 && confirmation.result_size == sizeof( result ) );
    memcpy( &result, confirmation.result, sizeof( result ) );
    CHECK( result == 2 );
    CHECK( ackord_server_process( server, 1, __ackord_handle, &context ) == ACKORD_OK && context.commands == 1 && context.calls == 2 );
    CHECK( ackord_server_process( server, 10, __ackord_handle, &context ) == ACKORD_OK );
    CHECK( ackord_server_confirmed( server, 1, &confirmed ) == ACKORD_OK && confirmed == 2 );
    CHECK( context.order[1] == 1 );
    CHECK( ackord_server_journal_next( server, 0, &entry ) == ACKORD_OK && entry.command.sequence == 2 && entry.index == 1 );
    CHECK( ackord_server_journal_trim( server, entry.index ) == ACKORD_OK );
    CHECK( ackord_server_submit( server, &command ) == ACKORD_STALE );
    CHECK( ackord_server_journal_next( server, 1, &entry ) == ACKORD_OK && entry.command.sequence == 1 );
    command.sequence = 67;
    CHECK( ackord_server_submit( server, &command ) == ACKORD_OUT_OF_WINDOW );
    command.sequence = 3;
    command.session = 2;
    CHECK( ackord_server_submit( server, &command ) == ACKORD_WRONG_SESSION );
    command.session = 1;
    CHECK( ackord_server_submit( server, &command ) == ACKORD_OK );
    CHECK( ackord_server_unregister( server, 1 ) == ACKORD_OK );
    CHECK( ackord_server_process( server, 10, __ackord_handle, &context ) == ACKORD_OK && context.commands == 2 );
    CHECK( ackord_server_reset( server, 2 ) == ACKORD_OK );
    CHECK( ackord_server_journal_next( server, 0, &entry ) == ACKORD_NOT_FOUND );
    ackord_server_destroy( server );
    return 1;
}

static int __ackord_test_order_and_capacity( void ) {
    ackord_config config = __ackord_test_config();
    ackord_server * server;
    ackord_command command = { 1, 1, 2, 0, 0, NULL, 0 };
    test_context context = { 0 };
    uint64_t prefix;
    config.max_records = 3;
    config.max_clients = 2;
    CHECK( ackord_server_create( &config, &server ) == ACKORD_OK );
    context.server = server;
    CHECK( ackord_server_reset( server, 1 ) == ACKORD_OK );
    CHECK( ackord_server_register( server, 1 ) == ACKORD_OK );
    CHECK( ackord_server_register( server, 2 ) == ACKORD_OK );
    CHECK( ackord_server_register( server, 3 ) == ACKORD_FULL );
    CHECK( ackord_server_submit( server, &command ) == ACKORD_OK );
    command.sequence = 1;
    CHECK( ackord_server_submit( server, &command ) == ACKORD_OK );
    command.client = 2;
    CHECK( ackord_server_submit( server, &command ) == ACKORD_OK );
    command.sequence = 2;
    CHECK( ackord_server_submit( server, &command ) == ACKORD_FULL );
    CHECK( ackord_server_process( server, 0, __ackord_handle, &context ) == ACKORD_OK );
    CHECK( context.commands == 3 && context.order[0] == 1 && context.order[1] == 2 && context.order[2] == 1 );
    CHECK( ackord_server_confirmed( server, 1, &prefix ) == ACKORD_OK && prefix == 2 );
    CHECK( ackord_server_journal_trim( server, 3 ) == ACKORD_OK );
    CHECK( ackord_server_submit( server, &command ) == ACKORD_OK );
    ackord_server_destroy( server );
    return 1;
}

static int __ackord_test_client( void ) {
    ackord_config config = __ackord_test_config();
    ackord_client * client;
    ackord_command command;
    ackord_confirmation confirmation = { 1, 3, 2, 0, 7, NULL, 0 };
    char payload[] = "abc";
    config.max_records = 2;
    CHECK( ackord_client_create( &config, &client ) == ACKORD_OK );
    CHECK( ackord_client_reset( client, 1, 3 ) == ACKORD_OK );
    CHECK( ackord_client_submit( client, 0, 1, payload, sizeof( payload ), &command ) == ACKORD_OK && command.sequence == 1 );
    payload[0] = 'z';
    CHECK( memcmp( command.payload, "abc", sizeof( payload ) ) == 0 );
    CHECK( ackord_client_submit( client, 0, 1, NULL, 0, &command ) == ACKORD_OK && command.sequence == 2 );
    CHECK( ackord_client_submit( client, 0, 1, NULL, 0, &command ) == ACKORD_FULL );
    CHECK( ackord_client_confirm_progress( client, 1, 3, 1, 2 ) == ACKORD_INVALID );
    CHECK( ackord_client_pending_count( client ) == 2 );
    CHECK( ackord_client_pending_next( client, 0, &command ) == ACKORD_OK && command.sequence == 1 );
    CHECK( ackord_client_pending_next( client, 1, &command ) == ACKORD_OK && command.sequence == 2 );
    CHECK( ackord_client_confirm( client, &confirmation ) == ACKORD_OK );
    CHECK( ackord_client_confirm( client, &confirmation ) == ACKORD_DUPLICATE );
    CHECK( ackord_client_pending_count( client ) == 1 );
    CHECK( ackord_client_confirm_through( client, 2, 3, 1 ) == ACKORD_WRONG_SESSION );
    CHECK( ackord_client_confirm_through( client, 1, 3, 3 ) == ACKORD_INVALID );
    CHECK( ackord_client_confirm_through( client, 1, 3, 1 ) == ACKORD_OK );
    CHECK( ackord_client_pending_count( client ) == 0 );
    CHECK( ackord_client_submit( client, 0, 1, NULL, 0, &command ) == ACKORD_OK && command.sequence == 3 );
    CHECK( ackord_client_reset( client, 2, 3 ) == ACKORD_OK && ackord_client_pending_count( client ) == 0 );
    CHECK( ackord_client_confirm( client, &confirmation ) == ACKORD_WRONG_SESSION );
    CHECK( ackord_client_submit( client, 0, 1, NULL, 0, &command ) == ACKORD_OK );
    CHECK( ackord_client_submit( client, 0, 1, NULL, 0, &command ) == ACKORD_OK );
    CHECK( ackord_client_confirm_progress( client, 2, 3, 0, 2 ) == ACKORD_OK );
    CHECK( ackord_client_pending_count( client ) == 1 );
    CHECK( ackord_client_pending_next( client, 0, &command ) == ACKORD_OK && command.sequence == 1 );
    ackord_client_destroy( client );
    return 1;
}

static int __ackord_test_progress_validation( void ) {
    ackord_config config = __ackord_test_config();
    ackord_client * client;
    ackord_command command;
    config.max_records = 3;
    CHECK( ackord_client_create( &config, &client ) == ACKORD_OK );
    CHECK( ackord_client_reset( client, 1, 1 ) == ACKORD_OK );
    for( uint64_t sequence = 1; sequence <= 3; ++sequence ) {
        CHECK( ackord_client_submit( client, 0, 1, "x", 1, &command ) == ACKORD_OK && command.sequence == sequence );
    }

    CHECK( ackord_client_validate_progress( NULL, 1, 1, 0, 0 ) == ACKORD_INVALID );
    CHECK( ackord_client_validate_progress( client, 2, 1, 0, 0 ) == ACKORD_WRONG_SESSION );
    CHECK( ackord_client_validate_progress( client, 1, 2, 0, 0 ) == ACKORD_WRONG_SESSION );
    CHECK( ackord_client_validate_progress( client, 1, 1, 4, 0 ) == ACKORD_INVALID );
    CHECK( ackord_client_validate_progress( client, 1, 1, 0, 8 ) == ACKORD_INVALID );
    CHECK( ackord_client_validate_progress( client, 1, 1, UINT64_MAX, 0 ) == ACKORD_INVALID );
    CHECK( ackord_client_validate_progress( client, 1, 1, UINT64_MAX - 1, UINT64_C( 1 ) << 63 ) == ACKORD_INVALID );
    CHECK( ackord_client_pending_count( client ) == 3 );
    CHECK( ackord_client_validate_progress( client, 1, 1, 0, 0 ) == ACKORD_OK );
    CHECK( ackord_client_validate_progress( client, 1, 1, 1, 2 ) == ACKORD_OK );
    CHECK( ackord_client_validate_progress( client, 1, 1, 3, 0 ) == ACKORD_OK );
    CHECK( ackord_client_pending_count( client ) == 3 );
    for( uint64_t sequence = 1; sequence <= 3; ++sequence ) {
        CHECK( ackord_client_pending_next( client, sequence - 1, &command ) == ACKORD_OK );
        CHECK( command.sequence == sequence && memcmp( command.payload, "x", 1 ) == 0 );
    }

    CHECK( ackord_client_confirm_progress( client, 1, 1, 1, 4 ) == ACKORD_INVALID );
    CHECK( ackord_client_pending_count( client ) == 3 );
    CHECK( ackord_client_confirm_progress( client, 1, 1, 1, 2 ) == ACKORD_OK );
    CHECK( ackord_client_pending_count( client ) == 1 );
    CHECK( ackord_client_pending_next( client, 0, &command ) == ACKORD_OK && command.sequence == 2 );
    ackord_client_destroy( client );
    return 1;
}

static void __ackord_count_commands( void * _user, const ackord_command * _commands, ackord_outcome * _outcomes, size_t _count ) {
    size_t * processed = _user;
    (void) _commands;
    (void) _outcomes;
    *processed += _count;
}

static int __ackord_test_window_boundary( void ) {
    ackord_config config = __ackord_test_config();
    ackord_server * server;
    ackord_command command = { 1, 1, 64, 0, 0, NULL, 0 };
    ackord_journal_entry entry;
    uint64_t prefix, mask, sequence;
    size_t processed = 0;
    config.max_records = 2;
    CHECK( ackord_server_create( &config, &server ) == ACKORD_OK );
    CHECK( ackord_server_reset( server, 1 ) == ACKORD_OK );
    CHECK( ackord_server_register( server, 1 ) == ACKORD_OK );
    CHECK( ackord_server_submit( server, &command ) == ACKORD_OK );
    CHECK( ackord_server_process( server, 0, __ackord_count_commands, &processed ) == ACKORD_OK );
    CHECK( ackord_server_completed_mask( server, 1, &mask ) == ACKORD_OK && mask == ( UINT64_C( 1 ) << 63 ) );
    CHECK( ackord_server_journal_next( server, 0, &entry ) == ACKORD_OK );
    CHECK( ackord_server_journal_trim( server, entry.index ) == ACKORD_OK );
    CHECK( ackord_server_submit( server, &command ) == ACKORD_STALE );
    for( sequence = 1; sequence < 64; ++sequence ) {
        command.sequence = sequence;
        CHECK( ackord_server_submit( server, &command ) == ACKORD_OK );
        CHECK( ackord_server_process( server, 0, __ackord_count_commands, &processed ) == ACKORD_OK );
        CHECK( ackord_server_confirmed( server, 1, &prefix ) == ACKORD_OK );
        CHECK( prefix == ( sequence == 63 ? 64 : sequence ) );
        CHECK( ackord_server_journal_next( server, 0, &entry ) == ACKORD_OK );
        CHECK( ackord_server_journal_trim( server, entry.index ) == ACKORD_OK );
    }

    CHECK( processed == 64 );
    CHECK( ackord_server_completed_mask( server, 1, &mask ) == ACKORD_OK && mask == 0 );
    ackord_server_destroy( server );
    return 1;
}

static int __ackord_test_journal_wrap( void ) {
    ackord_config config = __ackord_test_config();
    ackord_server * server;
    ackord_command command = { 1, 1, 1, 0, 0, "a", 1 };
    ackord_journal_entry entry;
    ackord_journal_entry retained;
    test_context context = { 0 };
    uint64_t result;
    config.max_clients = 2;
    config.max_records = 3;
    CHECK( ackord_server_create( &config, &server ) == ACKORD_OK );
    context.server = server;
    CHECK( ackord_server_reset( server, 1 ) == ACKORD_OK );
    CHECK( ackord_server_register( server, 1 ) == ACKORD_OK );
    CHECK( ackord_server_journal_next( server, UINT64_MAX, &entry ) == ACKORD_NOT_FOUND );
    CHECK( ackord_server_submit( server, &command ) == ACKORD_OK );
    command.sequence = 2;
    command.step = 20;
    command.payload = "b";
    CHECK( ackord_server_submit( server, &command ) == ACKORD_OK );
    command.sequence = 3;
    command.step = 10;
    command.payload = "c";
    CHECK( ackord_server_submit( server, &command ) == ACKORD_OK );
    CHECK( ackord_server_process( server, 0, __ackord_handle, &context ) == ACKORD_OK );
    CHECK( ackord_server_journal_next( server, 0, &entry ) == ACKORD_OK );
    CHECK( entry.index == 1 && entry.command.sequence == 1 );
    CHECK( ackord_server_process( server, 10, __ackord_handle, &context ) == ACKORD_OK );
    CHECK( ackord_server_journal_next( server, 1, &retained ) == ACKORD_OK );
    CHECK( retained.index == 2 && retained.command.sequence == 3 );
    CHECK( ackord_server_journal_trim( server, 0 ) == ACKORD_OK );
    CHECK( ackord_server_journal_next( server, 0, &entry ) == ACKORD_OK && entry.index == 1 );
    CHECK( ackord_server_journal_trim( server, 1 ) == ACKORD_OK );
    command.sequence = 4;
    command.payload = "d";
    CHECK( ackord_server_submit( server, &command ) == ACKORD_OK );
    CHECK( ackord_server_process( server, 10, __ackord_handle, &context ) == ACKORD_OK );
    command.sequence = 5;
    CHECK( ackord_server_submit( server, &command ) == ACKORD_FULL );
    CHECK( ackord_server_unregister( server, 1 ) == ACKORD_OK );
    CHECK( ackord_server_register( server, 2 ) == ACKORD_OK );
    command.client = 2;
    command.sequence = 1;
    command.payload = "e";
    CHECK( ackord_server_submit( server, &command ) == ACKORD_OK );
    CHECK( ackord_server_process( server, 10, __ackord_handle, &context ) == ACKORD_OK );
    CHECK( memcmp( retained.command.payload, "c", 1 ) == 0 );
    memcpy( &result, retained.confirmation.result, sizeof( result ) );
    CHECK( result == 3 );
    CHECK( ackord_server_journal_trim( server, 1 ) == ACKORD_OK );
    CHECK( ackord_server_journal_next( server, 0, &entry ) == ACKORD_OK && entry.index == 2 );
    CHECK( ackord_server_journal_next( server, 1, &entry ) == ACKORD_OK && entry.index == 2 );
    CHECK( ackord_server_journal_next( server, 2, &retained ) == ACKORD_OK );
    CHECK( retained.index == 3 && retained.command.client == 1 && retained.command.sequence == 4 );
    CHECK( ackord_server_journal_next( server, 3, &entry ) == ACKORD_OK );
    CHECK( entry.index == 4 && entry.command.client == 2 && entry.command.sequence == 1 );
    CHECK( ackord_server_journal_next( server, 4, &entry ) == ACKORD_NOT_FOUND );
    CHECK( ackord_server_journal_next( server, UINT64_MAX, &entry ) == ACKORD_NOT_FOUND );
    CHECK( ackord_server_journal_trim( server, 2 ) == ACKORD_OK );
    command.sequence = 2;
    command.payload = "f";
    CHECK( ackord_server_submit( server, &command ) == ACKORD_OK );
    CHECK( ackord_server_process( server, 10, __ackord_handle, &context ) == ACKORD_OK );
    CHECK( memcmp( retained.command.payload, "d", 1 ) == 0 );
    memcpy( &result, retained.confirmation.result, sizeof( result ) );
    CHECK( result == 4 );
    CHECK( ackord_server_journal_next( server, 0, &entry ) == ACKORD_OK && entry.index == 3 );
    CHECK( ackord_server_journal_next( server, 3, &entry ) == ACKORD_OK && entry.index == 4 );
    CHECK( ackord_server_journal_next( server, 4, &entry ) == ACKORD_OK && entry.index == 5 );
    CHECK( ackord_server_journal_trim( server, UINT64_MAX ) == ACKORD_OK );
    CHECK( ackord_server_journal_next( server, 0, &entry ) == ACKORD_NOT_FOUND );
    command.sequence = 3;
    CHECK( ackord_server_submit( server, &command ) == ACKORD_OK );
    CHECK( ackord_server_process( server, 10, __ackord_handle, &context ) == ACKORD_OK );
    CHECK( ackord_server_journal_next( server, 0, &entry ) == ACKORD_OK && entry.index == 6 );
    CHECK( ackord_server_journal_next( server, 5, &entry ) == ACKORD_OK && entry.index == 6 );
    CHECK( ackord_server_reset( server, 1 ) == ACKORD_DUPLICATE );
    CHECK( ackord_server_journal_next( server, 0, &entry ) == ACKORD_OK && entry.index == 6 );
    CHECK( ackord_server_reset( server, 2 ) == ACKORD_OK );
    CHECK( ackord_server_journal_next( server, 0, &entry ) == ACKORD_NOT_FOUND );
    CHECK( ackord_server_register( server, 1 ) == ACKORD_OK );
    command.session = 2;
    command.client = 1;
    command.sequence = 1;
    CHECK( ackord_server_submit( server, &command ) == ACKORD_OK );
    CHECK( ackord_server_process( server, 10, __ackord_handle, &context ) == ACKORD_OK );
    CHECK( ackord_server_journal_next( server, 0, &entry ) == ACKORD_OK );
    CHECK( entry.index == 1 && entry.command.session == 2 );
    ackord_server_destroy( server );
    return 1;
}

static int __ackord_test_record_reuse( void ) {
    ackord_config config = __ackord_test_config();
    ackord_server * server;
    ackord_command command = { 1, 1, 1, 0, 0, "x", 1 };
    ackord_command_view view;
    size_t processed = 0;
    config.max_clients = 4;
    config.max_records = 8;
    CHECK( ackord_server_create( &config, &server ) == ACKORD_OK );
    CHECK( ackord_server_reset( server, 1 ) == ACKORD_OK );
    for( uint32_t client = 1; client <= 4; ++client ) {
        CHECK( ackord_server_register( server, client ) == ACKORD_OK );
    }

    for( uint32_t client = 1; client <= 2; ++client ) {
        command.client = client;
        command.step = client == 1 ? 0 : 100;
        for( uint64_t sequence = 1; sequence <= 4; ++sequence ) {
            command.sequence = sequence;
            CHECK( ackord_server_submit( server, &command ) == ACKORD_OK );
            CHECK( ackord_server_submit( server, &command ) == ACKORD_DUPLICATE );
        }
    }

    CHECK( ackord_server_process( server, 0, __ackord_count_commands, &processed ) == ACKORD_OK && processed == 4 );
    CHECK( ackord_server_journal_trim( server, 2 ) == ACKORD_OK );
    command.client = 3;
    for( uint64_t sequence = 1; sequence <= 2; ++sequence ) {
        command.sequence = sequence;
        CHECK( ackord_server_submit( server, &command ) == ACKORD_OK );
    }

    CHECK( ackord_server_unregister( server, 2 ) == ACKORD_OK );
    for( uint64_t sequence = 1; sequence <= 4; ++sequence ) {
        CHECK( ackord_server_lookup( server, 1, 2, sequence, &view ) == ACKORD_NOT_FOUND );
    }

    command.client = 4;
    command.step = 0;
    for( uint64_t sequence = 1; sequence <= 4; ++sequence ) {
        command.sequence = sequence;
        CHECK( ackord_server_submit( server, &command ) == ACKORD_OK );
    }

    command.client = 3;
    command.sequence = 3;
    CHECK( ackord_server_submit( server, &command ) == ACKORD_FULL );
    CHECK( ackord_server_lookup( server, 1, 1, 1, &view ) == ACKORD_STALE );
    CHECK( ackord_server_lookup( server, 1, 1, 2, &view ) == ACKORD_STALE );
    for( uint64_t sequence = 3; sequence <= 4; ++sequence ) {
        CHECK( ackord_server_lookup( server, 1, 1, sequence, &view ) == ACKORD_OK );
        CHECK( view.state == ACKORD_COMMAND_COMPLETED && memcmp( view.command.payload, "x", 1 ) == 0 );
    }

    command.client = 3;
    command.step = 100;
    for( uint64_t sequence = 1; sequence <= 2; ++sequence ) {
        command.sequence = sequence;
        CHECK( ackord_server_submit( server, &command ) == ACKORD_DUPLICATE );
        CHECK( ackord_server_lookup( server, 1, 3, sequence, &view ) == ACKORD_OK && view.state == ACKORD_COMMAND_PENDING );
    }

    CHECK( ackord_server_process( server, 100, __ackord_count_commands, &processed ) == ACKORD_OK && processed == 10 );
    CHECK( ackord_server_unregister( server, 1 ) == ACKORD_OK );
    CHECK( ackord_server_lookup( server, 1, 1, 3, &view ) == ACKORD_OK && view.state == ACKORD_COMMAND_COMPLETED );
    command.sequence = 1;
    command.payload = "y";
    CHECK( ackord_server_submit( server, &command ) == ACKORD_CONFLICT );
    command.payload = "x";
    CHECK( ackord_server_submit( server, &command ) == ACKORD_DUPLICATE );
    CHECK( ackord_server_journal_trim( server, UINT64_MAX ) == ACKORD_OK );
    CHECK( ackord_server_submit( server, &command ) == ACKORD_STALE );
    for( uint64_t sequence = 3; sequence <= 10; ++sequence ) {
        command.sequence = sequence;
        CHECK( ackord_server_submit( server, &command ) == ACKORD_OK );
        CHECK( ackord_server_submit( server, &command ) == ACKORD_DUPLICATE );
    }

    CHECK( ackord_server_reset( server, 2 ) == ACKORD_OK );
    CHECK( ackord_server_register( server, 1 ) == ACKORD_OK );
    command.client = 1;
    command.sequence = 1;
    CHECK( ackord_server_submit( server, &command ) == ACKORD_WRONG_SESSION );
    command.session = 2;
    CHECK( ackord_server_submit( server, &command ) == ACKORD_OK );
    CHECK( ackord_server_lookup( server, 2, 1, 1, &view ) == ACKORD_OK && view.state == ACKORD_COMMAND_PENDING );
    CHECK( ackord_server_process( server, 100, __ackord_count_commands, &processed ) == ACKORD_OK && processed == 11 );
    ackord_server_destroy( server );
    return 1;
}

typedef struct allocation_state {
    size_t calls, failAt, outstanding, lastSize, limit;
} allocation_state;

static void * __ackord_allocate( void * _user, size_t _size ) {
    allocation_state * state = _user;
    void * memory;
    state->lastSize = _size;
    if( ++state->calls == state->failAt ) {
        return NULL;
    }

    if( _size >= UINT32_MAX ) {
        return NULL;
    }

    if( state->limit != 0 && _size > state->limit ) {
        return NULL;
    }

    memory = malloc( _size );
    if( memory != NULL ) {
        ++state->outstanding;
    }

    return memory;
}

static void __ackord_deallocate( void * _user, void * _memory ) {
    allocation_state * state = _user;
    --state->outstanding;
    free( _memory );
}

typedef struct ordering_context {
    ackord_command previous;
    size_t expected;
    size_t count;
    int valid;
    unsigned char seen[4096];
} ordering_context;

static void __ackord_handle_ordering( void * _user, const ackord_command * _commands, ackord_outcome * _outcomes, size_t _count ) {
    ordering_context * context = _user;
    (void) _outcomes;

    for( size_t index = 0; index < _count; ++index ) {
        const ackord_command * command = &_commands[index];
        const ackord_command * previous = &context->previous;
        if( context->count != 0 ) {
            if( command->step < previous->step ) {
                context->valid = 0;
            }

            if( command->step == previous->step && command->client < previous->client ) {
                context->valid = 0;
            }

            if( command->step == previous->step && command->client == previous->client && command->sequence <= previous->sequence ) {
                context->valid = 0;
            }
        }

        size_t identity = (size_t) ( UINT32_MAX - command->client ) * 64 + command->sequence - 1;
        if( identity >= context->expected ) {
            context->valid = 0;
        } else if( context->seen[identity] != 0 ) {
            context->valid = 0;
        } else {
            context->seen[identity] = 1;
        }

        context->previous = *command;
        ++context->count;
    }
}

static int __ackord_test_large_ordering( void ) {
    size_t counts[] = { 0, 1, 2, 3, 63, 64, 65, 127, 256, 1023, 4096 };
    size_t indices[4096];
    allocation_state state = { 0 };
    ackord_config config = __ackord_test_config();
    ackord_server * server = NULL;
    config.max_clients = 64;
    config.max_records = 4096;
    config.max_payload_size = 1;
    config.max_result_size = 0;
    config.allocator = ( ackord_allocator ){ __ackord_allocate, __ackord_deallocate, &state };
    CHECK( ackord_server_create( &config, &server ) == ACKORD_OK );
    size_t allocations = state.calls;
    state.failAt = allocations + 1;

    for( size_t test = 0; test < sizeof( counts ) / sizeof( counts[0] ); ++test ) {
        size_t count = counts[test];
        for( size_t direction = 0; direction < 3; ++direction ) {
            ordering_context context = { 0 };
            context.expected = count;
            context.valid = 1;
            uint64_t session = 1 + test * 3 + direction;
            CHECK( ackord_server_reset( server, session ) == ACKORD_OK );
            for( uint32_t client = 0; client < 64; ++client ) {
                CHECK( ackord_server_register( server, UINT32_MAX - client ) == ACKORD_OK );
            }

            for( size_t index = 0; index < count; ++index ) {
                indices[index] = direction == 1 ? count - index - 1 : index;
            }

            if( direction == 2 ) {
                uint32_t random = 1;
                for( size_t remaining = count; remaining > 1; --remaining ) {
                    random = random * UINT32_C( 1664525 ) + UINT32_C( 1013904223 );
                    size_t selected = random % remaining;
                    size_t saved = indices[remaining - 1];
                    indices[remaining - 1] = indices[selected];
                    indices[selected] = saved;
                }
            }

            size_t due = 0;
            for( size_t index = 0; index < count; ++index ) {
                size_t identity = indices[index];
                uint64_t sequence = identity % 64 + 1;
                ackord_command command = { session, UINT32_MAX - (uint32_t) ( identity / 64 ), sequence, sequence % 7, 0, NULL, 0 };
                CHECK( ackord_server_submit( server, &command ) == ACKORD_OK );
                if( command.step <= 3 ) {
                    ++due;
                }
            }

            CHECK( ackord_server_process( server, 3, __ackord_handle_ordering, &context ) == ACKORD_OK );
            CHECK( context.valid != 0 && context.count == due );
            CHECK( ackord_server_process( server, 6, __ackord_handle_ordering, &context ) == ACKORD_OK );
            CHECK( context.valid != 0 && context.count == count );
            CHECK( state.calls == allocations );
            for( size_t index = 0; index < count; ++index ) {
                CHECK( context.seen[index] == 1 );
            }
        }
    }

    ackord_server_destroy( server );
    CHECK( state.outstanding == 0 );
    return 1;
}

static int __ackord_test_client_storage( void ) {
    ackord_config config = __ackord_test_config();
    allocation_state state = { 0 };
    ackord_client * client = NULL;
    ackord_command command;
    ackord_command first;
    unsigned char payload[16];
    config.max_records = 1024;
    config.max_payload_size = sizeof( payload );
    config.max_result_size = 65536;
    config.allocator = ( ackord_allocator ){ __ackord_allocate, __ackord_deallocate, &state };
    state.limit = 1024 * 1024;
    CHECK( ackord_client_create( &config, &client ) == ACKORD_OK );
    CHECK( state.calls == 3 && state.lastSize == config.max_records * sizeof( payload ) );
    CHECK( ackord_client_reset( client, 1, 1 ) == ACKORD_OK );
    for( size_t index = 0; index < config.max_records; ++index ) {
        memset( payload, (int) ( index % 256 ), sizeof( payload ) );
        CHECK( ackord_client_submit( client, 0, 0, payload, sizeof( payload ), &command ) == ACKORD_OK );
        if( index == 0 ) {
            first = command;
        }
    }

    memset( payload, 0, sizeof( payload ) );
    CHECK( memcmp( first.payload, payload, sizeof( payload ) ) == 0 );
    memset( payload, 255, sizeof( payload ) );
    CHECK( memcmp( command.payload, payload, sizeof( payload ) ) == 0 );
    CHECK( ackord_client_pending_count( client ) == config.max_records );
    CHECK( ackord_client_reset( client, 1, 1 ) == ACKORD_DUPLICATE );
    CHECK( ackord_client_pending_count( client ) == config.max_records );
    CHECK( ackord_client_clear( NULL ) == ACKORD_INVALID );
    CHECK( ackord_client_clear( client ) == ACKORD_OK );
    CHECK( ackord_client_pending_count( client ) == 0 );
    CHECK( ackord_client_pending_next( client, 0, &command ) == ACKORD_NOT_FOUND );
    CHECK( ackord_client_submit( client, 0, 0, payload, sizeof( payload ), &command ) == ACKORD_INVALID );
    CHECK( ackord_client_reset( client, 1, 1 ) == ACKORD_OK );
    CHECK( ackord_client_submit( client, 0, 0, payload, sizeof( payload ), &command ) == ACKORD_OK && command.sequence == 1 );
    CHECK( command.payload == first.payload );
    CHECK( state.calls == 3 );
    ackord_client_destroy( client );
    CHECK( state.outstanding == 0 );

    config.max_records = 17;
    config.max_payload_size = 0;
    for( size_t failure = 1; failure <= 3; ++failure ) {
        state = ( allocation_state ){ 0 };
        state.failAt = failure;
        client = NULL;
        ackord_status status = ackord_client_create( &config, &client );
        CHECK( status == ( failure <= 2 ? ACKORD_NO_MEMORY : ACKORD_OK ) );
        if( status == ACKORD_NO_MEMORY ) {
            CHECK( client == NULL && state.outstanding == 0 );
            CHECK( state.calls == failure );
            continue;
        }

        CHECK( state.calls == 2 );
        CHECK( ackord_client_reset( client, 1, 1 ) == ACKORD_OK );
        CHECK( ackord_client_submit( client, 0, 0, payload, 1, &command ) == ACKORD_INVALID );
        for( uint64_t sequence = 1; sequence <= config.max_records; ++sequence ) {
            CHECK( ackord_client_submit( client, 0, 0, NULL, 0, &command ) == ACKORD_OK );
            CHECK( command.sequence == sequence && command.payload == NULL && command.payload_size == 0 );
        }

        CHECK( ackord_client_submit( client, 0, 0, NULL, 0, &command ) == ACKORD_FULL );
        CHECK( ackord_client_confirm_through( client, 1, 1, config.max_records ) == ACKORD_OK );
        CHECK( ackord_client_submit( client, 0, 0, NULL, 0, &command ) == ACKORD_OK && command.sequence == 18 );
        CHECK( ackord_client_reset( client, 2, 1 ) == ACKORD_OK );
        CHECK( ackord_client_submit( client, 0, 0, NULL, 0, &command ) == ACKORD_OK && command.sequence == 1 );
        CHECK( ackord_client_clear( client ) == ACKORD_OK );
        CHECK( ackord_client_clear( client ) == ACKORD_OK );
        CHECK( state.calls == 2 );
        ackord_client_destroy( client );
        CHECK( state.outstanding == 0 );
    }

    return 1;
}

static int __ackord_test_allocations( void ) {
    size_t failure;
    for( failure = 1; failure <= 10; ++failure ) {
        ackord_config config = __ackord_test_config();
        allocation_state state = { 0 };
        ackord_server * server = NULL;
        ackord_client * client = NULL;
        ackord_status status;
        state.failAt = failure;
        config.allocator.allocate = __ackord_allocate;
        config.allocator.deallocate = __ackord_deallocate;
        config.allocator.user = &state;
        status = ackord_server_create( &config, &server );
        CHECK( status == ( failure <= 9 ? ACKORD_NO_MEMORY : ACKORD_OK ) );
        if( status == ACKORD_NO_MEMORY ) {
            CHECK( server == NULL );
            CHECK( state.calls == failure );
            CHECK( state.outstanding == 0 );
        }

        ackord_server_destroy( server );
        CHECK( state.outstanding == 0 );
        state.calls = 0;
        status = ackord_client_create( &config, &client );
        CHECK( status == ( failure <= 3 ? ACKORD_NO_MEMORY : ACKORD_OK ) );
        if( status == ACKORD_NO_MEMORY ) {
            CHECK( client == NULL );
            CHECK( state.calls == failure );
            CHECK( state.outstanding == 0 );
        }

        ackord_client_destroy( client );
        CHECK( state.outstanding == 0 );
    }
    {
        ackord_config config = __ackord_test_config();
        ackord_server * server = NULL;
        config.max_records = SIZE_MAX;
        CHECK( ackord_server_create( &config, &server ) == ACKORD_INVALID && server == NULL );
    }

    return 1;
}

static int __ackord_test_size_limits( void ) {
    uint32_t resultSizes[] = { 1, UINT32_MAX };

    for( size_t index = 0; index < sizeof( resultSizes ) / sizeof( resultSizes[0] ); ++index ) {
        ackord_config config = __ackord_test_config();
        allocation_state state = { 0 };
        ackord_server * server = NULL;
        ackord_client * client = NULL;
        ackord_status expected;
        config.max_clients = 1;
        config.max_records = 1;
        config.max_payload_size = UINT32_MAX;
        config.max_result_size = resultSizes[index];
        config.allocator.allocate = __ackord_allocate;
        config.allocator.deallocate = __ackord_deallocate;
        config.allocator.user = &state;
#if SIZE_MAX > UINT32_MAX
        expected = ACKORD_NO_MEMORY;
#else
        expected = ACKORD_INVALID;
#endif
        CHECK( ackord_server_create( &config, &server ) == expected && server == NULL );
#if SIZE_MAX > UINT32_MAX
        CHECK( state.lastSize == (size_t) UINT32_MAX + resultSizes[index] );
#else
        CHECK( state.calls == 0 );
#endif
        CHECK( state.outstanding == 0 );
        state = ( allocation_state ){ 0 };
        CHECK( ackord_client_create( &config, &client ) == expected && client == NULL );
#if SIZE_MAX > UINT32_MAX
        CHECK( state.lastSize == UINT32_MAX );
#else
        CHECK( state.calls == 0 );
#endif
        CHECK( state.outstanding == 0 );
    }

    return 1;
}

static int __ackord_test_explicit_config( void ) {
    ackord_config config = { 0 };
    ackord_server * server = NULL;
    ackord_client * client = NULL;
    CHECK( ackord_server_create( NULL, &server ) == ACKORD_INVALID && server == NULL );
    CHECK( ackord_client_create( NULL, &client ) == ACKORD_INVALID && client == NULL );
    CHECK( ackord_server_create( &config, &server ) == ACKORD_INVALID && server == NULL );
    CHECK( ackord_client_create( &config, &client ) == ACKORD_INVALID && client == NULL );
    config = __ackord_test_config();
    config.allocator.allocate = NULL;
    CHECK( ackord_server_create( &config, &server ) == ACKORD_INVALID && server == NULL );
    CHECK( ackord_client_create( &config, &client ) == ACKORD_INVALID && client == NULL );
    config.allocator.allocate = __ackord_test_allocate;
    config.allocator.deallocate = NULL;
    CHECK( ackord_server_create( &config, &server ) == ACKORD_INVALID && server == NULL );
    CHECK( ackord_client_create( &config, &client ) == ACKORD_INVALID && client == NULL );
    return 1;
}

int main( void ) {
    if( __ackord_test_explicit_config() == 0 ) {
        return EXIT_FAILURE;
    }

    if( __ackord_test_server() == 0 ) {
        return EXIT_FAILURE;
    }

    if( __ackord_test_order_and_capacity() == 0 ) {
        return EXIT_FAILURE;
    }

    if( __ackord_test_large_ordering() == 0 ) {
        return EXIT_FAILURE;
    }

    if( __ackord_test_client() == 0 ) {
        return EXIT_FAILURE;
    }

    if( __ackord_test_progress_validation() == 0 ) {
        return EXIT_FAILURE;
    }

    if( __ackord_test_client_storage() == 0 ) {
        return EXIT_FAILURE;
    }

    if( __ackord_test_window_boundary() == 0 ) {
        return EXIT_FAILURE;
    }

    if( __ackord_test_journal_wrap() == 0 ) {
        return EXIT_FAILURE;
    }

    if( __ackord_test_record_reuse() == 0 ) {
        return EXIT_FAILURE;
    }

    if( __ackord_test_allocations() == 0 ) {
        return EXIT_FAILURE;
    }

    if( __ackord_test_size_limits() == 0 ) {
        return EXIT_FAILURE;
    }

    puts( "Ackord: server, ordering, capacity, client and allocation tests passed" );
    return EXIT_SUCCESS;
}
