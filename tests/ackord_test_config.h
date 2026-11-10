// clang-format Language: C
#ifndef ACKORD_TEST_CONFIG_H
#define ACKORD_TEST_CONFIG_H

#include "ackord/ackord.h"
#include <stdlib.h>

static void * __ackord_test_allocate( void * _user, size_t _size ) {
    (void) _user;
    void * memory = malloc( _size );

    return memory;
}

static void __ackord_test_deallocate( void * _user, void * _memory ) {
    (void) _user;
    free( _memory );
}

static ackord_config __ackord_test_config( void ) {
    ackord_config config = { 16, 1024, 256, 256, 64, { __ackord_test_allocate, __ackord_test_deallocate, NULL } };
    return config;
}
#endif
