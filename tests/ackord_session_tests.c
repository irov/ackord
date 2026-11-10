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

typedef struct input_context {
    size_t calls;
    size_t count;
    uint32_t clients[128];
    uint64_t sequences[128];
    unsigned held[4];
} input_context;

static void __ackord_apply_input( void * _user, const ackord_command * _commands, ackord_outcome * _outcomes, size_t _count ) {
    input_context * context = _user;

    ++context->calls;
    for( size_t index = 0; index < _count; ++index ) {
        context->clients[context->count] = _commands[index].client;
        context->sequences[context->count++] = _commands[index].sequence;
        if( _commands[index].client < 4 ) {
            context->held[_commands[index].client] = _commands[index].type == 1;
        }

        if( _commands[index].type == 99 ) {
            _outcomes[index].status = 7;
        }
    }
}

static ackord_status __ackord_apply_checked( void * _user, const ackord_command * _commands, ackord_outcome * _outcomes, size_t _count ) {
    __ackord_apply_input( _user, _commands, _outcomes, _count );
    return ACKORD_OK;
}

static int __ackord_test_strict_trigger_order( void ) {
    ackord_config config = __ackord_test_config();
    ackord_server * server = NULL;
    ackord_command command = { 1, 1, 2, 0, 2, NULL, 0 };
    ackord_command_view view;
    ackord_confirmation confirmation;
    input_context context = { 0 };
    uint64_t prefix;
    config.max_clients = 2;
    config.max_records = 8;
    CHECK( ackord_server_create( &config, &server ) == ACKORD_OK );
    CHECK( ackord_server_set_ordering( server, (ackord_ordering) 99 ) == ACKORD_INVALID );
    CHECK( ackord_server_set_ordering( server, ACKORD_ORDER_BY_SEQUENCE ) == ACKORD_OK );
    CHECK( ackord_server_reset( server, 1 ) == ACKORD_OK );
    CHECK( ackord_server_register( server, 1 ) == ACKORD_OK );
    CHECK( ackord_server_register( server, 2 ) == ACKORD_OK );
    CHECK( ackord_server_submit( server, &command ) == ACKORD_OK );
    CHECK( ackord_server_set_ordering( server, ACKORD_ORDER_BY_STEP ) == ACKORD_BUSY );
    command.client = 2;
    command.sequence = 1;
    CHECK( ackord_server_submit( server, &command ) == ACKORD_OK );
    CHECK( ackord_server_process_checked( server, 0, __ackord_apply_checked, &context ) == ACKORD_OK );
    CHECK( context.count == 1 && context.clients[0] == 2 );
    CHECK( ackord_server_confirmed( server, 1, &prefix ) == ACKORD_OK && prefix == 0 );
    CHECK( ackord_server_confirmation( server, 1, 2, &confirmation ) == ACKORD_NOT_FOUND );
    CHECK( ackord_server_lookup( server, 1, 1, 2, &view ) == ACKORD_OK && view.state == ACKORD_COMMAND_PENDING );
    command.client = 1;
    command.step = 2;
    command.type = 1;
    CHECK( ackord_server_submit( server, &command ) == ACKORD_OK );
    CHECK( ackord_server_reset( server, 1 ) == ACKORD_DUPLICATE );
    CHECK( ackord_server_process( server, 1, __ackord_apply_input, &context ) == ACKORD_OK );
    CHECK( context.count == 1 && context.calls == 2 );
    CHECK( ackord_server_process_checked( server, 2, __ackord_apply_checked, &context ) == ACKORD_OK );
    CHECK( context.count == 3 && context.sequences[1] == 1 && context.sequences[2] == 2 );
    CHECK( context.held[1] == 0 );
    CHECK( ackord_server_confirmed( server, 1, &prefix ) == ACKORD_OK && prefix == 2 );
    CHECK( ackord_server_submit( server, &command ) == ACKORD_DUPLICATE );
    command.step = 3;
    CHECK( ackord_server_submit( server, &command ) == ACKORD_CONFLICT );
    CHECK( ackord_server_lookup( server, 1, 1, 1, &view ) == ACKORD_OK && view.command.step == 2 );
    CHECK( ackord_server_set_ordering( server, ACKORD_ORDER_BY_STEP ) == ACKORD_BUSY );
    CHECK( ackord_server_reset( server, 2 ) == ACKORD_OK );
    CHECK( ackord_server_set_ordering( server, ACKORD_ORDER_BY_STEP ) == ACKORD_OK );
    ackord_server_destroy( server );
    return 1;
}

