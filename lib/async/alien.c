/*
 * Copyright (C) 2024 Isaac de Wolff <idewolff@vincitech.nl>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
This file is part of the async plugin for ucode
*/
#include <linux/futex.h>
#include <stdatomic.h>
#include <sys/syscall.h>
#include <err.h>
#include <errno.h>

#include "ucode/lib.h"
#include "ucode/vm.h"
#include "ucode/types.h"
#include "ucode/async.h"

#include "manager.h"
#include "callback.h"
#include "alien.h"

#ifdef ASYNC_HAS_ALIENS

static int
async_futex(uint32_t *uaddr, int futex_op, uint32_t val,
    const struct timespec *timeout, uint32_t *uaddr2, uint32_t val3)
{
    return syscall(SYS_futex, uaddr, futex_op, val,
                          timeout, uaddr2, val3);
}


void
async_alien_enter( async_manager_t *manager )
{
    uint32_t *futex_addr = &manager->alien->the_futex;

    while (1) 
    {
        const uint32_t zero = 0;
        if( atomic_compare_exchange_strong( futex_addr, &zero, 1 ) )
            return;

        int futex_rc = async_futex(futex_addr, FUTEX_WAIT, 0, NULL, NULL, 0);
        if (futex_rc == -1) 
        {
            if (errno != EAGAIN) 
            {
                perror("futex wait");
                exit(1);
            }
        } 
    }
}

void
async_alien_release( async_manager_t *manager )
{
    uint32_t *futex_addr = &manager->alien->the_futex;

    const uint32_t zero = 0;
    atomic_store( futex_addr, zero );
    if( -1 == async_futex(futex_addr, FUTEX_WAKE, 0, NULL, NULL, 0) )
    {
        perror( "futex wake" );
        exit( 1 );
    }
}

struct async_alien_impl
{
    uc_async_alient_t header;
    const uc_async_callback_queuer_t *queuer;
    async_manager_t *manager;
};

static void 
_uc_async_alien_free( struct uc_async_alien const ** alien )
{
    if( 0 == alien || 0 == *alien )
        return;
    struct async_alien_impl const *myalien = (struct async_alien_impl const *)*alien;
    const uc_async_callback_queuer_t *queuer = myalien->queuer;
    async_manager_t *manager = myalien->manager;

    free( (void *)*alien );
    *alien = 0;

    if( manager )
    {
        if( 0 == __atomic_add_fetch( &manager->alien->num_aliens, -1, __ATOMIC_RELAXED ) )
        {
            async_wakeup( queuer );
        }
    }

    if( queuer )
    {
        uc_async_callback_queuer_free( &queuer );
    }
}

static int 
_uc_async_alien_call( const struct uc_async_alien *alien, int (*func)( uc_vm_t *, void *, int flags ), void *user )
{
    if( !alien )
        return EXCEPTION_RUNTIME;
    struct async_alien_impl *myalien = (struct async_alien_impl *)alien;
    async_manager_t *manager = myalien->manager;
    if( 0 == manager )
        return EXCEPTION_RUNTIME;

    ASYNC_ALIENT_ENTER( manager );
    int todo_seq_before = manager->alien->todo_seq;
    struct uc_threadlocal *push = uc_threadlocal_data;
    uc_threadlocal_data = manager->alien->threadlocal;

    int ret = (func)( manager->vm, user, UC_ASYNC_CALLBACK_FLAG_EXECUTE|UC_ASYNC_CALLBACK_FLAG_CLEANUP );

    uc_threadlocal_data = push; 
    if( todo_seq_before != manager->alien->todo_seq )
    {
        // Something is added to the todolist. 
        // Wakeup script thread to let it reconsider it's sleep duration
        async_wakeup( myalien->queuer );
    }
    ASYNC_ALIENT_LEAVE( manager );
    return ret;
}

static const uc_async_alient_t *
_uc_async_new_alien( struct uc_async_manager *_manager )
{
    async_manager_t *manager = async_manager_cast( _manager );
    __atomic_add_fetch( &manager->alien->num_aliens, 1, __ATOMIC_RELAXED );
    struct async_alien_impl *ret = xalloc( sizeof( struct async_alien_impl ) );
    ret->header.free = _uc_async_alien_free;
    ret->header.call = _uc_async_alien_call;
    ret->queuer = manager->header.new_callback_queuer( &manager->header );
    ret->manager = manager;
    return &ret->header;
}

void 
async_alien_free( async_manager_t *manager, struct async_alien *alien )
{
    free( alien );
}


#else  // defined ASYNC_HAS_ALIENS

static const uc_async_alient_t *
_uc_async_new_alien( struct uc_async_manager * )
{
    return 0;
}

#endif // nded ASYNC_HAS_ALIENS

void 
async_alien_init( async_manager_t *manager, uc_value_t *scope )
{
    manager->header.new_alien = _uc_async_new_alien;

#ifdef ASYNC_HAS_ALIENS
    manager->alien = xalloc( sizeof( struct async_alien ) );
    manager->alien->threadlocal = uc_threadlocal_data;
    manager->alien->the_futex = 1;
#endif
}

