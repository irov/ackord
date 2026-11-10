#include "ackord/ackord.h"

typedef struct ackord_record {
    unsigned state; /* 0 free, 1 pending, 2 completed */
    uint64_t index;
    ackord_command command;
    ackord_confirmation confirmation;
    unsigned char * storage;
    struct ackord_record * next;
} ackord_record;

typedef struct ackord_peer {
    uint32_t id;
    uint64_t confirmed;
    uint64_t completed;
    unsigned state;
    ackord_record * next;
} ackord_peer;

enum {
    ACKORD_PEER_ACTIVE = 1,
    ACKORD_PEER_SUSPENDED,
    ACKORD_PEER_RETIRED
};

struct ackord_server {
    ackord_config config;
    uint64_t session;
    uint64_t journalIndex;
    size_t journalCount;
    int processing;
    int faulted;
    ackord_ordering ordering;
    ackord_record * records;
    ackord_record * freeRecords;
    ackord_record ** identities;
    ackord_peer * peers;
    ackord_command * batch;
    ackord_outcome * outcomes;
    ackord_record ** selected;
    ackord_record ** journal;
    unsigned char * storage;
};

struct ackord_client {
    ackord_config config;
    uint64_t session;
    uint32_t id;
    uint64_t nextSequence;
    size_t count;
    ackord_record * records;
    unsigned char * storage;
};

static inline void __ackord_zero( void * _memory, size_t _size ) {
    unsigned char * bytes = _memory;

    for( size_t index = 0; index < _size; ++index ) {
        bytes[index] = 0;
    }
}

static inline void __ackord_copy( void * _destination, const void * _source, size_t _size ) {
    unsigned char * output = _destination;
    const unsigned char * input = _source;

    for( size_t index = 0; index < _size; ++index ) {
        output[index] = input[index];
    }
}

static inline int __ackord_equal( const void * _left, const void * _right, size_t _size ) {
    const unsigned char * leftBytes = _left;
    const unsigned char * rightBytes = _right;

    for( size_t index = 0; index < _size; ++index ) {
        if( leftBytes[index] != rightBytes[index] ) {
            return 0;
        }
    }

    return 1;
}

static int __ackord_config_valid( const ackord_config * _config ) {
    if( _config == NULL ) {
        return 0;
    }

    if( _config->allocator.allocate == NULL ) {
        return 0;
    }

    if( _config->allocator.deallocate == NULL ) {
        return 0;
    }

    if( _config->max_clients == 0 ) {
        return 0;
    }

    if( _config->max_records == 0 ) {
        return 0;
    }

    if( _config->sequence_window == 0 ) {
        return 0;
    }

    if( _config->sequence_window > 64 ) {
        return 0;
    }

    if( _config->max_payload_size > SIZE_MAX - _config->max_result_size ) {
        return 0;
    }

    if( (size_t) _config->max_payload_size + _config->max_result_size == 0 ) {
        return 0;
    }

    if( _config->max_records > SIZE_MAX / ( (size_t) _config->max_payload_size + _config->max_result_size ) ) {
        return 0;
    }

    if( _config->max_records > SIZE_MAX / sizeof( ackord_record ) ) {
        return 0;
    }

    if( _config->max_records > SIZE_MAX / sizeof( ackord_outcome ) ) {
        return 0;
    }

    if( _config->max_clients > SIZE_MAX / sizeof( ackord_peer ) ) {
        return 0;
    }

    return 1;
}

static inline void * __ackord_array( const ackord_config * _config, size_t _count, size_t _size ) {
    void * memory = _config->allocator.allocate( _config->allocator.user, _count * _size );
    if( memory == NULL ) {
        return NULL;
    }

    __ackord_zero( memory, _count * _size );

    return memory;
}

static inline void __ackord_free( const ackord_config * _config, void * _memory ) {
    if( _memory == NULL ) {
        return;
    }

    _config->allocator.deallocate( _config->allocator.user, _memory );
}