static int __ackord_test_strict_window_and_rejection( void ) {
    ackord_config config = __ackord_test_config();
    ackord_server * server = NULL;
    ackord_command command = { 1, 1, 64, 0, 2, NULL, 0 };
    ackord_confirmation confirmation;
    input_context context = { 0 };
    uint64_t sequence, prefix, mask;
    config.max_clients = 1;
    config.max_records = 64;
    CHECK( ackord_server_create( &config, &server ) == ACKORD_OK );
    CHECK( ackord_server_reset( server, 1 ) == ACKORD_OK );
    CHECK( ackord_server_set_ordering( server, ACKORD_ORDER_BY_SEQUENCE ) == ACKORD_OK );
    CHECK( ackord_server_register( server, 1 ) == ACKORD_OK );
    for( sequence = 64; sequence != 0; --sequence ) {
        command.sequence = sequence;
        command.step = 64 - sequence;
        command.type = sequence == 32 ? 99 : 2;
        CHECK( ackord_server_submit( server, &command ) == ACKORD_OK );
    }

    CHECK( ackord_server_process_checked( server, 62, __ackord_apply_checked, &context ) == ACKORD_OK );
    CHECK( context.count == 0 && context.calls == 1 );
    CHECK( ackord_server_process_checked( server, 63, __ackord_apply_checked, &context ) == ACKORD_OK );
    CHECK( context.count == 64 );
    for( sequence = 1; sequence <= 64; ++sequence ) {
        CHECK( context.sequences[sequence - 1] == sequence );
    }

    CHECK( ackord_server_confirmed( server, 1, &prefix ) == ACKORD_OK && prefix == 64 );
    CHECK( ackord_server_completed_mask( server, 1, &mask ) == ACKORD_OK && mask == 0 );
    CHECK( ackord_server_confirmation( server, 1, 32, &confirmation ) == ACKORD_OK && confirmation.status == 7 );
    CHECK( ackord_server_process_checked( server, 64, __ackord_apply_checked, &context ) == ACKORD_OK );
    CHECK( context.count == 64 && context.calls == 3 );
    ackord_server_destroy( server );
    return 1;
}

static int __ackord_test_strict_determinism( void ) {
    input_context first = { 0 };
    unsigned round;
    for( round = 0; round < 2; ++round ) {
        ackord_config config = __ackord_test_config();
        ackord_server * server = NULL;
        input_context context = { 0 };
        uint64_t previous[5] = { 0 };

        config.max_clients = 4;
        config.max_records = 32;
        CHECK( ackord_server_create( &config, &server ) == ACKORD_OK );
        CHECK( ackord_server_set_ordering( server, ACKORD_ORDER_BY_SEQUENCE ) == ACKORD_OK );
        CHECK( ackord_server_reset( server, 1 ) == ACKORD_OK );
        for( unsigned index = 0; index < 4; ++index ) {
            CHECK( ackord_server_register( server, round == 0 ? index + 1 : 4 - index ) == ACKORD_OK );
        }

        for( unsigned index = 0; index < 32; ++index ) {
            unsigned commandIndex = round == 0 ? index : 31 - index;
            ackord_command command = { 1, commandIndex % 4 + 1, commandIndex / 4 + 1, ( commandIndex * 7 ) % 11, 2, NULL, 0 };
            CHECK( ackord_server_submit( server, &command ) == ACKORD_OK );
        }

        CHECK( ackord_server_process_checked( server, 10, __ackord_apply_checked, &context ) == ACKORD_OK );
        CHECK( context.count == 32 );
        for( unsigned index = 0; index < 32; ++index ) {
            CHECK( context.sequences[index] == ++previous[context.clients[index]] );
            if( round != 0 ) {
                CHECK( context.clients[index] == first.clients[index] );
                CHECK( context.sequences[index] == first.sequences[index] );
            }
        }

        first = context;
        ackord_server_destroy( server );
    }

    return 1;
}

