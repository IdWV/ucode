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
#ifndef UC_ASYNC_ALIEN_H
#define UC_ASYNC_ALIEN_H

#include "manager.h"

#ifdef ASYNC_HAS_ALIENS
extern __hidden void
async_alien_enter( async_manager_t *manager );

extern __hidden void
async_alien_release( async_manager_t *manager );

struct async_alien
{
    // Counter which is incremented at each todo added,
    // to know if an alien call should interrupt the sleep
    int todo_seq;
	
	uint32_t the_futex;
    uint32_t num_aliens;

	struct uc_threadlocal *threadlocal;
};

extern __hidden void 
async_alien_free( async_manager_t *, struct async_alien * );

#   define ASYNC_ALIENT_ENTER(manager) async_alien_enter( manager );
#   define ASYNC_ALIENT_LEAVE(manager) async_alien_release( manager );
#else // ASYNC_HAS_ALIENS

#   define ASYNC_ALIENT_ENTER(...)
#   define ASYNC_ALIENT_LEAVE(...)
#endif // 

extern __hidden void 
async_alien_init( async_manager_t *manager, uc_value_t *scope );

#endif //ndef UC_ASYNC_ALIEN_H