ackord_status ackord_server_create( const ackord_config * _config, ackord_server ** _out ) {
    if( _out == NULL ) {
        return ACKORD_INVALID;
    }

    *_out = NULL;
    if( __ackord_config_valid( _config ) == 0 ) {
        return ACKORD_INVALID;
    }

    ackord_server * server = __ackord_array( _config, 1, sizeof( *server ) );
    if( server == NULL ) {
        return ACKORD_NO_MEMORY;
    }

    __ackord_copy( &server->config, _config, sizeof( server->config ) );
    server->records = __ackord_array( _config, _config->max_records, sizeof( *server->records ) );
    if( server->records == NULL ) {
        ackord_server_destroy( server );
        return ACKORD_NO_MEMORY;
    }

    server->identities = __ackord_array( _config, _config->max_records, sizeof( *server->identities ) );
    if( server->identities == NULL ) {
        ackord_server_destroy( server );
        return ACKORD_NO_MEMORY;
    }

    server->peers = __ackord_array( _config, _config->max_clients, sizeof( *server->peers ) );
    if( server->peers == NULL ) {
        ackord_server_destroy( server );
        return ACKORD_NO_MEMORY;
    }

    server->batch = __ackord_array( _config, _config->max_records, sizeof( *server->batch ) );
    if( server->batch == NULL ) {
        ackord_server_destroy( server );
        return ACKORD_NO_MEMORY;
    }

    server->outcomes = __ackord_array( _config, _config->max_records, sizeof( *server->outcomes ) );
    if( server->outcomes == NULL ) {
        ackord_server_destroy( server );
        return ACKORD_NO_MEMORY;
    }

    server->selected = __ackord_array( _config, _config->max_records, sizeof( *server->selected ) );
    if( server->selected == NULL ) {
        ackord_server_destroy( server );
        return ACKORD_NO_MEMORY;
    }

    server->journal = __ackord_array( _config, _config->max_records, sizeof( *server->journal ) );
    if( server->journal == NULL ) {
        ackord_server_destroy( server );
        return ACKORD_NO_MEMORY;
    }

    server->storage = __ackord_array( _config, _config->max_records, (size_t) _config->max_payload_size + _config->max_result_size );
    if( server->storage == NULL ) {
        ackord_server_destroy( server );
        return ACKORD_NO_MEMORY;
    }

    for( size_t index = 0; index < _config->max_records; ++index ) {
        ackord_record * record = &server->records[index];
        record->storage = server->storage + index * ( (size_t) _config->max_payload_size + _config->max_result_size );
        record->next = server->freeRecords;
        server->freeRecords = record;
    }

    *_out = server;
    return ACKORD_OK;
}

void ackord_server_destroy( ackord_server * _server ) {
    if( _server == NULL ) {
        return;
    }
    /* Destruction from a handler is a programmer error, not a deferred action. */
    if( _server->processing != 0 ) {
        return;
    }

    ackord_config config;
    __ackord_copy( &config, &_server->config, sizeof( config ) );
    __ackord_free( &config, _server->records );
    __ackord_free( &config, _server->identities );
    __ackord_free( &config, _server->peers );
    __ackord_free( &config, _server->batch );
    __ackord_free( &config, _server->outcomes );
    __ackord_free( &config, _server->selected );
    __ackord_free( &config, _server->journal );
    __ackord_free( &config, _server->storage );
    __ackord_free( &config, _server );
}

ackord_status ackord_server_reset( ackord_server * _server, uint64_t _session ) {
    if( _server == NULL ) {
        return ACKORD_INVALID;
    }

    if( _session == 0 ) {
        return ACKORD_INVALID;
    }

    if( _server->processing != 0 ) {
        return ACKORD_BUSY;
    }

    if( _server->session == _session ) {
        return ACKORD_DUPLICATE;
    }

    _server->session = _session;
    _server->journalIndex = 0;
    _server->journalCount = 0;
    _server->faulted = 0;
    __ackord_zero( _server->peers, _server->config.max_clients * sizeof( *_server->peers ) );
    __ackord_zero( _server->identities, _server->config.max_records * sizeof( *_server->identities ) );
    _server->freeRecords = NULL;
    for( size_t index = 0; index < _server->config.max_records; ++index ) {
        ackord_record * record = &_server->records[index];
        record->state = 0;
        record->next = _server->freeRecords;
        _server->freeRecords = record;
    }

    return ACKORD_OK;
}

ackord_status ackord_server_set_ordering( ackord_server * _server, ackord_ordering _ordering ) {
    if( _server == NULL ) {
        return ACKORD_INVALID;
    }

    if( _ordering != ACKORD_ORDER_BY_STEP && _ordering != ACKORD_ORDER_BY_SEQUENCE ) {
        return ACKORD_INVALID;
    }

    if( _server->processing != 0 ) {
        return ACKORD_BUSY;
    }

    if( _server->faulted != 0 ) {
        return ACKORD_FAULTED;
    }

    if( _server->ordering == _ordering ) {
        return ACKORD_OK;
    }

    if( _server->journalIndex != 0 ) {
        return ACKORD_BUSY;
    }

    for( size_t index = 0; index < _server->config.max_records; ++index ) {
        if( _server->records[index].state != 0 ) {
            return ACKORD_BUSY;
        }
    }

    _server->ordering = _ordering;
    return ACKORD_OK;
}

static ackord_peer * __ackord_find_peer( const ackord_server * _server, uint32_t _id ) {
    for( size_t index = 0; index < _server->config.max_clients; ++index ) {
        if( _server->peers[index].id == _id ) {
            return &_server->peers[index];
        }
    }

    return NULL;
}