static int __ackord_test_lookup( void ) {
    ackord_config config = __ackord_test_config();
    ackord_server * server = NULL;
    char payload[] = "abc";
    ackord_command command = { 7, 1, 2, 4, 99, payload, sizeof( payload ) };
    ackord_command_view view;
    ackord_journal_entry entry;
    input_context context = { 0 };
    uint64_t prefix;
    config.max_records = 4;
    CHECK( ackord_server_create( &config, &server ) == ACKORD_OK );
    CHECK( ackord_server_reset( server, 7 ) == ACKORD_OK );
    CHECK( ackord_server_register( server, 1 ) == ACKORD_OK );
    CHECK( ackord_server_submit( server, &command ) == ACKORD_OK );
    payload[0] = 'z';
    CHECK( ackord_server_lookup( server, 7, 1, 2, &view ) == ACKORD_OK );
    CHECK( view.state == ACKORD_COMMAND_PENDING && view.command.step == 4 );
    CHECK( memcmp( view.command.payload, "abc", sizeof( payload ) ) == 0 );
    CHECK( view.confirmation.sequence == 0 && view.confirmation.result == NULL );
    CHECK( ackord_server_lookup( server, 8, 1, 2, &view ) == ACKORD_WRONG_SESSION );
    CHECK( view.state == 0 && view.command.payload == NULL && view.confirmation.result == NULL );
    CHECK( ackord_server_lookup( server, 7, 1, 0, &view ) == ACKORD_INVALID );
    CHECK( ackord_server_lookup( server, 7, 0, 2, &view ) == ACKORD_INVALID );
    CHECK( ackord_server_lookup( server, 7, 2, 2, &view ) == ACKORD_NOT_FOUND );
    CHECK( ackord_server_lookup( server, 7, 1, UINT64_MAX, &view ) == ACKORD_NOT_FOUND );
    CHECK( ackord_server_lookup( server, 7, 1, 2, NULL ) == ACKORD_INVALID );
    CHECK( ackord_server_process_checked( server, 9, __ackord_apply_checked, &context ) == ACKORD_OK );
    CHECK( ackord_server_lookup( server, 7, 1, 2, &view ) == ACKORD_OK );
    CHECK( view.state == ACKORD_COMMAND_COMPLETED && view.command.step == 4 );
    CHECK( view.confirmation.step == 9 && view.confirmation.status == 7 );
    CHECK( ackord_server_journal_next( server, 0, &entry ) == ACKORD_OK );
    CHECK( ackord_server_journal_trim( server, entry.index ) == ACKORD_OK );
    CHECK( ackord_server_lookup( server, 7, 1, 2, &view ) == ACKORD_STALE );
    CHECK( view.state == 0 && view.command.payload == NULL );
    CHECK( ackord_server_confirmed( server, 1, &prefix ) == ACKORD_OK && prefix == 0 );
    command.sequence = 1;
    command.type = 2;
    command.payload = NULL;
    command.payload_size = 0;
    CHECK( ackord_server_submit( server, &command ) == ACKORD_OK );
    CHECK( ackord_server_process_checked( server, 9, __ackord_apply_checked, &context ) == ACKORD_OK );
    CHECK( ackord_server_confirmed( server, 1, &prefix ) == ACKORD_OK && prefix == 2 );
    CHECK( ackord_server_lookup( server, 7, 1, 2, &view ) == ACKORD_STALE );
    CHECK( ackord_server_lookup( server, 7, 1, 1, &view ) == ACKORD_OK );
    CHECK( ackord_server_reset( server, 8 ) == ACKORD_OK );
    CHECK( ackord_server_lookup( server, 7, 1, 1, &view ) == ACKORD_WRONG_SESSION );
    ackord_server_destroy( server );
    return 1;
}

