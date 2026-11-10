#include "ackord/ackord.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void * __ackord_counter_allocate( void * _user, size_t _size ) {
    (void) _user;
    void * memory = malloc( _size );

    return memory;
}

static void __ackord_counter_deallocate( void * _user, void * _memory ) {
    (void) _user;
    free( _memory );
}

static void __ackord_increment( void * _user, const ackord_command * _commands, ackord_outcome * _outcomes, size_t _count ) {
    int32_t * counter = _user;

    for( size_t index = 0; index < _count; ++index ) {
        int32_t delta = 0;
        if( _commands[index].payload_size != sizeof( delta ) ) {
            _outcomes[index].status = 1;
            continue;
        }

        memcpy( &delta, _commands[index].payload, sizeof( delta ) );
        *counter += delta;
        memcpy( _outcomes[index].result, counter, sizeof( *counter ) );
        _outcomes[index].result_size = sizeof( *counter );
    }
}

static int __ackord_counter_run( ackord_server * _server, ackord_client * _client ) {
    if( ackord_server_reset( _server, 1 ) != ACKORD_OK ) {
        return 0;
    }

    if( ackord_server_register( _server, 1 ) != ACKORD_OK ) {
        return 0;
    }

    if( ackord_client_reset( _client, 1, 1 ) != ACKORD_OK ) {
        return 0;
    }

    int32_t delta = 3;
    ackord_command command;
    if( ackord_client_submit( _client, 0, 1, &delta, sizeof( delta ), &command ) != ACKORD_OK ) {
        return 0;
    }

    if( ackord_server_submit( _server, &command ) != ACKORD_OK ) {
        return 0;
    }

    int32_t counter = 0;
    if( ackord_server_process( _server, 0, __ackord_increment, &counter ) != ACKORD_OK ) {
        return 0;
    }

    if( ackord_server_submit( _server, &command ) != ACKORD_DUPLICATE ) {
        return 0;
    }

    if( ackord_server_process( _server, 1, __ackord_increment, &counter ) != ACKORD_OK ) {
        return 0;
    }

    ackord_confirmation confirmation;
    if( ackord_server_confirmation( _server, 1, command.sequence, &confirmation ) != ACKORD_OK ) {
        return 0;
    }

    if( ackord_client_confirm( _client, &confirmation ) != ACKORD_OK ) {
        return 0;
    }

    size_t pendingCount = ackord_client_pending_count( _client );
    int successful = counter == 3 && pendingCount == 0;
    printf( "counter=%d pending=%zu\n", counter, pendingCount );

    return successful;
}

int main( void ) {
    ackord_config config = { 1, 4, sizeof( int32_t ), sizeof( int32_t ), 4, { __ackord_counter_allocate, __ackord_counter_deallocate, NULL } };
    ackord_server * server = NULL;
    if( ackord_server_create( &config, &server ) != ACKORD_OK ) {
        return 1;
    }

    ackord_client * client = NULL;
    if( ackord_client_create( &config, &client ) != ACKORD_OK ) {
        ackord_server_destroy( server );
        return 1;
    }

    int successful = __ackord_counter_run( server, client );
    ackord_client_destroy( client );
    ackord_server_destroy( server );
    return successful ? 0 : 1;
}