static inline size_t __ackord_identity_bucket( const ackord_server * _server, uint32_t _client, uint64_t _sequence ) {
    uint64_t hash = _sequence ^ ( (uint64_t) _client * UINT64_C( 0x9e3779b97f4a7c15 ) );
    hash ^= hash >> 30;
    hash *= UINT64_C( 0xbf58476d1ce4e5b9 );
    hash ^= hash >> 27;
    hash *= UINT64_C( 0x94d049bb133111eb );
    hash ^= hash >> 31;
    size_t bucket = (size_t) ( hash % _server->config.max_records );

    return bucket;
}

static ackord_record * __ackord_find_record( const ackord_server * _server, uint32_t _client, uint64_t _sequence ) {
    size_t bucket = __ackord_identity_bucket( _server, _client, _sequence );
    for( ackord_record * record = _server->identities[bucket]; record != NULL; record = record->next ) {
        if( record->command.client != _client ) {
            continue;
        }

        if( record->command.sequence != _sequence ) {
            continue;
        }

        return record;
    }

    return NULL;
}

static void __ackord_release_record( ackord_server * _server, ackord_record * _record ) {
    size_t bucket = __ackord_identity_bucket( _server, _record->command.client, _record->command.sequence );
    ackord_record ** link = &_server->identities[bucket];
    while( *link != NULL ) {
        if( *link != _record ) {
            link = &( *link )->next;
            continue;
        }

        *link = _record->next;
        _record->state = 0;
        _record->next = _server->freeRecords;
        _server->freeRecords = _record;
        return;
    }
}

ackord_status ackord_server_register( ackord_server * _server, uint32_t _client ) {
    if( _server == NULL ) {
        return ACKORD_INVALID;
    }

    if( _client == 0 ) {
        return ACKORD_INVALID;
    }

    if( _server->session == 0 ) {
        return ACKORD_INVALID;
    }

    if( _server->processing != 0 ) {
        return ACKORD_BUSY;
    }

    if( _server->faulted != 0 ) {
        return ACKORD_FAULTED;
    }

    ackord_peer * peer = __ackord_find_peer( _server, _client );
    if( peer != NULL ) {
        if( peer->state == ACKORD_PEER_RETIRED ) {
            return ACKORD_RETIRED;
        }

        return ACKORD_DUPLICATE;
    }

    peer = __ackord_find_peer( _server, 0 );
    if( peer == NULL ) {
        return ACKORD_FULL;
    }

    __ackord_zero( peer, sizeof( *peer ) );
    peer->id = _client;
    peer->state = ACKORD_PEER_ACTIVE;
    return ACKORD_OK;
}

static ackord_status __ackord_server_set_suspended( ackord_server * _server, uint32_t _client, unsigned _state ) {
    if( _server == NULL ) {
        return ACKORD_INVALID;
    }

    if( _client == 0 ) {
        return ACKORD_INVALID;
    }

    if( _server->processing != 0 ) {
        return ACKORD_BUSY;
    }

    if( _server->faulted != 0 ) {
        return ACKORD_FAULTED;
    }

    ackord_peer * peer = __ackord_find_peer( _server, _client );
    if( peer == NULL ) {
        return ACKORD_NOT_FOUND;
    }

    if( peer->state == ACKORD_PEER_RETIRED ) {
        return ACKORD_RETIRED;
    }

    if( peer->state == _state ) {
        return ACKORD_DUPLICATE;
    }

    peer->state = _state;
    return ACKORD_OK;
}

ackord_status ackord_server_suspend( ackord_server * _server, uint32_t _client ) {
    ackord_status status = __ackord_server_set_suspended( _server, _client, ACKORD_PEER_SUSPENDED );
    return status;
}

ackord_status ackord_server_resume( ackord_server * _server, uint32_t _client ) {
    ackord_status status = __ackord_server_set_suspended( _server, _client, ACKORD_PEER_ACTIVE );
    return status;
}

ackord_status ackord_server_unregister( ackord_server * _server, uint32_t _client ) {
    if( _server == NULL ) {
        return ACKORD_INVALID;
    }

    if( _client == 0 ) {
        return ACKORD_INVALID;
    }

    if( _server->processing != 0 ) {
        return ACKORD_BUSY;
    }

    if( _server->faulted != 0 ) {
        return ACKORD_FAULTED;
    }

    ackord_peer * peer = __ackord_find_peer( _server, _client );
    if( peer == NULL ) {
        return ACKORD_NOT_FOUND;
    }

    if( peer->state == ACKORD_PEER_RETIRED ) {
        return ACKORD_DUPLICATE;
    }

    peer->state = ACKORD_PEER_RETIRED;
    peer->next = NULL;
    for( size_t index = 0; index < _server->config.max_records; ++index ) {
        if( _server->records[index].state == 1 && _server->records[index].command.client == _client ) {
            __ackord_release_record( _server, &_server->records[index] );
        }
    }

    return ACKORD_OK;
}