static int __ackord_test_suspend_resume( void ) {
    unsigned mode;
    for( mode = 0; mode < 2; ++mode ) {
        ackord_config config = __ackord_test_config();
        ackord_server * server = NULL;
        ackord_client * client = NULL;
        ackord_command first, second, other = { 1, 2, 1, 0, 2, NULL, 0 };
        ackord_command_view view;
        input_context context = { 0 };
        uint64_t prefix, mask;
        config.max_clients = 2;
        config.max_records = 4;
        CHECK( ackord_server_create( &config, &server ) == ACKORD_OK );
        CHECK( ackord_client_create( &config, &client ) == ACKORD_OK );
        CHECK( ackord_server_reset( server, 1 ) == ACKORD_OK );
        CHECK( ackord_server_set_ordering( server, (ackord_ordering) mode ) == ACKORD_OK );
        CHECK( ackord_server_register( server, 1 ) == ACKORD_OK );
        CHECK( ackord_server_register( server, 2 ) == ACKORD_OK );
        CHECK( ackord_client_reset( client, 1, 1 ) == ACKORD_OK );
        CHECK( ackord_client_submit( client, 0, 1, NULL, 0, &first ) == ACKORD_OK );
        CHECK( ackord_server_submit( server, &first ) == ACKORD_OK );
        CHECK( ackord_server_suspend( server, 1 ) == ACKORD_OK );
        CHECK( ackord_server_suspend( server, 1 ) == ACKORD_DUPLICATE );
        CHECK( ackord_server_register( server, 1 ) == ACKORD_DUPLICATE );
        CHECK( ackord_server_submit( server, &other ) == ACKORD_OK );
        CHECK( ackord_server_process_checked( server, 0, __ackord_apply_checked, &context ) == ACKORD_OK );
        CHECK( context.count == 1 && context.clients[0] == 2 );
        CHECK( ackord_server_lookup( server, 1, 1, 1, &view ) == ACKORD_OK && view.state == ACKORD_COMMAND_PENDING );
        CHECK( ackord_server_confirmed( server, 1, &prefix ) == ACKORD_OK && prefix == 0 );
        CHECK( ackord_client_submit( client, 0, 2, NULL, 0, &second ) == ACKORD_OK );
        CHECK( ackord_server_submit( server, &second ) == ACKORD_SUSPENDED );
        CHECK( ackord_client_reset( client, 1, 1 ) == ACKORD_DUPLICATE );
        CHECK( ackord_client_pending_count( client ) == 2 );
        CHECK( ackord_server_resume( server, 1 ) == ACKORD_OK );
        CHECK( ackord_server_resume( server, 1 ) == ACKORD_DUPLICATE );
        CHECK( ackord_server_submit( server, &second ) == ACKORD_OK );
        CHECK( ackord_server_process_checked( server, 0, __ackord_apply_checked, &context ) == ACKORD_OK );
        CHECK( context.count == 3 && context.held[1] == 0 );
        CHECK( ackord_server_confirmed( server, 1, &prefix ) == ACKORD_OK && prefix == 2 );
        CHECK( ackord_server_completed_mask( server, 1, &mask ) == ACKORD_OK && mask == 0 );
        CHECK( ackord_client_confirm_progress( client, 1, 1, prefix, mask ) == ACKORD_OK );
        CHECK( ackord_client_pending_count( client ) == 0 );
        CHECK( ackord_client_submit( client, 0, 1, NULL, 0, &first ) == ACKORD_OK && first.sequence == 3 );
        CHECK( ackord_server_submit( server, &first ) == ACKORD_OK );
        CHECK( ackord_server_process_checked( server, 0, __ackord_apply_checked, &context ) == ACKORD_OK );
        CHECK( context.held[1] == 1 );
        CHECK( ackord_server_suspend( server, 1 ) == ACKORD_OK );
        CHECK( ackord_server_confirmed( server, 1, &prefix ) == ACKORD_OK && prefix == 3 );
        CHECK( ackord_server_resume( server, 1 ) == ACKORD_OK );
        CHECK( ackord_server_submit( server, &first ) == ACKORD_DUPLICATE );
        CHECK( ackord_server_suspend( server, 0 ) == ACKORD_INVALID );
        CHECK( ackord_server_resume( server, 99 ) == ACKORD_NOT_FOUND );
        ackord_client_destroy( client );
        ackord_server_destroy( server );
    }

    return 1;
}