ackord_status ackord_server_submit( ackord_server * _server, const ackord_command * _command ) {
    if( _server == NULL ) {
        return ACKORD_INVALID;
    }

    if( _command == NULL ) {
        return ACKORD_INVALID;
    }

    if( _server->processing != 0 ) {
        return ACKORD_BUSY;
    }

    if( _server->faulted != 0 ) {
        return ACKORD_FAULTED;
    }

    if( _command->sequence == 0 ) {
        return ACKORD_INVALID;
    }

    if( _command->client == 0 ) {
        return ACKORD_INVALID;
    }

    if( _command->payload_size > _server->config.max_payload_size ) {
        return ACKORD_INVALID;
    }

    if( _command->payload_size != 0 && _command->payload == NULL ) {
        return ACKORD_INVALID;
    }

    if( _command->session != _server->session ) {
        return ACKORD_WRONG_SESSION;
    }

    if( _server->session == 0 ) {
        return ACKORD_WRONG_SESSION;
    }

    ackord_peer * peer = __ackord_find_peer( _server, _command->client );
    if( peer == NULL ) {
        return ACKORD_NOT_FOUND;
    }

    if( peer->state == ACKORD_PEER_RETIRED ) {
        return ACKORD_RETIRED;
    }

    if( peer->state == ACKORD_PEER_SUSPENDED ) {
        return ACKORD_SUSPENDED;
    }

    ackord_record * record = __ackord_find_record( _server, _command->client, _command->sequence );
    if( record != NULL ) {
        if( record->command.step != _command->step ) {
            return ACKORD_CONFLICT;
        }

        if( record->command.type != _command->type ) {
            return ACKORD_CONFLICT;
        }

        if( record->command.payload_size != _command->payload_size ) {
            return ACKORD_CONFLICT;
        }

        if( _command->payload_size != 0 && __ackord_equal( record->command.payload, _command->payload, _command->payload_size ) == 0 ) {
            return ACKORD_CONFLICT;
        }

        return ACKORD_DUPLICATE;
    }

    if( _command->sequence <= peer->confirmed ) {
        return ACKORD_STALE;
    }

    uint64_t distance = _command->sequence - peer->confirmed;
    if( distance > _server->config.sequence_window ) {
        return ACKORD_OUT_OF_WINDOW;
    }

    if( ( peer->completed & ( UINT64_C( 1 ) << ( distance - 1 ) ) ) != 0 ) {
        return ACKORD_STALE;
    }

    ackord_record * available = _server->freeRecords;
    if( available == NULL ) {
        return ACKORD_FULL;
    }

    _server->freeRecords = available->next;
    __ackord_copy( &available->command, _command, sizeof( available->command ) );
    if( _command->payload_size != 0 ) {
        __ackord_copy( available->storage, _command->payload, _command->payload_size );
    }

    available->command.payload = available->storage;
    available->state = 1;
    size_t bucket = __ackord_identity_bucket( _server, _command->client, _command->sequence );
    available->next = _server->identities[bucket];
    _server->identities[bucket] = available;
    return ACKORD_OK;
}

ackord_status ackord_server_lookup( const ackord_server * _server, uint64_t _session, uint32_t _client, uint64_t _sequence, ackord_command_view * _out ) {
    if( _out == NULL ) {
        return ACKORD_INVALID;
    }

    __ackord_zero( _out, sizeof( *_out ) );
    if( _server == NULL ) {
        return ACKORD_INVALID;
    }

    if( _session == 0 ) {
        return ACKORD_INVALID;
    }

    if( _client == 0 ) {
        return ACKORD_INVALID;
    }

    if( _sequence == 0 ) {
        return ACKORD_INVALID;
    }

    if( _session != _server->session ) {
        return ACKORD_WRONG_SESSION;
    }

    ackord_peer * peer = __ackord_find_peer( _server, _client );
    if( peer == NULL ) {
        return ACKORD_NOT_FOUND;
    }

    ackord_record * record = __ackord_find_record( _server, _client, _sequence );
    if( record != NULL ) {
        __ackord_copy( &_out->command, &record->command, sizeof( _out->command ) );
        _out->state = ACKORD_COMMAND_PENDING;
        if( record->state == 2 ) {
            _out->state = ACKORD_COMMAND_COMPLETED;
            __ackord_copy( &_out->confirmation, &record->confirmation, sizeof( _out->confirmation ) );
        }

        return ACKORD_OK;
    }

    if( _sequence <= peer->confirmed ) {
        return ACKORD_STALE;
    }

    uint64_t distance = _sequence - peer->confirmed;
    if( distance <= 64 ) {
        if( ( peer->completed & ( UINT64_C( 1 ) << ( distance - 1 ) ) ) != 0 ) {
            return ACKORD_STALE;
        }
    }

    return ACKORD_NOT_FOUND;
}

static inline int __ackord_compare_records( const ackord_record * _left, const ackord_record * _right ) {
    const ackord_command * leftCommand = &_left->command;
    const ackord_command * rightCommand = &_right->command;
    if( leftCommand->step != rightCommand->step ) {
        return leftCommand->step < rightCommand->step ? -1 : 1;
    }

    if( leftCommand->client != rightCommand->client ) {
        return leftCommand->client < rightCommand->client ? -1 : 1;
    }

    if( leftCommand->sequence != rightCommand->sequence ) {
        return leftCommand->sequence < rightCommand->sequence ? -1 : 1;
    }

    return 0;
}

static void __ackord_sift_records( ackord_record ** _records, size_t _root, size_t _count ) {
    ackord_record * record = _records[_root];

    while( _root < _count / 2 ) {
        size_t child = _root * 2 + 1;
        if( child + 1 < _count && __ackord_compare_records( _records[child], _records[child + 1] ) < 0 ) {
            ++child;
        }

        if( __ackord_compare_records( record, _records[child] ) >= 0 ) {
            break;
        }

        _records[_root] = _records[child];
        _root = child;
    }

    _records[_root] = record;
}

static size_t __ackord_select_by_step( ackord_server * _server, uint64_t _step ) {
    size_t count = 0;

    for( size_t index = 0; index < _server->config.max_records; ++index ) {
        ackord_record * record = &_server->records[index];
        if( record->state != 1 ) {
            continue;
        }

        if( record->command.step > _step ) {
            continue;
        }

        ackord_peer * peer = __ackord_find_peer( _server, record->command.client );
        if( peer->state != ACKORD_PEER_ACTIVE ) {
            continue;
        }

        _server->selected[count++] = record;
    }

    for( size_t index = count / 2; index != 0; --index ) {
        __ackord_sift_records( _server->selected, index - 1, count );
    }

    for( size_t remaining = count; remaining > 1; --remaining ) {
        ackord_record * record = _server->selected[0];
        _server->selected[0] = _server->selected[remaining - 1];
        _server->selected[remaining - 1] = record;
        __ackord_sift_records( _server->selected, 0, remaining - 1 );
    }

    return count;
}

static size_t __ackord_select_by_sequence( ackord_server * _server, uint64_t _step ) {
    size_t count = 0;

    for( size_t index = 0; index < _server->config.max_clients; ++index ) {
        ackord_peer * peer = &_server->peers[index];
        peer->next = NULL;
        if( peer->state != ACKORD_PEER_ACTIVE ) {
            continue;
        }

        if( peer->confirmed == UINT64_MAX ) {
            continue;
        }

        peer->next = __ackord_find_record( _server, peer->id, peer->confirmed + 1 );
    }

    for( ;; ) {
        ackord_peer * selected = NULL;
        for( size_t index = 0; index < _server->config.max_clients; ++index ) {
            ackord_peer * peer = &_server->peers[index];
            if( peer->next == NULL ) {
                continue;
            }

            if( peer->next->state != 1 ) {
                continue;
            }

            if( peer->next->command.step > _step ) {
                continue;
            }

            if( selected != NULL ) {
                if( __ackord_compare_records( peer->next, selected->next ) >= 0 ) {
                    continue;
                }
            }

            selected = peer;
        }

        if( selected == NULL ) {
            break;
        }

        ackord_record * record = selected->next;
        _server->selected[count++] = record;
        selected->next = NULL;
        if( record->command.sequence == UINT64_MAX ) {
            continue;
        }

        selected->next = __ackord_find_record( _server, selected->id, record->command.sequence + 1 );
    }

    return count;
}

static int __ackord_outcomes_valid( const ackord_server * _server, size_t _count ) {
    for( size_t index = 0; index < _count; ++index ) {
        const ackord_outcome * outcome = &_server->outcomes[index];
        const void * result = _server->selected[index]->storage + _server->config.max_payload_size;
        if( outcome->result != result ) {
            return 0;
        }

        if( outcome->result_capacity != _server->config.max_result_size ) {
            return 0;
        }

        if( outcome->result_size > _server->config.max_result_size ) {
            return 0;
        }

        if( outcome->status == INT32_MIN ) {
            return 0;
        }
    }

    return 1;
}