static int __ackord_test_retired_identity( void ) {
    ackord_config config = __ackord_test_config();
    ackord_server * server = NULL;
    ackord_command command = { 1, 1, 1, 0, 2, NULL, 0 };
    ackord_command_view view;
    ackord_journal_entry entry;
    input_context context = { 0 };
    config.max_clients = 1;
    config.max_records = 3;
    CHECK( ackord_server_create( &config, &server ) == ACKORD_OK );
    CHECK( ackord_server_reset( server, 1 ) == ACKORD_OK );
    CHECK( ackord_server_register( server, 1 ) == ACKORD_OK );
    CHECK( ackord_server_submit( server, &command ) == ACKORD_OK );
    CHECK( ackord_server_process_checked( server, 0, __ackord_apply_checked, &context ) == ACKORD_OK );
    command.sequence = 2;
    CHECK( ackord_server_submit( server, &command ) == ACKORD_OK );
    CHECK( ackord_server_suspend( server, 1 ) == ACKORD_OK );
    CHECK( ackord_server_unregister( server, 1 ) == ACKORD_OK );
    CHECK( ackord_server_unregister( server, 1 ) == ACKORD_DUPLICATE );
    CHECK( ackord_server_register( server, 1 ) == ACKORD_RETIRED );
    CHECK( ackord_server_suspend( server, 1 ) == ACKORD_RETIRED );
    CHECK( ackord_server_resume( server, 1 ) == ACKORD_RETIRED );
    CHECK( ackord_server_submit( server, &command ) == ACKORD_RETIRED );
    CHECK( ackord_server_lookup( server, 1, 1, 2, &view ) == ACKORD_NOT_FOUND );
    CHECK( ackord_server_lookup( server, 1, 1, 1, &view ) == ACKORD_OK && view.state == ACKORD_COMMAND_COMPLETED );
    CHECK( ackord_server_register( server, 2 ) == ACKORD_FULL );
    CHECK( ackord_server_journal_next( server, 0, &entry ) == ACKORD_OK );
    CHECK( ackord_server_journal_trim( server, entry.index ) == ACKORD_OK );
    CHECK( ackord_server_lookup( server, 1, 1, 1, &view ) == ACKORD_STALE );
    CHECK( ackord_server_reset( server, 1 ) == ACKORD_DUPLICATE );
    CHECK( ackord_server_register( server, 1 ) == ACKORD_RETIRED );
    CHECK( ackord_server_reset( server, 2 ) == ACKORD_OK );
    CHECK( ackord_server_register( server, 1 ) == ACKORD_OK );
    CHECK( ackord_server_submit( server, &command ) == ACKORD_WRONG_SESSION );
    command.session = 2;
    command.sequence = 1;
    CHECK( ackord_server_submit( server, &command ) == ACKORD_OK );
    CHECK( ackord_server_process_checked( server, 0, __ackord_apply_checked, &context ) == ACKORD_OK );
    CHECK( context.count == 2 );
    ackord_server_destroy( server );
    return 1;
}