static ackord_status __ackord_process( ackord_server * _server, uint64_t _step, ackord_handler _handler, ackord_checked_handler _checked, void * _user ) {
    size_t count;

    ackord_status status = ACKORD_OK;
    if( _server == NULL ) {
        return ACKORD_INVALID;
    }

    if( _handler == NULL && _checked == NULL ) {
        return ACKORD_INVALID;
    }

    if( _server->session == 0 ) {
        return ACKORD_INVALID;
    }

    if( _server->processing != 0 ) {
        return ACKORD_BUSY;
    }

    if( _server->faulted != 0 ) {
        return ACKORD_FAULTED;
    }

    if( _server->ordering == ACKORD_ORDER_BY_SEQUENCE ) {
        count = __ackord_select_by_sequence( _server, _step );
    } else {
        count = __ackord_select_by_step( _server, _step );
    }

    if( count > UINT64_MAX - _server->journalIndex ) {
        return ACKORD_FULL;
    }

    for( size_t index = 0; index < count; ++index ) {
        __ackord_copy( &_server->batch[index], &_server->selected[index]->command, sizeof( _server->batch[index] ) );
        _server->outcomes[index].status = 0;
        _server->outcomes[index].result = _server->selected[index]->storage + _server->config.max_payload_size;
        _server->outcomes[index].result_capacity = _server->config.max_result_size;
        _server->outcomes[index].result_size = 0;
    }

    _server->processing = 1;
    if( _checked != NULL ) {
        status = _checked( _user, _server->batch, _server->outcomes, count );
    } else {
        _handler( _user, _server->batch, _server->outcomes, count );
    }

    if( status == ACKORD_OK ) {
        if( __ackord_outcomes_valid( _server, count ) == 0 ) {
            status = ACKORD_INVALID;
        }
    }

    if( status != ACKORD_OK ) {
        _server->processing = 0;
        _server->faulted = 1;
        return ACKORD_HANDLER_FAILED;
    }

    for( size_t index = 0; index < count; ++index ) {
        ackord_record * record = _server->selected[index];
        ackord_peer * peer = __ackord_find_peer( _server, record->command.client );
        ackord_confirmation * confirmation = &record->confirmation;
        confirmation->session = _server->session;
        confirmation->client = record->command.client;
        confirmation->sequence = record->command.sequence;
        confirmation->step = _step;
        confirmation->status = _server->outcomes[index].status;
        confirmation->result = record->storage + _server->config.max_payload_size;
        confirmation->result_size = _server->outcomes[index].result_size;
        record->state = 2;
        record->index = ++_server->journalIndex;
        size_t position = (size_t) ( ( record->index - 1 ) % _server->config.max_records );
        _server->journal[position] = record;
        ++_server->journalCount;
        peer->completed |= UINT64_C( 1 ) << ( record->command.sequence - peer->confirmed - 1 );
        while( ( peer->completed & UINT64_C( 1 ) ) != 0 ) {
            ++peer->confirmed;
            peer->completed >>= 1;
        }
    }

    _server->processing = 0;
    return ACKORD_OK;
}

ackord_status ackord_server_process( ackord_server * _server, uint64_t _step, ackord_handler _handler, void * _user ) {
    ackord_status status = __ackord_process( _server, _step, _handler, NULL, _user );
    return status;
}

ackord_status ackord_server_process_checked( ackord_server * _server, uint64_t _step, ackord_checked_handler _handler, void * _user ) {
    ackord_status status = __ackord_process( _server, _step, NULL, _handler, _user );
    return status;
}

ackord_status ackord_server_confirmed( const ackord_server * _server, uint32_t _client, uint64_t * _sequence ) {
    if( _server == NULL ) {
        return ACKORD_INVALID;
    }

    if( _client == 0 ) {
        return ACKORD_INVALID;
    }

    if( _sequence == NULL ) {
        return ACKORD_INVALID;
    }

    ackord_peer * peer = __ackord_find_peer( _server, _client );
    if( peer == NULL ) {
        return ACKORD_NOT_FOUND;
    }

    *_sequence = peer->confirmed;
    return ACKORD_OK;
}

ackord_status ackord_server_completed_mask( const ackord_server * _server, uint32_t _client, uint64_t * _mask ) {
    if( _server == NULL ) {
        return ACKORD_INVALID;
    }

    if( _client == 0 ) {
        return ACKORD_INVALID;
    }

    if( _mask == NULL ) {
        return ACKORD_INVALID;
    }

    ackord_peer * peer = __ackord_find_peer( _server, _client );
    if( peer == NULL ) {
        return ACKORD_NOT_FOUND;
    }

    *_mask = peer->completed;
    return ACKORD_OK;
}

ackord_status ackord_server_confirmation( const ackord_server * _server, uint32_t _client, uint64_t _sequence, ackord_confirmation * _out ) {
    if( _server == NULL ) {
        return ACKORD_INVALID;
    }

    if( _out == NULL ) {
        return ACKORD_INVALID;
    }

    const ackord_record * record = __ackord_find_record( _server, _client, _sequence );
    if( record == NULL ) {
        return ACKORD_NOT_FOUND;
    }

    if( record->state != 2 ) {
        return ACKORD_NOT_FOUND;
    }

    __ackord_copy( _out, &record->confirmation, sizeof( *_out ) );
    return ACKORD_OK;
}

ackord_status ackord_server_journal_next( const ackord_server * _server, uint64_t _afterIndex, ackord_journal_entry * _out ) {
    if( _server == NULL ) {
        return ACKORD_INVALID;
    }

    if( _out == NULL ) {
        return ACKORD_INVALID;
    }

    if( _server->journalCount == 0 ) {
        return ACKORD_NOT_FOUND;
    }

    if( _afterIndex >= _server->journalIndex ) {
        return ACKORD_NOT_FOUND;
    }

    uint64_t firstIndex = _server->journalIndex - _server->journalCount + 1;
    uint64_t index = _afterIndex + 1;
    if( index < firstIndex ) {
        index = firstIndex;
    }

    size_t position = (size_t) ( ( index - 1 ) % _server->config.max_records );
    const ackord_record * next = _server->journal[position];
    _out->index = next->index;
    __ackord_copy( &_out->command, &next->command, sizeof( _out->command ) );
    __ackord_copy( &_out->confirmation, &next->confirmation, sizeof( _out->confirmation ) );
    return ACKORD_OK;
}

ackord_status ackord_server_journal_trim( ackord_server * _server, uint64_t _throughIndex ) {
    if( _server == NULL ) {
        return ACKORD_INVALID;
    }

    if( _server->processing != 0 ) {
        return ACKORD_BUSY;
    }

    if( _server->faulted != 0 ) {
        return ACKORD_FAULTED;
    }

    while( _server->journalCount != 0 ) {
        uint64_t index = _server->journalIndex - _server->journalCount + 1;
        if( index > _throughIndex ) {
            break;
        }

        size_t position = (size_t) ( ( index - 1 ) % _server->config.max_records );
        __ackord_release_record( _server, _server->journal[position] );
        _server->journal[position] = NULL;
        --_server->journalCount;
    }

    return ACKORD_OK;
}

ackord_status ackord_client_create( const ackord_config * _config, ackord_client ** _out ) {
    if( _out == NULL ) {
        return ACKORD_INVALID;
    }

    *_out = NULL;
    if( __ackord_config_valid( _config ) == 0 ) {
        return ACKORD_INVALID;
    }

    ackord_client * client = __ackord_array( _config, 1, sizeof( *client ) );
    if( client == NULL ) {
        return ACKORD_NO_MEMORY;
    }

    __ackord_copy( &client->config, _config, sizeof( client->config ) );
    client->records = __ackord_array( _config, _config->max_records, sizeof( *client->records ) );
    if( client->records == NULL ) {
        ackord_client_destroy( client );
        return ACKORD_NO_MEMORY;
    }

    if( _config->max_payload_size == 0 ) {
        *_out = client;
        return ACKORD_OK;
    }

    client->storage = __ackord_array( _config, _config->max_records, _config->max_payload_size );
    if( client->storage == NULL ) {
        ackord_client_destroy( client );
        return ACKORD_NO_MEMORY;
    }

    for( size_t index = 0; index < _config->max_records; ++index ) {
        client->records[index].storage = client->storage + index * _config->max_payload_size;
    }

    *_out = client;
    return ACKORD_OK;
}

void ackord_client_destroy( ackord_client * _client ) {
    if( _client == NULL ) {
        return;
    }

    ackord_config config;
    __ackord_copy( &config, &_client->config, sizeof( config ) );
    __ackord_free( &config, _client->records );
    __ackord_free( &config, _client->storage );
    __ackord_free( &config, _client );
}

ackord_status ackord_client_clear( ackord_client * _client ) {
    if( _client == NULL ) {
        return ACKORD_INVALID;
    }

    _client->session = 0;
    _client->id = 0;
    _client->nextSequence = 0;
    _client->count = 0;
    for( size_t index = 0; index < _client->config.max_records; ++index ) {
        _client->records[index].state = 0;
    }

    return ACKORD_OK;
}

ackord_status ackord_client_reset( ackord_client * _client, uint64_t _session, uint32_t _id ) {
    if( _client == NULL ) {
        return ACKORD_INVALID;
    }

    if( _session == 0 ) {
        return ACKORD_INVALID;
    }

    if( _id == 0 ) {
        return ACKORD_INVALID;
    }

    if( _client->session == _session && _client->id == _id ) {
        return ACKORD_DUPLICATE;
    }

    ackord_client_clear( _client );
    _client->session = _session;
    _client->id = _id;
    _client->nextSequence = 1;
    return ACKORD_OK;
}

ackord_status ackord_client_submit( ackord_client * _client, uint64_t _step, uint32_t _type, const void * _payload, uint32_t _payloadSize, ackord_command * _out ) {
    if( _client == NULL ) {
        return ACKORD_INVALID;
    }

    if( _out == NULL ) {
        return ACKORD_INVALID;
    }

    if( _client->session == 0 ) {
        return ACKORD_INVALID;
    }

    if( _payloadSize > _client->config.max_payload_size ) {
        return ACKORD_INVALID;
    }

    if( _payloadSize != 0 && _payload == NULL ) {
        return ACKORD_INVALID;
    }

    if( _client->nextSequence == UINT64_MAX ) {
        return ACKORD_FULL;
    }

    for( size_t index = 0; index < _client->config.max_records; ++index ) {
        ackord_record * record = &_client->records[index];
        if( record->state != 0 ) {
            continue;
        }

        record->command.session = _client->session;
        record->command.client = _client->id;
        record->command.sequence = _client->nextSequence++;
        record->command.step = _step;
        record->command.type = _type;
        record->command.payload = record->storage;
        record->command.payload_size = _payloadSize;
        if( _payloadSize != 0 ) {
            __ackord_copy( record->storage, _payload, _payloadSize );
        }

        record->state = 1;
        ++_client->count;
        __ackord_copy( _out, &record->command, sizeof( *_out ) );
        return ACKORD_OK;
    }

    return ACKORD_FULL;
}