static int __ackord_test_suspended_selective_progress( void ) {
    ackord_config config = __ackord_test_config();
    ackord_server * server = NULL;
    ackord_command command = { 1, 1, 2, 0, 2, NULL, 0 };
    input_context context = { 0 };
    uint64_t prefix, mask;
    config.max_records = 3;
    CHECK( ackord_server_create( &config, &server ) == ACKORD_OK );
    CHECK( ackord_server_reset( server, 1 ) == ACKORD_OK );
    CHECK( ackord_server_register( server, 1 ) == ACKORD_OK );
    CHECK( ackord_server_submit( server, &command ) == ACKORD_OK );
    command.sequence = 1;
    command.step = 10;
    CHECK( ackord_server_submit( server, &command ) == ACKORD_OK );
    CHECK( ackord_server_process_checked( server, 0, __ackord_apply_checked, &context ) == ACKORD_OK );
    CHECK( context.count == 1 );
    CHECK( ackord_server_completed_mask( server, 1, &mask ) == ACKORD_OK && mask == 2 );
    CHECK( ackord_server_suspend( server, 1 ) == ACKORD_OK );
    CHECK( ackord_server_process_checked( server, 10, __ackord_apply_checked, &context ) == ACKORD_OK );
    CHECK( context.count == 1 );
    CHECK( ackord_server_confirmed( server, 1, &prefix ) == ACKORD_OK && prefix == 0 );
    CHECK( ackord_server_completed_mask( server, 1, &mask ) == ACKORD_OK && mask == 2 );
    CHECK( ackord_server_resume( server, 1 ) == ACKORD_OK );
    CHECK( ackord_server_process_checked( server, 10, __ackord_apply_checked, &context ) == ACKORD_OK );
    CHECK( context.count == 2 && context.sequences[1] == 1 );
    CHECK( ackord_server_confirmed( server, 1, &prefix ) == ACKORD_OK && prefix == 2 );
    CHECK( ackord_server_completed_mask( server, 1, &mask ) == ACKORD_OK && mask == 0 );
    ackord_server_destroy( server );
    return 1;
}

typedef struct failure_context {
    ackord_server * server;
    unsigned mode;
    size_t calls;
    size_t effects;
    int guardsOk;
} failure_context;

static ackord_status __ackord_fail_handler( void * _user, const ackord_command * _commands, ackord_outcome * _outcomes, size_t _count ) {
    failure_context * context = _user;
    ackord_command_view view;
    ++context->calls;
    ++context->effects;
    context->guardsOk = ackord_server_reset( context->server, 2 ) == ACKORD_BUSY;
    context->guardsOk &= ackord_server_suspend( context->server, 1 ) == ACKORD_BUSY;
    context->guardsOk &= ackord_server_resume( context->server, 1 ) == ACKORD_BUSY;
    context->guardsOk &= ackord_server_set_ordering( context->server, ACKORD_ORDER_BY_STEP ) == ACKORD_BUSY;
    context->guardsOk &= ackord_server_process_checked( context->server, 0, __ackord_fail_handler, context ) == ACKORD_BUSY;
    if( _count != 0 ) {
        context->guardsOk &= ackord_server_lookup( context->server, _commands[0].session, _commands[0].client, _commands[0].sequence, &view ) == ACKORD_OK;
        context->guardsOk &= view.state == ACKORD_COMMAND_PENDING && view.confirmation.sequence == 0;
    }

    if( context->mode == 0 ) {
        return ACKORD_INVALID;
    }

    if( _count < 2 ) {
        return ACKORD_INVALID;
    }

    if( context->mode == 1 ) {
        _outcomes[1].result_size = _outcomes[1].result_capacity + 1;
    }

    if( context->mode == 2 ) {
        _outcomes[1].result = NULL;
    }

    if( context->mode == 3 ) {
        _outcomes[1].result_capacity = 0;
    }

    if( context->mode == 4 ) {
        _outcomes[1].status = INT32_MIN;
    }

    return ACKORD_OK;
}

static void __ackord_fail_legacy_handler( void * _user, const ackord_command * _commands, ackord_outcome * _outcomes, size_t _count ) {
    (void) __ackord_fail_handler( _user, _commands, _outcomes, _count );
}

static int __ackord_test_failed_batch( void ) {
    unsigned mode;
    for( mode = 0; mode < 6; ++mode ) {
        ackord_config config = __ackord_test_config();
        ackord_server * server = NULL;
        ackord_command command = { 1, 1, 1, 0, 2, NULL, 0 };
        ackord_command_view view;
        ackord_confirmation confirmation;
        ackord_journal_entry entry;
        input_context context = { 0 };
        failure_context failure = { 0 };
        uint64_t prefix, mask;
        config.max_records = 4;
        CHECK( ackord_server_create( &config, &server ) == ACKORD_OK );
        CHECK( ackord_server_reset( server, 1 ) == ACKORD_OK );
        CHECK( ackord_server_register( server, 1 ) == ACKORD_OK );
        CHECK( ackord_server_submit( server, &command ) == ACKORD_OK );
        CHECK( ackord_server_process_checked( server, 0, __ackord_apply_checked, &context ) == ACKORD_OK );
        command.sequence = 2;
        CHECK( ackord_server_submit( server, &command ) == ACKORD_OK );
        command.sequence = 3;
        CHECK( ackord_server_submit( server, &command ) == ACKORD_OK );
        failure.server = server;
        failure.mode = mode == 5 ? 1 : mode;
        if( mode == 5 ) {
            CHECK( ackord_server_process( server, 0, __ackord_fail_legacy_handler, &failure ) == ACKORD_HANDLER_FAILED );
        } else {
            CHECK( ackord_server_process_checked( server, 0, __ackord_fail_handler, &failure ) == ACKORD_HANDLER_FAILED );
        }

        CHECK( failure.calls == 1 && failure.effects == 1 && failure.guardsOk );
        CHECK( ackord_server_confirmed( server, 1, &prefix ) == ACKORD_OK && prefix == 1 );
        CHECK( ackord_server_completed_mask( server, 1, &mask ) == ACKORD_OK && mask == 0 );
        CHECK( ackord_server_confirmation( server, 1, 1, &confirmation ) == ACKORD_OK );
        CHECK( ackord_server_confirmation( server, 1, 2, &confirmation ) == ACKORD_NOT_FOUND );
        CHECK( ackord_server_lookup( server, 1, 1, 2, &view ) == ACKORD_OK && view.state == ACKORD_COMMAND_PENDING );
        CHECK( ackord_server_lookup( server, 1, 1, 3, &view ) == ACKORD_OK && view.state == ACKORD_COMMAND_PENDING );
        CHECK( ackord_server_journal_next( server, 0, &entry ) == ACKORD_OK && entry.index == 1 );
        CHECK( ackord_server_journal_next( server, entry.index, &entry ) == ACKORD_NOT_FOUND );
        CHECK( ackord_server_process( server, 1, __ackord_apply_input, &context ) == ACKORD_FAULTED );
        CHECK( ackord_server_process_checked( server, 1, __ackord_fail_handler, &failure ) == ACKORD_FAULTED );
        CHECK( failure.calls == 1 && context.calls == 1 );
        CHECK( ackord_server_submit( server, &command ) == ACKORD_FAULTED );
        CHECK( ackord_server_register( server, 2 ) == ACKORD_FAULTED );
        CHECK( ackord_server_suspend( server, 1 ) == ACKORD_FAULTED );
        CHECK( ackord_server_resume( server, 1 ) == ACKORD_FAULTED );
        CHECK( ackord_server_unregister( server, 1 ) == ACKORD_FAULTED );
        CHECK( ackord_server_journal_trim( server, 1 ) == ACKORD_FAULTED );
        CHECK( ackord_server_set_ordering( server, ACKORD_ORDER_BY_SEQUENCE ) == ACKORD_FAULTED );
        CHECK( ackord_server_reset( server, 1 ) == ACKORD_DUPLICATE );
        CHECK( ackord_server_process_checked( server, 1, __ackord_apply_checked, &context ) == ACKORD_FAULTED );
        CHECK( ackord_server_reset( server, 2 ) == ACKORD_OK );
        CHECK( ackord_server_register( server, 1 ) == ACKORD_OK );
        CHECK( ackord_server_process_checked( server, 0, __ackord_apply_checked, &context ) == ACKORD_OK );
        ackord_server_destroy( server );
    }

    return 1;
}

static int __ackord_test_empty_failure( void ) {
    ackord_config config = __ackord_test_config();
    ackord_server * server = NULL;
    failure_context context = { 0 };
    CHECK( ackord_server_create( &config, &server ) == ACKORD_OK );
    CHECK( ackord_server_reset( server, 1 ) == ACKORD_OK );
    context.server = server;
    CHECK( ackord_server_process_checked( server, 0, __ackord_fail_handler, &context ) == ACKORD_HANDLER_FAILED );
    CHECK( context.calls == 1 && context.guardsOk );
    CHECK( ackord_server_process_checked( server, 0, __ackord_fail_handler, &context ) == ACKORD_FAULTED );
    CHECK( context.calls == 1 );
    ackord_server_destroy( server );
    return 1;
}