ackord_status ackord_client_pending_next( const ackord_client * _client, uint64_t _afterSequence, ackord_command * _out ) {
    const ackord_record * next = NULL;

    if( _client == NULL ) {
        return ACKORD_INVALID;
    }

    if( _out == NULL ) {
        return ACKORD_INVALID;
    }

    for( size_t index = 0; index < _client->config.max_records; ++index ) {
        const ackord_record * record = &_client->records[index];
        if( record->state == 0 ) {
            continue;
        }

        if( record->command.sequence <= _afterSequence ) {
            continue;
        }

        if( next == NULL || record->command.sequence < next->command.sequence ) {
            next = record;
        }
    }

    if( next == NULL ) {
        return ACKORD_NOT_FOUND;
    }

    __ackord_copy( _out, &next->command, sizeof( *_out ) );
    return ACKORD_OK;
}

size_t ackord_client_pending_count( const ackord_client * _client ) {
    if( _client == NULL ) {
        return 0;
    }

    return _client->count;
}

ackord_status ackord_client_confirm( ackord_client * _client, const ackord_confirmation * _confirmation ) {
    if( _client == NULL ) {
        return ACKORD_INVALID;
    }

    if( _confirmation == NULL ) {
        return ACKORD_INVALID;
    }

    if( _confirmation->session != _client->session ) {
        return ACKORD_WRONG_SESSION;
    }

    if( _confirmation->client != _client->id ) {
        return ACKORD_WRONG_SESSION;
    }

    if( _confirmation->sequence == 0 ) {
        return ACKORD_INVALID;
    }

    if( _confirmation->sequence >= _client->nextSequence ) {
        return ACKORD_INVALID;
    }

    for( size_t index = 0; index < _client->config.max_records; ++index ) {
        ackord_record * record = &_client->records[index];
        if( record->state == 0 ) {
            continue;
        }

        if( record->command.sequence != _confirmation->sequence ) {
            continue;
        }

        record->state = 0;
        --_client->count;
        return ACKORD_OK;
    }

    return ACKORD_DUPLICATE;
}

ackord_status ackord_client_validate_progress( const ackord_client * _client, uint64_t _session, uint32_t _id, uint64_t _sequence, uint64_t _completedMask ) {
    uint32_t bit;
    if( _client == NULL ) {
        return ACKORD_INVALID;
    }

    if( _session != _client->session ) {
        return ACKORD_WRONG_SESSION;
    }

    if( _id != _client->id ) {
        return ACKORD_WRONG_SESSION;
    }

    if( _sequence >= _client->nextSequence ) {
        return ACKORD_INVALID;
    }

    for( bit = 0; bit < 64; ++bit ) {
        if( ( _completedMask & ( UINT64_C( 1 ) << bit ) ) == 0 ) {
            continue;
        }

        if( _sequence > UINT64_MAX - bit - 1 ) {
            return ACKORD_INVALID;
        }

        if( _sequence + bit + 1 >= _client->nextSequence ) {
            return ACKORD_INVALID;
        }
    }

    return ACKORD_OK;
}

ackord_status ackord_client_confirm_progress( ackord_client * _client, uint64_t _session, uint32_t _id, uint64_t _sequence, uint64_t _completedMask ) {
    ackord_status status = ackord_client_validate_progress( _client, _session, _id, _sequence, _completedMask );
    if( status != ACKORD_OK ) {
        return status;
    }

    for( size_t index = 0; index < _client->config.max_records; ++index ) {
        ackord_record * record = &_client->records[index];
        uint64_t distance;
        if( record->state == 0 ) {
            continue;
        }

        if( record->command.sequence > _sequence ) {
            distance = record->command.sequence - _sequence;
            if( distance > 64 ) {
                continue;
            }

            if( ( _completedMask & ( UINT64_C( 1 ) << ( distance - 1 ) ) ) == 0 ) {
                continue;
            }
        }

        record->state = 0;
        --_client->count;
    }

    return ACKORD_OK;
}

ackord_status ackord_client_confirm_through( ackord_client * _client, uint64_t _session, uint32_t _id, uint64_t _sequence ) {
    ackord_status status = ackord_client_confirm_progress( _client, _session, _id, _sequence, 0 );

    return status;
}