typedef struct allocation_context {
    size_t allocations;
    size_t outstanding;
    int deny;
} allocation_context;

static void * __ackord_allocate( void * _user, size_t _size ) {
    allocation_context * context = _user;
    void * memory;
    ++context->allocations;
    if( context->deny != 0 ) {
        return NULL;
    }

    memory = malloc( _size );
    if( memory != NULL ) {
        ++context->outstanding;
    }

    return memory;
}

static void __ackord_deallocate( void * _user, void * _memory ) {
    allocation_context * context = _user;
    --context->outstanding;
    free( _memory );
}

static int __ackord_test_reserved_storage( void ) {
    ackord_config config = __ackord_test_config();
    ackord_server * server = NULL;
    allocation_context allocator = { 0 };
    input_context context = { 0 };
    ackord_command command = { 1, 1, 2, 0, 2, NULL, 0 };
    ackord_command_view view;
    size_t allocations;
    config.max_clients = 2;
    config.max_records = 4;
    config.allocator.allocate = __ackord_allocate;
    config.allocator.deallocate = __ackord_deallocate;
    config.allocator.user = &allocator;
    CHECK( ackord_server_create( &config, &server ) == ACKORD_OK );
    allocations = allocator.allocations;
    allocator.deny = 1;
    CHECK( ackord_server_set_ordering( server, ACKORD_ORDER_BY_SEQUENCE ) == ACKORD_OK );
    CHECK( ackord_server_reset( server, 1 ) == ACKORD_OK );
    CHECK( ackord_server_register( server, 1 ) == ACKORD_OK );
    CHECK( ackord_server_submit( server, &command ) == ACKORD_OK );
    command.sequence = 1;
    CHECK( ackord_server_submit( server, &command ) == ACKORD_OK );
    CHECK( ackord_server_suspend( server, 1 ) == ACKORD_OK );
    CHECK( ackord_server_process_checked( server, 0, __ackord_apply_checked, &context ) == ACKORD_OK && context.count == 0 );
    CHECK( ackord_server_resume( server, 1 ) == ACKORD_OK );
    CHECK( ackord_server_process_checked( server, 0, __ackord_apply_checked, &context ) == ACKORD_OK && context.count == 2 );
    CHECK( ackord_server_lookup( server, 1, 1, 1, &view ) == ACKORD_OK );
    CHECK( ackord_server_journal_trim( server, UINT64_MAX ) == ACKORD_OK );
    CHECK( ackord_server_unregister( server, 1 ) == ACKORD_OK );
    CHECK( ackord_server_register( server, 1 ) == ACKORD_RETIRED );
    CHECK( allocator.allocations == allocations );
    ackord_server_destroy( server );
    CHECK( allocator.outstanding == 0 );
    return 1;
}

int main( void ) {
    if( __ackord_test_strict_trigger_order() == 0 ) {
        return EXIT_FAILURE;
    }

    if( __ackord_test_strict_window_and_rejection() == 0 ) {
        return EXIT_FAILURE;
    }

    if( __ackord_test_strict_determinism() == 0 ) {
        return EXIT_FAILURE;
    }

    if( __ackord_test_lookup() == 0 ) {
        return EXIT_FAILURE;
    }

    if( __ackord_test_suspend_resume() == 0 ) {
        return EXIT_FAILURE;
    }

    if( __ackord_test_retired_identity() == 0 ) {
        return EXIT_FAILURE;
    }

    if( __ackord_test_suspended_selective_progress() == 0 ) {
        return EXIT_FAILURE;
    }

    if( __ackord_test_failed_batch() == 0 ) {
        return EXIT_FAILURE;
    }

    if( __ackord_test_empty_failure() == 0 ) {
        return EXIT_FAILURE;
    }

    if( __ackord_test_reserved_storage() == 0 ) {
        return EXIT_FAILURE;
    }

    puts( "Ackord: strict ordering, lookup, stream lifecycle and checked processing passed" );
    return EXIT_SUCCESS;
}
