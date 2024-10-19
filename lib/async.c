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

/**
 * # Async functions
 *
 * The `async` module provides asynchronous functionality.
 *
 * Functions can be individually imported and directly accessed using the
 * {@link https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Statements/import#named_import named import}
 * syntax:
 *
 *   ```
 *   import { Promise, setTimeout } from 'async';
 *
 *   Promise( (resolver)=>
 *   {
 *	   setTimeout( ()=>
 *	   {
 *		   resolver.resolve( 'done' );
 *	   }, 1000 )
 *   }).then( ( a )=>
 *   {
 *	   print( a );
 *   });
 *   ```
 *
 * Alternatively, the module namespace can be imported
 * using a wildcard import statement:
 *
 *   ```
 *   import * as async from 'async';
 *
 *   async.Promise( (resolver)=>
 *   {
 *	   async.setTimeout( ()=>
 *	   {
 *		   resolver.resolve( 'done' );
 *	   }, 1000 )
 *   }).then( ( a )=>
 *   {
 *	   print( a );
 *   }); 
 *   ```
 *
 * @module async
 */

#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <assert.h>
#include <math.h>

#include "ucode/module.h"
#include "ucode/platform.h"
#include "ucode/async.h"

//#define DEBUG_PRINT
#define HAS_UPTIME
#ifdef DEBUG_PRINT
#   define DEBUG_PRINTF(...) printf(__VA_ARGS__)
#   define HAS_UPTIME
#else
#   define DEBUG_PRINTF(...)
#endif

#ifdef NDEBUG
#   define DEBUG_ASSERT(...) 
#else
#   define DEBUG_ASSERT(...)	assert(__VA_ARGS__)
#endif

struct async_todo;
struct async_promise;
struct async_callback_link;

struct async_manager
{
	struct uc_async_manager header;

	uc_vm_t *vm;

	// Linked list of pending todo's
	struct async_todo *todo_list;
	// Points to the active promise which excecuting a then, catch or finally handler
	// to be able to catch the arguments of 'throw()'.
	struct async_promise *active_promise;

	// Number of pending promises
	int pending_promises_cnt:31;
    int silent:1; // exit is called, no more output

	// Pointer to linked list of async callback's
	struct uc_async_callback_queuer *callback_queuer;


#ifdef HAS_UPTIME
	// For uptime
	int64_t start_time;
#endif

	uc_resource_type_t *promise_type;
	uc_resource_type_t *resolver_type;
	uc_resource_type_t *timer_type;
};

typedef struct async_manager async_manager_t;

static inline async_manager_t *
async_manager_cast( struct uc_async_manager *m )
{
	return (async_manager_t *)m;
}

static inline async_manager_t *
async_manager_get( uc_vm_t *vm )
{
	if( !vm )   return 0;
	struct uc_async_manager *manager = uc_async_manager_get( vm );
	if( !manager )
		return 0;
	return async_manager_cast( manager );
}

static int64_t
async_timer_current_time();

#ifdef HAS_UPTIME
static double
uptime( async_manager_t *manager )
{
	if (!manager)
		return NAN;
	int64_t now = async_timer_current_time() - manager->start_time;
	return (double)now / 1000.0;
}
#endif


/*******
 * The part of the code which is responsible for multithreaded asynchronity
 **/
#define SIGNEWCALLBACK SIGUSR1 // Signal used for async callbacks

/* There is max one instance of this struct per vm.
which is created when the first 'uc_async_callback_queuer' is created */
struct async_callback_unique_in_vm
{
	// linked list of callbacks to be executed
	struct async_callback_link *stack;
	int refcount;

	// Thread of the script
	pthread_t thread;
	// VM in which we live.
	async_manager_t *manager;
};

static uc_value_t *
async_callback_signal_handler(uc_vm_t *vm, size_t nargs)
{
	// Do nothing. We only want to interrupt the async_sleep function
#ifdef DEBUG_PRINT
	async_manager_t *manager = async_manager_get( vm );
	DEBUG_PRINTF( "%-1.3lf Signal handler\n", manager ? uptime(manager) : NAN );
#endif
	return 0;
}

static struct async_callback_unique_in_vm *
async_unique_in_vm_new( async_manager_t *manager )
{
	struct async_callback_unique_in_vm *unique = xalloc(sizeof(struct async_callback_unique_in_vm));
	unique->refcount = 1;

	// Setup signal handler
	uc_cfn_ptr_t ucsignal = uc_stdlib_function("signal");
	uc_value_t *func = ucv_cfunction_new("async", async_callback_signal_handler);

	uc_vm_stack_push( manager->vm, ucv_uint64_new( SIGNEWCALLBACK ));
	uc_vm_stack_push( manager->vm, func);

	if (ucsignal(manager->vm, 2) != func)
		fprintf(stderr, "Unable to install async_callback_signal_handler\n");

	ucv_put(uc_vm_stack_pop( manager->vm));
	ucv_put(uc_vm_stack_pop( manager->vm));
	ucv_put( func );

	// Remember the thread ID
	unique->thread = pthread_self();
	// And the vm
	unique->manager = manager;
	return unique;
}

static async_manager_t *
async_unique_is_synchron(struct async_callback_unique_in_vm *unique)
{
	if (unique->thread == pthread_self())
		return unique->manager;
	return 0;
}

/* Wakeup the sleeping script engine */
static void
async_unique_wakeup(struct async_callback_unique_in_vm *unique)
{
	if (async_unique_is_synchron( unique ) )
		// running in the script thread
		return;

	DEBUG_PRINTF( "%-1.3lf Wakeup script\n", uptime( unique->manager ) );
	// send a signal to the script thread;
	union sigval info = {0};
	info.sival_ptr = (void *)unique->thread;

	pthread_sigqueue(unique->thread, SIGNEWCALLBACK, info);
}

/* Start an interruptable sleep */
static void async_unique_sleep(struct async_callback_unique_in_vm *unique, int64_t msec)
{
	if (msec < 1)
		return;

	struct timespec wait;
	wait.tv_sec = msec / 1000;
	wait.tv_nsec = (msec % 1000) * 1000000;
	nanosleep(&wait, 0);
}

static int
async_unique_in_vm_link(struct async_callback_unique_in_vm *unique)
{
	return __atomic_add_fetch(&unique->refcount, 1, __ATOMIC_RELAXED);
}

static int
async_unique_in_vm_unlink(struct async_callback_unique_in_vm *unique)
{
	int refcount = __atomic_add_fetch(&unique->refcount, -1, __ATOMIC_RELAXED);
	if (refcount)
		return refcount;

	// TODO: Shouldn't we release the signal handler?

	free(unique);
	return refcount;
}

static struct async_callback_link *
uc_unique_lock_stack(struct async_callback_unique_in_vm *unique)
{
	struct async_callback_link **pstack = &unique->stack;
	/*
	The stack is locked as the least significant bit is 1.
	So we try to set it, which only succeeds if it is not set now.
	*/
	while (true)
	{
		struct async_callback_link *oldstack = *pstack;
		oldstack = (void *)(((intptr_t)oldstack) & ~(intptr_t)1);
		struct async_callback_link *newstack = oldstack;
		newstack = (void *)(((intptr_t)newstack) | (intptr_t)1);
		if (__atomic_compare_exchange_n(pstack, &oldstack, newstack, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
		{
			return oldstack;
		}
	}
}

static void
uc_unique_unlock_stack(struct async_callback_unique_in_vm *unique, struct async_callback_link *func)
{
	/*
	Unlock the stack by writing a 'clean' pointer, without bit 0 set.
	*/
	__atomic_store_n(&unique->stack, func, __ATOMIC_RELAXED);
}

/*******
 * End of multithreaded functions
 */



typedef enum
{
	callbackNone = 0,
	callbackUcode,
	callbackC_int_user_flags,
	callbackC_int_user_args_flags
} async_callback_type_t;

struct async_callback
{
	uint32_t callback_type : 4;
	uint32_t type : 4;
	uint32_t nargs : 8;
	uint32_t still_available_bits : 16;

	union
	{
		struct
		{
			uc_value_t *func;
			uc_value_t *this; // not used, so far
		} ucode;
		struct
		{
			int (*func)(uc_vm_t *, void *user, int flags);
			void *user;
		} c_int_user_flags;
		struct
		{
			int (*func)(uc_vm_t *, void *user, uc_value_t **args, size_t nargs, int flags);
			void *user;
		} c_int_user_args_flags;
	};

	uc_value_t *args[];
};

static void
async_callback_destroy( uc_vm_t *vm, struct async_callback *pcb)
{
	switch ((async_callback_type_t)pcb->callback_type)
	{
	case callbackNone:
		break;
	case callbackUcode:
		if (pcb->ucode.func)
			ucv_put(pcb->ucode.func);
		pcb->ucode.func = 0;
		if (pcb->ucode.this)
			ucv_put(pcb->ucode.this);
		pcb->ucode.this = 0;
		break;
	case callbackC_int_user_flags:
		if (pcb->c_int_user_flags.func)
		{
			(*pcb->c_int_user_flags.func)( vm, pcb->c_int_user_flags.user, UC_ASYNC_CALLBACK_FLAG_CLEANUP);
			pcb->c_int_user_flags.func = 0;
			pcb->c_int_user_flags.user = 0;
		}
		break;
	case callbackC_int_user_args_flags:
		if (pcb->c_int_user_args_flags.func)
		{
			(*pcb->c_int_user_args_flags.func)( vm, pcb->c_int_user_args_flags.user, 0, 0, UC_ASYNC_CALLBACK_FLAG_CLEANUP);
			pcb->c_int_user_args_flags.func = 0;
			pcb->c_int_user_args_flags.user = 0;
		}
		break;
	}
	for (size_t n = 0; n < pcb->nargs; n++)
	{
		ucv_put(pcb->args[n]);
		pcb->args[n] = 0;
	}
	pcb->nargs = 0;
}

static uc_value_t *
async_callback_get_ucode_func( async_manager_t *manager, struct async_callback *cb )
{
	switch ((async_callback_type_t)cb->callback_type)
	{
	case callbackNone:
		return 0;
	case callbackUcode:
		return ucv_get(cb->ucode.func);
	case callbackC_int_user_flags:
		return 0;
	case callbackC_int_user_args_flags:
		return 0;
	}
	return 0;
}

static int
async_callback_call( async_manager_t *manager, struct async_callback *cb, uc_value_t **args, size_t nargs, uc_value_t **ret, bool cleanup)
{
	int flags = UC_ASYNC_CALLBACK_FLAG_EXECUTE | (cleanup ? UC_ASYNC_CALLBACK_FLAG_CLEANUP : 0);
	switch ((async_callback_type_t)cb->callback_type)
	{
	case callbackNone:
		return EXCEPTION_NONE;
	case callbackUcode:
	{
		uc_vm_t *vm = manager->vm;
		uc_vm_stack_push(vm, ucv_get(cb->ucode.func));
		for (size_t n = 0; n < nargs; n++)
			uc_vm_stack_push(vm, ucv_get(args[n]));
		for (size_t n = 0; n < cb->nargs; n++)
			uc_vm_stack_push(vm, ucv_get(cb->args[n]));
		int ex = uc_vm_call(vm, false, cb->nargs + nargs);
		if (cleanup)
		{
			ucv_put(cb->ucode.func);
			cb->ucode.func = 0;
			ucv_put(cb->ucode.this);
			cb->ucode.this = 0;
			for (size_t n = 0; n < cb->nargs; n++)
			{
				ucv_put(cb->args[n]);
				cb->args[n] = 0;
			}
			cb->nargs = 0;
		}
		if (ex != EXCEPTION_NONE)
			return ex;
		uc_value_t *pret = uc_vm_stack_pop(vm);
		if (ret)
			*ret = pret;
		else
			ucv_put(pret);
		return ex;
	}
	case callbackC_int_user_flags:
	{
		if (!cb->c_int_user_flags.func)
			return EXCEPTION_NONE;
		int ex = (*cb->c_int_user_flags.func)(manager->vm, cb->c_int_user_flags.user, flags);
		if (cleanup)
		{
			cb->c_int_user_flags.func = 0;
			cb->c_int_user_flags.user = 0;
		}
		return ex;
	}
	case callbackC_int_user_args_flags:
	{
		if (!cb->c_int_user_flags.func)
			return EXCEPTION_NONE;
		uc_value_t *args2[nargs + cb->nargs];
		size_t m = 0;
		for (size_t n = 0; n < nargs; n++)
			args2[m++] = args[n];
		for (size_t n = 0; n < cb->nargs; n++)
			args2[m++] = cb->args[n];
		int ex = (*cb->c_int_user_args_flags.func)(manager->vm, cb->c_int_user_args_flags.user, args2, m, flags);
		if (cleanup)
		{
			cb->c_int_user_args_flags.func = 0;
			cb->c_int_user_args_flags.user = 0;
		}
		return ex;
	}
	}
	return EXCEPTION_NONE;
}

enum async_todo_type
{
	todoPromise = 1,
	todoTimer = 2,
	todoClearedTimer = todoTimer | 1,
};

struct async_todo
{
	struct uc_async_timer header; // empty struct

	uint32_t todo_type : 2;

	/* refcount can be max 3.
	For promises: 1 for the ucode promise object,
			1 for the associated resolver object,
			and 1 for being in the todo list.
	For timers: 1 for the ucode timer object
			and 1 for being in the todo list.
	So 3 bits is plenty */
	uint32_t refcount : 3;
	/* One bit to know if this object is in the todo list */
	uint32_t in_todo_list : 1;

	/* which leaves 26 bits for general purpose: */
	uint32_t promise_pending : 1; // is added to 'global' vm->pending_promises_cnt
	uint32_t promise_state : 2;   // pending, resolved, rejected
	uint32_t promise_result_is_exception : 1;
	/* still 22 bits left */

	struct async_todo *next;
};

typedef struct async_todo async_todo_t;

/**
 * Represents a timer object as returned by
 * {@link module:async#setTimeout|setTimeout()}, {@link module:async#setPeriodic|setPeriodic()} or {@link module:async#setImmediate|setImmediate()}.
 *
 * This class has no methods. The only sane usage is to pass it to {@link module:async#clearTimeout|clearTimeout()}
 * 
 * @class module:async.timer
 * @hideconstructor
 *
 */

struct async_timer
{
	async_todo_t header;
	int64_t due;
	uint32_t periodic;
	struct async_callback callback;
};

typedef struct async_timer async_timer_t;

// Safety. Let the compiler error out when the wrong type is casted.
static inline async_timer_t *
async_timer_cast(async_todo_t *p)
{
	DEBUG_ASSERT(p->todo_type & todoTimer);
	return (async_timer_t *)p;
}

static async_timer_t *
uc_timer_ucode_new( async_manager_t *manager, size_t nargs, uc_value_t *cb, size_t startarg)
{
	size_t n_args = nargs - startarg;
	async_timer_t *timer = xalloc(sizeof(async_timer_t) + n_args * sizeof(uc_value_t *));
	timer->header.refcount = 1;
	timer->header.todo_type = todoTimer;
	timer->callback.callback_type = callbackUcode;
	timer->callback.nargs = n_args;
	timer->callback.ucode.func = ucv_get(cb);
	uc_vm_t *vm = manager->vm;

	for (size_t n1 = startarg, n2 = 0; n1 < nargs; n1++, n2++)
		timer->callback.args[n2] = ucv_get(uc_fn_arg(n1));

	return timer;
}

static async_timer_t *
async_timer_c_int_user_flags_new( async_manager_t *manager, int (*func)(uc_vm_t *, void *, int), void *user)
{
	async_timer_t *timer = xalloc(sizeof(async_timer_t));
	timer->header.refcount = 1;
	timer->header.todo_type = todoTimer;
	timer->callback.callback_type = callbackC_int_user_flags;
	timer->callback.c_int_user_flags.func = func;
	timer->callback.c_int_user_flags.user = user;
	return timer;
}

static int
async_promise_destroy( async_manager_t *, async_todo_t *);

static int
async_todo_unlink( async_manager_t *manager, async_todo_t *todo)
{
	if (!todo)
		return EXCEPTION_NONE;

	if (0 != --todo->refcount)
		return EXCEPTION_NONE;

	DEBUG_ASSERT( 0 == todo->in_todo_list );

	int ret = EXCEPTION_NONE;
	switch ((enum async_todo_type)todo->todo_type)
	{
	case todoClearedTimer:
	case todoTimer:
		async_callback_destroy( manager ? manager->vm : 0, &async_timer_cast(todo)->callback);
		break;
	case todoPromise:
		ret = async_promise_destroy( manager, todo);
		break;
	}

	free(todo);
	return ret;
}

static int64_t
async_timer_current_time()
{
	struct timespec monotime;
	clock_gettime(CLOCK_MONOTONIC, &monotime);
	return ((int64_t)monotime.tv_sec) * 1000 + (monotime.tv_nsec / 1000000);
}

// When is the todo object due?
static inline int64_t
async_todo_due(async_todo_t *todo)
{
	switch ((enum async_todo_type)todo->todo_type)
	{
	case todoClearedTimer:
	case todoPromise:
		return 0;
	case todoTimer:
		return async_timer_cast(todo)->due;
	}
	return 0;
}

static void
async_todo_put_in_list( async_manager_t *manager, async_todo_t *todo)
{
	DEBUG_ASSERT( 0 == todo->in_todo_list );
	todo->in_todo_list = 1;

	int64_t due = async_todo_due(todo);
	if( due )
	{
		DEBUG_PRINTF( "%-1.3lf Todo %p scheduled at %-1.3lf\n", uptime(manager), todo, 
			(due - manager->start_time) / 1000.0 );

	}
	async_todo_t *previous = 0;
	for (async_todo_t *it = manager->todo_list; it; it = it->next)
	{
		if (async_todo_due(it) > due)
		{
			todo->next = it;
			if (previous)
				previous->next = todo;
			else
				manager->todo_list = todo;
			todo->refcount++;
			return;
		}
		previous = it;
	}
	todo->next = 0;
	if (previous)
		previous->next = todo;
	else
		manager->todo_list = todo;
	todo->refcount++;
}

#define reverse_stack(type, stack)		 \
	do									 \
	{									  \
		type *walk = stack, *reversed = 0; \
		while (walk)					   \
		{								  \
			type *pop = walk;			  \
			walk = pop->next;			  \
			pop->next = reversed;		  \
			reversed = pop;				\
		}								  \
		stack = reversed;				  \
	} while (0)

static int
async_promise_do( async_manager_t *, async_todo_t *);

static int
async_handle_todo( async_manager_t *manager )
{
	if (!manager->todo_list)
		return EXCEPTION_NONE;
	int64_t now = async_timer_current_time();
	async_todo_t *to_be_handled = 0;

	while (manager->todo_list)
	{
		bool end_of_todo_for_now = false;
		switch ((enum async_todo_type)manager->todo_list->todo_type)
		{
		case todoClearedTimer:
			break;
		case todoTimer:
			end_of_todo_for_now = async_timer_cast(manager->todo_list)->due > now;
			break;
		case todoPromise:
			break;
		}
		if (end_of_todo_for_now)
			break;

		async_todo_t *pop = manager->todo_list;
		manager->todo_list = pop->next;
		pop->in_todo_list = 0;
		pop->next = 0;

		if (todoClearedTimer == pop->todo_type)
		{
			async_todo_unlink(manager, pop);
		}
		else
		{
			pop->next = to_be_handled;
			to_be_handled = pop;
		}
	}

	if (!to_be_handled)
		return EXCEPTION_NONE;

	reverse_stack(async_todo_t, to_be_handled);

	while (to_be_handled)
	{
		async_todo_t *pop = to_be_handled;
		to_be_handled = pop->next;
		pop->next = 0;
		int ex = EXCEPTION_NONE;

		switch ((enum async_todo_type)pop->todo_type)
		{
		case todoClearedTimer:
		{
			// The timer can be cleared in one of the previous to_be_handled functions
			break;
		}
		case todoTimer:
		{
			async_timer_t *timer = async_timer_cast(pop);
			ex = async_callback_call(manager, &timer->callback, 0, 0, 0, false);
			if (0 == timer->periodic ||
				// the timer can be cleared in the callback itself
				todoClearedTimer == timer->header.todo_type)
				break;
			timer->due += timer->periodic;
			async_todo_put_in_list(manager, &timer->header);
			break;
		}
		case todoPromise:
		{
			ex = async_promise_do(manager, pop);
			break;
		}
		}

		{
			int ex2 = async_todo_unlink(manager, pop);
			if (EXCEPTION_NONE == ex)
				ex = ex2;
		}

		if (EXCEPTION_NONE != ex)
		{
			// put back all remaining todo's
			reverse_stack(async_todo_t, to_be_handled);
			while (to_be_handled)
			{
				async_todo_t *pop = to_be_handled;
				to_be_handled = pop->next;
				pop->next = manager->todo_list;
				manager->todo_list = pop;
				pop->in_todo_list = 1;
			}
			return ex;
		}
	}
	return EXCEPTION_NONE;
}

static int64_t
async_how_long_to_next_todo( async_manager_t *manager )
{
	while (manager->todo_list)
	{
		switch ((enum async_todo_type)manager->todo_list->todo_type)
		{
		case todoClearedTimer:
		{
			async_todo_t *pop = manager->todo_list;
			manager->todo_list = pop->next;
			pop->next = 0;
			pop->in_todo_list = 0;
			async_todo_unlink(manager, pop);
			continue;
		}
		case todoPromise:
			return 0;
		case todoTimer:
		{
			int64_t now = async_timer_current_time();
			int64_t due = async_timer_cast(manager->todo_list)->due;
			if (due > now)
				return due - now;
			return 0;
		}
		}
	}

	// Nothing in todo list
	return -1;
}

enum
{
	timerTimeout = 0,
	timerPeriodic,
	timerImmediate,
};

static const char *_strTimer = "async.timer";

static void
close_timer(void *p)
{
	async_timer_t *timer = p;
	if (timer)
	{
		async_todo_unlink(0, &timer->header);
	}
}

static uc_value_t *
createTimer(uc_vm_t *vm, size_t nargs, int type)
{
	uc_value_t *cb = uc_fn_arg(0);
	if (!ucv_is_callable(cb))
	{
		uc_vm_raise_exception(vm, EXCEPTION_TYPE, "arg1 needs to be callable");
		return 0;
	}
	int64_t timeout = 0;
	if (nargs > 1 && timerImmediate != type)
	{
		uc_value_t *arg2 = uc_fn_arg(1);
		timeout = ucv_int64_get(arg2);
	}
	else if (timerPeriodic == type)
	{
		uc_vm_raise_exception(vm, EXCEPTION_TYPE, "arg2 needs to be a number");
		return 0;
	}

	if (timerPeriodic == type && timeout < 1)
		timeout = 1;

	size_t startarg = 2;
	if (timerImmediate == type)
		startarg = 1;

	async_manager_t *manager = async_manager_get( vm );

	async_timer_t *timer = uc_timer_ucode_new( manager, nargs, cb, startarg);

	timer->periodic = (timerPeriodic == type) ? timeout : 0;

	if (timerImmediate == type)
	{
		timer->due = 0;
	}
	else
	{
		timer->due = async_timer_current_time() + timeout;
	}

	async_todo_put_in_list(manager, &timer->header);
	timer->header.refcount--;

	if (0 == manager->timer_type)
	{
		static const uc_function_list_t timer_fns[] = {};
		manager->timer_type = uc_type_declare(vm, _strTimer, timer_fns, close_timer);
	}

	timer->header.refcount++;
	return uc_resource_new(manager->timer_type, timer);
}

enum
{
	promisePending = 0,

	promiseThen,
	promiseCatch,
	promiseFinally,
};

struct async_promise_method
{
	struct async_promise_method *next;
	struct async_callback callback;
};

typedef struct async_promise_method async_promise_method_t;

static void
uc_promise_func_free( async_manager_t *manager, async_promise_method_t *func)
{
	if (!func)
		return;
	async_callback_destroy( manager->vm, &func->callback);
	free(func);
}

static void
async_exception_clear( async_manager_t *manager, uc_exception_t *exception)
{
	exception->type = EXCEPTION_NONE;

	ucv_put(exception->stacktrace);
	exception->stacktrace = NULL;

	free(exception->message);
	exception->message = NULL;
}

static void
async_exception_free( async_manager_t *manager, uc_exception_t *exception)
{
	if (!exception)
		return;
	async_exception_clear( manager, exception);
	free(exception);
}

static void
async_exception_move( async_manager_t *manager, uc_exception_t *to, uc_exception_t *from)
{
	if (from && to)
	{
		to->type = from->type;
		from->type = EXCEPTION_NONE;
		to->stacktrace = from->stacktrace;
		from->stacktrace = 0;
		to->message = from->message;
		from->message = 0;
	}
}

static uc_exception_t *
async_exception_new( async_manager_t *manager, uc_exception_t *exception)
{
	uc_exception_t *ret = xalloc(sizeof(uc_exception_t));
	if (exception)
	{
		async_exception_move( manager, ret, exception);
	}
	return ret;
}

/**
 * Represents a promise object as returned by
 * {@link module:async#Promise|Promise()} or {@link module:async#PromiseAll|PromiseAll()}.
 *
 * @class module:async.promise
 * @hideconstructor
 *
 * @see {@link module:async#Promise|Promise()}
 *
 * @implements then(), catch() and finally()
 * 
 * @example
 *
 * const promise = async.Promise(…);
 *
 * promise.then( ()=>{} );
 * promise.catch( ()=>{} );
 * promise.finally( ()=>{} );
 */

struct async_promise
{
	async_todo_t header;
	async_manager_t *manager;
	struct uc_async_promise_resolver *resolver;
	union
	{
		uc_value_t *value;
		uc_exception_t *exception;
	} result;
	/* Contains the ucode function which caused the reject.
	To be used for the user feedback when no catch handler is found */
	uc_value_t *reject_caused_by;
	async_promise_method_t *stack;
};

typedef struct async_promise async_promise_t;

static async_promise_t *
uc_promise_new( async_manager_t *manager )
{
	async_promise_t *p = xalloc(sizeof(async_promise_t));
	p->header.refcount = 1;
	p->header.todo_type = todoPromise;
	p->manager = manager;
	p->manager->pending_promises_cnt++;
	p->header.promise_pending = 1;

	DEBUG_PRINTF("%-1.3lf new promise %p %u\n", uptime(manager), p, manager->pending_promises_cnt);
	return p;
}

static inline async_promise_t *
async_promise_cast(async_todo_t *todo)
{
	DEBUG_ASSERT(todoPromise == todo->todo_type);
	return (async_promise_t *)todo;
}

static void
async_promise_clear_result( async_manager_t *manager, async_promise_t *promise )
{
	if( !promise )
		return;
	ucv_put( promise->reject_caused_by );
	promise->reject_caused_by = 0;

	if (promise->header.promise_result_is_exception)
	{
		async_exception_free( manager, promise->result.exception);
		promise->header.promise_result_is_exception = 0;
		promise->result.exception = 0;
	}
	else
	{
		ucv_put( promise->result.value );
		promise->result.value = 0;
	}
}

static uc_chunk_t *
uc_vm_frame_chunk(uc_callframe_t *frame)
{
	return frame->closure ? &frame->closure->function->chunk : NULL;
}

static void 
async_vm_raise_exception_caused_by( uc_vm_t *vm, uc_value_t *caused_by, int type, const char *err, intptr_t arg )
{
	uc_callframe_t *frame = 0;
	if( caused_by && UC_CLOSURE == ucv_type(caused_by) ) 
	{
		uc_vector_grow(&vm->callframes);

		frame = &vm->callframes.entries[vm->callframes.count++];
		frame->closure = (uc_closure_t *)caused_by;
		frame->cfunction = NULL;
		frame->stackframe = vm->stack.count;
		frame->ip = uc_vm_frame_chunk(frame)->entries; /* that would point to the first instruction of the closure so the error message would point there as well */
		frame->ctx = NULL;
		frame->mcall = false;
	}
	uc_vm_raise_exception(vm, type, err, arg );
	if( frame )
	{
		/* "pop" artifical callframe */
		vm->callframes.count--;
	}
}

static int
async_promise_destroy( async_manager_t *manager, async_todo_t *todo)
{
	async_promise_t *promise = async_promise_cast(todo);
	uc_vm_t *vm = manager ? manager->vm : 0;
    async_manager_t *vm_is_active = vm ? manager : 0;

	if (vm_is_active && promise->header.promise_pending)
	{
		vm_is_active->pending_promises_cnt--;
		promise->header.promise_pending = 0;
	}

	DEBUG_PRINTF("%-1.3lf delete promise %p %d\n", uptime(vm_is_active), promise, 
		vm_is_active ? vm_is_active->pending_promises_cnt : -1 );

	int ret = EXCEPTION_NONE;
	bool uncaught = promiseCatch == promise->header.promise_state;
	if (uncaught)
	{
		if (vm_is_active && promise->header.promise_result_is_exception)
		{
			// put back the original exception
			async_exception_clear( vm_is_active, &vm->exception);
			async_exception_move( vm_is_active, &vm->exception, promise->result.exception);
			async_exception_free( vm_is_active, promise->result.exception);
			promise->result.exception = 0;
			promise->header.promise_result_is_exception = 0;
			ret = vm->exception.type;
			uncaught = false;
		}
	}

	uc_value_t *caused_by = 0;
	if( uncaught )
	{
		caused_by = promise->reject_caused_by;
		promise->reject_caused_by = 0;
	}
	async_promise_clear_result( vm_is_active, promise );

	async_promise_method_t *stack = promise->stack;
	for (; stack;)
	{
		async_promise_method_t *pop = stack;
		stack = pop->next;

		if (vm_is_active && !(uncaught) && promiseFinally == pop->callback.type)
			async_callback_call( vm_is_active, &pop->callback, 0, 0, 0, false);
		async_callback_destroy( vm, &pop->callback);
		free(pop);
	}

	if (uncaught)
	{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#pragma GCC diagnostic ignored "-Wuse-after-free"
		static const char *err = "Rejected promise %p without catch handler\n";
		if (vm_is_active)
		{
			async_vm_raise_exception_caused_by( vm, caused_by, ret = EXCEPTION_RUNTIME, err, (intptr_t)promise );
		}
		else
		{
            if( !manager || !manager->silent )
			    printf(err, promise);
		}
#pragma GCC diagnostic pop
	}

	ucv_put( caused_by );
	return ret;
}

static void async_promise_method_pushback( async_manager_t *manager, async_promise_t *promise, async_promise_method_t *func)
{
	async_promise_method_t *previous = 0;
	for (async_promise_method_t *it = promise->stack; it; it = it->next)
	{
		previous = it;
	}
	if (previous)
		previous->next = func;
	else
		promise->stack = func;

	if (0 == promise->header.promise_pending)
	{
		promise->header.promise_pending = 1;
		manager->pending_promises_cnt++;
	}

	if ((0 == promise->header.in_todo_list) && 
		(promisePending != promise->header.promise_state))
	{
		async_todo_put_in_list( manager, &promise->header);
	}
}

static void
async_promise_add_ucode_func( async_manager_t *manager, async_promise_t *promise, uc_value_t *func, int type)
{
	if (!ucv_is_callable(func))
	{
		uc_vm_raise_exception(manager->vm, EXCEPTION_TYPE, "arg1 needs to be callable");
		return;
	}

	async_promise_method_t *pf = xalloc(sizeof(async_promise_method_t));
	pf->callback.type = type;
	pf->callback.callback_type = callbackUcode;
	pf->callback.ucode.func = ucv_get(func);
	pf->next = 0;

	async_promise_method_pushback(manager, promise, pf);
}

static void
async_promise_add_c_int_user_args_flags_func( async_manager_t *manager,
											async_promise_t *promise,
											int (*func)(uc_vm_t *, void *, uc_value_t **, size_t, int), void *user, int type)
{
	async_promise_method_t *pf = xalloc(sizeof(async_promise_method_t));
	pf->callback.type = type;
	pf->callback.callback_type = callbackC_int_user_args_flags;
	pf->callback.c_int_user_args_flags.func = func;
	pf->callback.c_int_user_args_flags.user = user;
	pf->next = 0;

	async_promise_method_pushback( manager, promise, pf);
}

/**
 * Represents a promise resolver object as passed to the callback function in
 * {@link module:async#Promise|Promise()}
 *
 * @class module:async.resolver
 * @hideconstructor
 *
 * @see {@link module:async#Promise|Promise()}
 *
 * @implements resolve() and reject()
 * 
 * @example
  * const promise = async.Promise((resolver)=>
 * {
 *      resolver.resolve( 'hello' );
 * });
 */

struct async_promise_resolver
{
	uc_async_promise_resolver_t header; // empty struct
	uint32_t refcount : 16;
	uint32_t type : 16;
	async_promise_t *promise;
	uc_value_t *callback; // The callback provided with the creator: async.Promise( callback )
};

typedef struct async_promise_resolver async_promise_resolver_t;

static inline async_promise_resolver_t *
async_promise_resolver_cast( struct uc_async_promise_resolver *p )
{
	return (async_promise_resolver_t *)p;
}

static async_promise_resolver_t *
async_promise_resolver_new( async_promise_t *promise )
{
	async_promise_resolver_t *resolver = xalloc(sizeof(async_promise_resolver_t));
	resolver->refcount = 1;
	promise->header.refcount++;
	resolver->promise = promise;
	promise->resolver = &resolver->header;
	return resolver;
}

static const char *_strPromise = "async.promise";
static const char *_strResolver = "async.promise.resolver";

static uc_value_t *
async_promise_then(uc_vm_t *vm, size_t nargs)
{
	async_promise_t **ppromise = uc_fn_this(_strPromise);
	if (!ppromise || !*ppromise)
		return 0;
	async_promise_add_ucode_func( (*ppromise)->manager, *ppromise, uc_fn_arg(0), promiseThen);
	return ucv_get(_uc_fn_this_res(vm));
}

static uc_value_t *
async_promise_catch(uc_vm_t *vm, size_t nargs)
{
	async_promise_t **ppromise = uc_fn_this(_strPromise);
	if (!ppromise || !*ppromise)
		return 0;
	async_promise_add_ucode_func( (*ppromise)->manager, *ppromise, uc_fn_arg(0), promiseCatch);
	return ucv_get(_uc_fn_this_res(vm));
}

static uc_value_t *
async_promise_finally(uc_vm_t *vm, size_t nargs)
{
	async_promise_t **ppromise = uc_fn_this(_strPromise);
	if (!ppromise || !*ppromise)
		return 0;
	async_promise_add_ucode_func( (*ppromise)->manager, *ppromise, uc_fn_arg(0), promiseFinally);
	return ucv_get(_uc_fn_this_res(vm));
}

static const uc_function_list_t promise_type_fns[] = {
	{"then", async_promise_then},
	{"catch", async_promise_catch},
	{"finally", async_promise_finally},
};

static void
close_promise(void *ud)
{
	async_promise_t *promise = ud;
	if (promise)
	{
		DEBUG_PRINTF("%-1.3lf close promise %p %u\n", uptime(promise->manager), promise, promise->header.refcount);
		async_todo_unlink(promise->manager, &promise->header);
	}
}

static int
async_promise_do( async_manager_t *manager, async_todo_t *todo)
{
	async_promise_t *promise = async_promise_cast(todo);

	int state = promise->header.promise_state;

	async_promise_method_t *next_to_be_handled = 0;
	// walk the stack searching for a handler
	for (; promise->stack;)
	{
		next_to_be_handled = promise->stack;
		promise->stack = next_to_be_handled->next;
		next_to_be_handled->next = 0;

		if (state == next_to_be_handled->callback.type  
			|| promiseFinally == next_to_be_handled->callback.type)
		{
			break;
		}

		uc_promise_func_free( manager, next_to_be_handled);
		next_to_be_handled = 0;
	}

	if( !next_to_be_handled )
	{
		// mark as 'not pending'
		if (promise->header.promise_pending)
		{
			promise->header.promise_pending = 0;
			if( manager ) manager->pending_promises_cnt--;
		}
		return EXCEPTION_NONE;
	}

	uc_value_t *out = 0;
	int ex = EXCEPTION_NONE;
	{
		uc_value_t *in = 0;
		if (promiseFinally != next_to_be_handled->callback.type)
		{
			if (promise->header.promise_result_is_exception)
			{
				in = ucv_string_new(promise->result.exception->message);
			}
			else
			{
				in = ucv_get( promise->result.value );
			}

			async_promise_clear_result( manager, promise );			
		}

		// reset the state, so we know when throw() is called
		promise->header.promise_state = promisePending;

		{
			async_promise_t *push = manager->active_promise;
			manager->active_promise = promise;
			ex = async_callback_call( manager, &next_to_be_handled->callback, &in, 1, &out, true);
			manager->active_promise = push;
		}

		ucv_put(in);
	}

	uc_value_t *caused_by = async_callback_get_ucode_func( manager, &next_to_be_handled->callback );
	int ittype = next_to_be_handled->callback.type;
	uc_promise_func_free( manager, next_to_be_handled);

	if (EXCEPTION_NONE != ex)
	{
		if (EXCEPTION_EXIT == ex)
		{
			ucv_put( caused_by );
			return ex;
		}
		ucv_put(out);
		ucv_put(promise->reject_caused_by);
		promise->reject_caused_by = caused_by;
		if( promiseCatch == promise->header.promise_state )
		{
			// Caused by a throw()
			async_exception_clear( manager, &manager->vm->exception );
		}
		else 
		{
			// Take over the exception
			promise->result.exception = async_exception_new( manager, &manager->vm->exception);
			promise->header.promise_result_is_exception = 1;
			promise->header.promise_state = promiseCatch;
		}
		// reschedule
		async_todo_put_in_list( manager, &promise->header );
		return EXCEPTION_NONE;
	}

	ucv_put(caused_by);
	caused_by = 0;

	if (promiseFinally == ittype)
	{
		// Return value of finally is ignored, if it's not an exception
		ucv_put(out);

		// put state back
		promise->header.promise_state = state;
		// reschedule
		async_todo_put_in_list( manager, &promise->header );
		return EXCEPTION_NONE;
	}

	{   // Is the result a promise?
		async_promise_t **ppromise = (async_promise_t **)ucv_resource_dataptr(out, _strPromise);
		if (ppromise && *ppromise)
		{
			async_promise_t *new_promise = *ppromise;
			// We must push it's handler stack in front of ours,
			// and adopt it's state and it's resolver
			async_promise_method_t *previous = 0;
			for (async_promise_method_t *it2 = new_promise->stack; it2; it2 = it2->next)
			{
				previous = it2;
			}
			if (previous)
				previous->next = promise->stack;
			else
				new_promise->stack = promise->stack;
			promise->stack = new_promise->stack;
			new_promise->stack = 0;

			if (promise->resolver)
			{
				// Shouldn't be possible, but handle anyway
				async_promise_resolver_cast( promise->resolver )->promise = 0;
				promise->resolver = 0;
				promise->header.refcount--;
			}

			if (new_promise->resolver)
			{
				async_promise_resolver_cast( new_promise->resolver )->promise = promise;
				promise->resolver = new_promise->resolver;
				new_promise->resolver = 0;
				new_promise->header.refcount--;
				promise->header.refcount++;
			}

			promise->result.value = new_promise->result.value;
			new_promise->result.value = 0;
			promise->header.promise_result_is_exception = new_promise->header.promise_result_is_exception;
			new_promise->header.promise_result_is_exception = 0;
			promise->header.promise_state = new_promise->header.promise_state;

			// destroys also new_promise
			ucv_put(out);

			if( promisePending == promise->header.promise_state )
			{
				// not reschedule. We must wait for the resolver to act
			}
			else 
			{
				// reschedule
				async_todo_put_in_list( manager, &promise->header );
			}

			return EXCEPTION_NONE;
		}
	}

	promise->header.promise_state = promiseThen;
	promise->result.value = out;
	promise->header.promise_result_is_exception = 0;
	// reschedule
	async_todo_put_in_list( manager, &promise->header );
	return EXCEPTION_NONE;
}

static void
async_resolve_or_reject( async_manager_t *manager, async_promise_resolver_t *resolver, uc_value_t *res, int type)
{
	if (!resolver || !resolver->promise)
		return;

	async_promise_t *promise = resolver->promise;
	resolver->promise = 0;
	promise->resolver = 0;

	if (promisePending != promise->header.promise_state)
	{
		async_todo_unlink( manager, &promise->header);
		return;
	}

	if (promiseThen == type)
		promise->header.promise_state = promiseThen;
	else if (promiseCatch == type)
		promise->header.promise_state = promiseCatch;

	promise->result.value = ucv_get(res);
	promise->header.promise_result_is_exception = 0;

	if( !promise->header.promise_pending )
	{
		promise->header.promise_pending = 1;
		manager->pending_promises_cnt++;
	}

	if (!promise->header.in_todo_list)
	{
		async_todo_put_in_list( manager, &promise->header);
	}

	async_todo_unlink( manager, &promise->header);
}

static uc_value_t *
async_resolver_resolve_or_reject(uc_vm_t *vm, size_t nargs,int type)
{
	async_promise_resolver_t **presolve = uc_fn_this(_strResolver);
	if (!presolve || !*presolve)
	{
		uc_vm_raise_exception( vm, EXCEPTION_RUNTIME, "function doesn't have an '%s' this", _strResolver );
		return 0;
	}
	async_manager_t *manager = async_manager_get( vm );
	async_resolve_or_reject( manager, *presolve, uc_fn_arg(0), type);
	return 0;
}


static uc_value_t *
async_resolver_resolve(uc_vm_t *vm, size_t nargs)
{
	return async_resolver_resolve_or_reject(vm, nargs, promiseThen);
}

static uc_value_t *
async_resolver_reject(uc_vm_t *vm, size_t nargs)
{
	return async_resolver_resolve_or_reject(vm, nargs, promiseCatch);
}

static int 
async_promise_resolver_unlink( async_manager_t *manager, async_promise_resolver_t *resolver )
{
	if( 0 == resolver || 0 != --resolver->refcount )
		return EXCEPTION_NONE;

	if (resolver->promise)
	{
		async_promise_t *promise = resolver->promise;
		resolver->promise = 0;
		promise->resolver = 0;

		DEBUG_PRINTF("%-1.3lf promise abandoned %p\n", uptime(promise->manager), promise);
		promise->result.value = ucv_string_new("Promise abandoned");
		promise->header.promise_result_is_exception = 0;
		promise->header.promise_state = promiseCatch;
		promise->reject_caused_by = resolver->callback;
		resolver->callback = 0;
		if (promise->manager)
			async_todo_put_in_list(promise->manager, &promise->header);
		async_todo_unlink( promise->manager, &promise->header);
	}

	if( resolver->callback )
		ucv_put( resolver->callback );
	free(resolver);
	return EXCEPTION_NONE;
}

static void close_resolver(void *ud)
{
	async_promise_resolver_t *resolver = ud;
	async_promise_resolver_unlink( 0, resolver );
}

static const uc_function_list_t resolver_type_fns[] = {
	{"resolve", async_resolver_resolve},
	{"reject", async_resolver_reject}};

static int
uc_resolver_immediate(uc_vm_t *vm, void *user, int flags)
{
	async_promise_resolver_t *resolver = user;
	async_manager_t *manager = async_manager_get( vm );
	int ex = EXCEPTION_NONE;
	if (flags & UC_ASYNC_CALLBACK_FLAG_EXECUTE)
	{
		uc_vm_stack_push(vm, ucv_get(resolver->callback));
		resolver->refcount++;
		uc_vm_stack_push(vm, uc_resource_new( manager->resolver_type, resolver));

		ex = uc_vm_call(vm, false, 1);

		if( EXCEPTION_NONE == ex )
			ucv_put(uc_vm_stack_pop(vm));
	}
	if (flags & UC_ASYNC_CALLBACK_FLAG_CLEANUP)
	{
		int ex2 = async_promise_resolver_unlink( manager, resolver );
		if( EXCEPTION_NONE == ex )
			ex = ex2;
	}
	return ex;
}

struct async_promise_array_result
{
	struct async_promise_array *promise_array;
	uc_value_t *value;
	int state; // only in use by ASYNC_PROMISE_ALLSETTLED
};

typedef enum {
	ASYNC_PROMISE_ALL = 1,
	ASYNC_PROMISE_ANY,
	ASYNC_PROMISE_RACE,
	ASYNC_PROMISE_ALLSETTLED,
} promise_array_type_t;

struct async_promise_array
{
	uint32_t refcount:9;
	uint32_t exec_refcount:9;
	uint32_t numresults: 8;
	uint32_t type : 4; // all, any, race, allsettled
	async_promise_resolver_t *resolver;
	
	struct async_promise_array_result results[];
};

static void async_promise_array_unlink(struct async_promise_array *promise_array)
{
	if (0 != --promise_array->refcount)
		return;
	for( uint32_t n=0; n<promise_array->numresults; n++ )
		ucv_put( promise_array->results[ n ].value );
	free(promise_array);
}

static int async_promise_array_resolve( async_manager_t *manager, struct async_promise_array *promise_array, uc_value_t **args, size_t nargs, int type)
{
	if( !promise_array->resolver )
		return EXCEPTION_NONE;

	if( promisePending == type )
	{
		DEBUG_PRINTF("%-1.3lf %p will be pending forever\n", uptime(manager), promise_array->resolver);
		// to prevent it from keeping the script running forever, we'll remove it's 'promise pending' status
		async_promise_t *promise = promise_array->resolver->promise;
		if( promise )
		{
			if( promise->header.promise_pending )
			{
				manager->pending_promises_cnt--;
				promise->header.promise_pending = 0;
			}
			// and cleanup
			promise->resolver = 0;
			promise_array->resolver->promise = 0;
			async_todo_unlink( manager, &promise->header );
		}
	}
	else 
	{
		DEBUG_PRINTF("%-1.3lf %p resolved\n", uptime(manager), promise_array->resolver);
		uc_value_t *value = 0;
		int value_type = 0;

		switch( (promise_array_type_t)promise_array->type )
		{
			case ASYNC_PROMISE_ALL:
				if( promiseCatch == type )
					value_type = 1;
				else if( promiseThen == type )
					value_type = 2;
				break;
			case ASYNC_PROMISE_ANY:
				if( promiseCatch == type )
					value_type = 2;
				else if( promiseThen == type )
					value_type = 1;
				break;
			case ASYNC_PROMISE_RACE:
				value_type = 1;
				break;
			case ASYNC_PROMISE_ALLSETTLED:
				value_type = 3;

		}
		switch( value_type )
		{
			case 1: // the provided argument in the current call
			{
				if( nargs > 0)
					value = args[0];
				break;
			}
			case 2: // the array of stored values
			{
				value = ucv_array_new_length( manager->vm, promise_array->numresults );
				for( uint32_t n=0; n<promise_array->numresults; n++ )
				{
					uc_value_t *elem = promise_array->results[ n ].value;
					promise_array->results[ n ].value = 0;
					if( elem )
						ucv_array_set( value, n, elem );
				}
				break;
			}
			case 3: // the array of stored values, as struct (for 'allsettled)
			{
				value = ucv_array_new_length( manager->vm, promise_array->numresults );
				uc_value_t *fullfilled = 0, *rejected = 0;
				for( uint32_t n=0; n<promise_array->numresults; n++ )
				{
					struct async_promise_array_result *result =
						&promise_array->results[ n ];
					uc_value_t *obj = ucv_object_new( manager->vm );
					ucv_get( obj );
					if( result->state == promiseCatch )
					{
						if( 0 == rejected ) rejected = ucv_string_new( "rejected" );
						ucv_object_add( obj, "status", ucv_get( rejected ) );
						ucv_object_add( obj, "reason", result->value );
						result->value = 0;
					}
					if( result->state == promiseThen )
					{
						if( 0 == fullfilled ) fullfilled = ucv_string_new( "fullfilled" );
						ucv_object_add( obj, "status", ucv_get( fullfilled ) );
						ucv_object_add( obj, "value", result->value );
						result->value = 0;
					}
					ucv_array_set( value, n, obj );
				}
				ucv_put( fullfilled );
				ucv_put( rejected );
				break;
			}
		}
		
		async_resolve_or_reject(manager, promise_array->resolver, value, type);
	}
	async_promise_resolver_unlink( manager, promise_array->resolver);
	promise_array->resolver = 0;
	return EXCEPTION_NONE;
}

static int async_promise_array_immediate(uc_vm_t *vm, void *user, int flags)
{
	/* When we come in this function, the array of promises didn't contain 
	any usable value. So what to do? */
	struct async_promise_array *promise_array = user;
	int ex = EXCEPTION_NONE;
	if (flags & UC_ASYNC_CALLBACK_FLAG_EXECUTE)
	{
		int type = 0;
		switch( (promise_array_type_t)promise_array->type )
		{
			case ASYNC_PROMISE_ALL:
				type = promiseThen;
				break;
			case ASYNC_PROMISE_ANY:
				type = promiseCatch;
				break;
			case ASYNC_PROMISE_RACE:
				/* According to the spec the promise should stay pending forever */
				type = promisePending;
				break;
			case ASYNC_PROMISE_ALLSETTLED:
				type = promiseThen;
				break;
		}
		ex = async_promise_array_resolve( async_manager_get( vm ), promise_array, 0, 0, type);
	}
	if (flags & UC_ASYNC_CALLBACK_FLAG_CLEANUP)
		async_promise_array_unlink(promise_array);
	return ex;
}

static int async_promise_array_then(uc_vm_t *vm, void *user, uc_value_t **args, size_t nargs, int flags)
{
	struct async_promise_array_result *result = user;
	struct async_promise_array *promise_array = result->promise_array;
	async_manager_t *manager = async_manager_get( vm );

	int ex = EXCEPTION_NONE;
	if (flags & UC_ASYNC_CALLBACK_FLAG_EXECUTE)
	{
		DEBUG_PRINTF("%-1.3lf promise_array_then()\n", uptime(manager));
		result->state = promiseThen;
		int cnt = --promise_array->exec_refcount;
		switch( (promise_array_type_t)promise_array->type )
		{
			case ASYNC_PROMISE_ALL:
				if( nargs ) result->value = ucv_get( args[ 0 ] );
				break;
			case ASYNC_PROMISE_ANY:
				cnt = 0;
				break;
			case ASYNC_PROMISE_RACE:
				cnt = 0;
				break;
			case ASYNC_PROMISE_ALLSETTLED:
				if( nargs ) result->value = ucv_get( args[ 0 ] );
				break;
		}
		if( 0 == cnt )
			ex = async_promise_array_resolve( manager, promise_array, args, nargs, promiseThen);
	}
	if (flags & UC_ASYNC_CALLBACK_FLAG_CLEANUP)
	{
		async_promise_array_unlink(promise_array);
	}
	return ex;
}

static int async_promise_array_catch(uc_vm_t *vm, void *user, uc_value_t **args, size_t nargs, int flags)
{
	struct async_promise_array_result *result = user;
	struct async_promise_array *promise_array = result->promise_array;
	async_manager_t *manager = async_manager_get( vm );
	int ex = EXCEPTION_NONE;
	if (flags & UC_ASYNC_CALLBACK_FLAG_EXECUTE)
	{
		DEBUG_PRINTF("%-1.3lf promise_array_catch()\n", uptime(manager));
		result->state = promiseCatch;
		int cnt = --promise_array->exec_refcount;
		switch( (promise_array_type_t)promise_array->type )
		{
			case ASYNC_PROMISE_ALL:
				cnt = 0;
				break;
			case ASYNC_PROMISE_ANY:
				if( nargs ) result->value = ucv_get( args[ 0 ] );
				break;
			case ASYNC_PROMISE_RACE:
				cnt = 0;
				break;
			case ASYNC_PROMISE_ALLSETTLED:
				if( nargs ) result->value = ucv_get( args[ 0 ] );
				break;
		}
		if( 0 == cnt )
			ex = async_promise_array_resolve(manager, promise_array, args, nargs, promiseCatch);
	}
	if (flags & UC_ASYNC_CALLBACK_FLAG_CLEANUP)
	{
		async_promise_array_unlink(promise_array);
	}
	return ex;
}

static uc_value_t *
async_promise_array_new( uc_vm_t *vm, size_t nargs, int type )
{
	uc_value_t *arr = uc_fn_arg(0);
	if (arr && arr->type != UC_ARRAY)
	{
		uc_vm_raise_exception(vm, EXCEPTION_TYPE, "arg1 needs to be an array");
		return 0;
	}

	size_t length = ucv_array_length(arr);
	if( length > 255 )
	{
		// promise_array->numresults has only 8 bits
		uc_vm_raise_exception(vm, EXCEPTION_TYPE, "arg1p[] may not exceed 255 elements");
		return 0;
	}

	async_manager_t *manager = async_manager_get( vm );
	if( !manager )
		return 0;

	struct async_promise_array *promise_array = 
		xalloc(sizeof(struct async_promise_array) + length * sizeof(struct async_promise_array_result));

	promise_array->type = type;
	async_promise_t *promise = uc_promise_new( manager );
	promise_array->resolver = async_promise_resolver_new(promise);

	for (size_t n = 0; n < length; n++)
	{
		uc_value_t *elem = ucv_array_get(arr, n);
		async_promise_t **ppromise = (async_promise_t **)ucv_resource_dataptr(elem, _strPromise);
		if (ppromise && *ppromise)
		{
			if( ASYNC_PROMISE_RACE == type &&
				((*ppromise)->header.promise_state != promisePending) &&
				(promisePending == promise->header.promise_state) )
			{
				// this one should fullfill the promise
				if( (*ppromise)->header.promise_result_is_exception )
				{
					promise->result.exception = (*ppromise)->result.exception;
					(*ppromise)->result.exception = 0;
					(*ppromise)->header.promise_result_is_exception = 0;
				}
				else 
				{
					promise->result.value = (*ppromise)->result.value;
					(*ppromise)->result.value = 0;
				}
				promise->header.promise_state = (*ppromise)->header.promise_state;
				promise_array->resolver->promise = 0;
				promise->resolver = 0;
				async_todo_unlink( manager, &promise->header );
				// Lets continue normally to keep the code simple
			}
			struct async_promise_array_result *slot = 
			&promise_array->results[ promise_array->numresults++ ];
			slot->promise_array = promise_array;
			promise_array->exec_refcount++;
			promise_array->refcount++;
			async_promise_add_c_int_user_args_flags_func( manager, *ppromise, async_promise_array_then, slot, promiseThen);
			promise_array->refcount++;
			async_promise_add_c_int_user_args_flags_func( manager, *ppromise, async_promise_array_catch, slot, promiseCatch);
		}
		else if( ASYNC_PROMISE_RACE == type )
		{
			if( promisePending == promise->header.promise_state )
			{
				// Promise should be resolved by this value
				promise->result.value = ucv_get( elem );
				promise->header.promise_state = promiseThen;
				promise_array->resolver->promise = 0;
				promise->resolver = 0;
				async_todo_unlink( manager, &promise->header );
			}
		}
		else if( ASYNC_PROMISE_ALLSETTLED == type )
		{
			// This value should simply show up as fullfilled:
			struct async_promise_array_result *slot = 
			&promise_array->results[ promise_array->numresults++ ];
			slot->promise_array = promise_array;
			slot->state = promiseThen;
			slot->value = ucv_get( elem );
		}
	}

	if (0 == promise_array->exec_refcount && 0 != promise_array->resolver->promise )
	{
		// Array didn't contain any promises. We will resolve in a 'setImmediate'.
		promise_array->refcount++;
		async_timer_t *timer = async_timer_c_int_user_flags_new( manager, async_promise_array_immediate, promise_array);
		async_todo_put_in_list( manager, &timer->header);
		timer->header.refcount--;
	}

	return uc_resource_new( manager->promise_type, promise);
}

static struct uc_value *
_uc_async_new_promise( struct uc_async_manager *_man, uc_async_promise_resolver_t **resolver)
{
	if( !resolver )
		return 0;
	async_manager_t *manager = async_manager_cast( _man );
	async_promise_t *promise = uc_promise_new( manager );
	*resolver = &async_promise_resolver_new(promise)->header;
	return uc_resource_new( manager->promise_type, promise);
}

static void
_uc_async_resolve_reject( struct uc_async_manager *_man, uc_async_promise_resolver_t **resolver, uc_value_t *res, bool resolve)
{
	if (!resolver || !*resolver)
		return;
	async_manager_t *manager = async_manager_cast( _man );
	async_promise_resolver_t *res2 = async_promise_resolver_cast(*resolver);
	*resolver = 0;
	async_resolve_or_reject( manager, res2, res, resolve ? promiseThen : promiseCatch);
	async_promise_resolver_unlink( manager, res2);
}

struct async_callback_link
{
	struct async_callback_link *next;
	struct async_callback callback;
};

struct async_callback_queuer
{
	struct uc_async_callback_queuer header;
	struct async_callback_unique_in_vm *unique_in_vm;
};

typedef struct async_callback_queuer async_callback_queuer_t;

static inline async_callback_queuer_t *
async_callback_queuer_cast(struct uc_async_callback_queuer *handler)
{
	return (async_callback_queuer_t *)handler;
}

static inline async_callback_queuer_t const *
async_callback_queuer_cast_const(struct uc_async_callback_queuer const *handler)
{
	return (async_callback_queuer_t const *)handler;
}

static int
async_handle_callbacks( async_manager_t *manager )
{
	if( 0 == manager || 0 == manager->callback_queuer )
		return EXCEPTION_NONE;
	async_callback_queuer_t *l_queuer = async_callback_queuer_cast(manager->callback_queuer);
	struct async_callback_link *stack = uc_unique_lock_stack(l_queuer->unique_in_vm);
	uc_unique_unlock_stack(l_queuer->unique_in_vm, 0);

	if (0 == stack)
		return EXCEPTION_NONE;

	reverse_stack(struct async_callback_link, stack);

	while (stack)
	{
		struct async_callback_link *pop = stack;
		stack = pop->next;
		int ex = async_callback_call( manager, &pop->callback, 0, 0, 0, true);
		free( pop );
		if (EXCEPTION_NONE == ex)
			continue;
		if (stack)
		{
			// put remaining stack back
			struct async_callback_link *last = stack;
			reverse_stack(struct async_callback_link, stack);
			last->next = uc_unique_lock_stack(l_queuer->unique_in_vm);
			uc_unique_unlock_stack(l_queuer->unique_in_vm, stack);
		}
		return ex;
	}
	return EXCEPTION_NONE;
}

static bool
async_any_callbacks_waiting( async_manager_t *manager )
{
	if (0 == manager || 0 == manager->callback_queuer)
		return false;
	async_callback_queuer_t *l_queuer = async_callback_queuer_cast(manager->callback_queuer);
	if ((intptr_t)l_queuer->unique_in_vm->stack & ~(intptr_t)3)
		return true;
	return false;
}

static bool
_uc_async_request_callback(struct uc_async_callback_queuer const *queuer,
						   int (*func)(struct uc_vm *, void *, int), void *user)
{
	struct async_callback_link *pfunc = xalloc(sizeof(struct async_callback_link));
	pfunc->callback.callback_type = callbackC_int_user_flags;
	pfunc->callback.c_int_user_flags.func = func;
	pfunc->callback.c_int_user_flags.user = user;

	const async_callback_queuer_t *l_queuer = async_callback_queuer_cast_const(queuer);

	struct async_callback_link *stack = uc_unique_lock_stack(l_queuer->unique_in_vm);

	if (stack == (struct async_callback_link *)l_queuer->unique_in_vm)
	{
		// vm doesn't exist anymore
		uc_unique_unlock_stack(l_queuer->unique_in_vm, stack);
		free(pfunc);
		return false;
	}

	pfunc->next = stack;
	uc_unique_unlock_stack(l_queuer->unique_in_vm, pfunc);

	async_unique_wakeup( l_queuer->unique_in_vm );
	return true;
}

static void
_uc_async_callback_queuer_free(struct uc_async_callback_queuer const **pqueuer)
{
	if (0 == pqueuer || 0 == *pqueuer)
		return;
	async_callback_queuer_t const *l_queuer = async_callback_queuer_cast_const(*pqueuer);
	*pqueuer = 0;

	struct async_callback_unique_in_vm *unique_in_vm = l_queuer->unique_in_vm;
	free((void *)l_queuer);
	async_unique_in_vm_unlink(unique_in_vm);
}

static int _async_create_timer( uc_vm_t *vm, void *user, int flags)
{
	async_timer_t *timer = user;
	async_manager_t *manager = async_manager_get( vm );

	if (flags & UC_ASYNC_CALLBACK_FLAG_EXECUTE)
	{
		async_todo_put_in_list( manager, &timer->header);
	}
	if (flags & UC_ASYNC_CALLBACK_FLAG_CLEANUP)
	{
		async_todo_unlink( manager, &timer->header);
	}
	return EXCEPTION_NONE;
}

static uc_async_timer_t *
_uc_async_create_timer(struct uc_async_callback_queuer const *queuer,
					   int (*cb)(uc_vm_t *, void *, int), void *user, uint32_t msec, bool periodic)
{
	async_timer_t *timer = async_timer_c_int_user_flags_new(0, cb, user);
	timer->due = async_timer_current_time() + msec;
	if (periodic)
		timer->periodic = msec;
	timer->header.refcount++;

	async_callback_queuer_t const *l_queuer = async_callback_queuer_cast_const(queuer);

	// are we synchron?
	async_manager_t *manager = async_unique_is_synchron(l_queuer->unique_in_vm);
	if (manager)
	{
		_async_create_timer( manager->vm, timer, UC_ASYNC_CALLBACK_FLAG_EXECUTE | UC_ASYNC_CALLBACK_FLAG_CLEANUP);
	}
	else
	{
		_uc_async_request_callback(queuer, _async_create_timer, timer);
	}

	return &timer->header.header;
}

static int
_async_free_timer(uc_vm_t *vm, void *user, int flags)
{
	uintptr_t v = (uintptr_t)user;
	bool clear = v & 1;
	v = v & ~((uintptr_t)1);
	async_timer_t *timer = (async_timer_t *)v;

	if (flags & UC_ASYNC_CALLBACK_FLAG_EXECUTE)
	{
		if (clear)
			timer->header.todo_type = todoClearedTimer;
	}
	if (flags & UC_ASYNC_CALLBACK_FLAG_CLEANUP)
	{
		async_todo_unlink( async_manager_get(vm), &timer->header);
	}
	return EXCEPTION_NONE;
}

static void
_uc_async_free_timer(struct uc_async_callback_queuer const *queuer,
					 uc_async_timer_t **_pptimer, bool clear)
{
	async_callback_queuer_t const *l_queuer = async_callback_queuer_cast_const(queuer);
	async_timer_t **pptimer = (async_timer_t **)_pptimer;
	if (!pptimer || !*pptimer)
		return;
	async_timer_t *timer = *pptimer;
	*_pptimer = 0;

	// use bit 0 to store the clear flag
	if (clear)
	{
		timer = (async_timer_t *)(((uintptr_t)timer) | 1);
	}

	// are we synchron?
	async_manager_t *manager = async_unique_is_synchron(l_queuer->unique_in_vm);
	if (manager)
	{
		_async_free_timer( manager->vm, timer, UC_ASYNC_CALLBACK_FLAG_EXECUTE | UC_ASYNC_CALLBACK_FLAG_CLEANUP);
	}
	else
	{
		_uc_async_request_callback(queuer, _async_free_timer, timer);
	}
}

static async_callback_queuer_t *
async_callback_queuer_new()
{
	async_callback_queuer_t *pcallbackhandler = xalloc(sizeof(async_callback_queuer_t));
	pcallbackhandler->header.free = _uc_async_callback_queuer_free;
	pcallbackhandler->header.request_callback = _uc_async_request_callback;
	pcallbackhandler->header.create_timer = _uc_async_create_timer;
	pcallbackhandler->header.free_timer = _uc_async_free_timer;
	return pcallbackhandler;
}

// callback handler functions
static struct uc_async_callback_queuer const *
_uc_async_new_callback_queuer( struct uc_async_manager *_man )
{
	async_manager_t *manager = async_manager_cast( _man );
	if( 0 == manager )
		return 0;

	if (0 == manager->callback_queuer)
	{
		async_callback_queuer_t *pcallbackhandler = async_callback_queuer_new();
		manager->callback_queuer = &pcallbackhandler->header;
		pcallbackhandler->unique_in_vm = async_unique_in_vm_new(manager);
	}

	async_callback_queuer_t *l_queuer = async_callback_queuer_cast(manager->callback_queuer);
	struct async_callback_unique_in_vm *unique_in_vm = l_queuer->unique_in_vm;
	async_callback_queuer_t *pcallbackhandler = async_callback_queuer_new();
	async_unique_in_vm_link(unique_in_vm);
	pcallbackhandler->unique_in_vm = unique_in_vm;
	return &pcallbackhandler->header;
}

static void
async_callback_free( async_manager_t *manager, struct uc_async_callback_queuer *queuer)
{
	if (0 == queuer)
		return;
	async_callback_queuer_t *l_queuer = async_callback_queuer_cast(queuer);
	struct async_callback_link *stack = uc_unique_lock_stack(l_queuer->unique_in_vm);
	// write sentinel value meaning that callbacks are disabled forever
	uc_unique_unlock_stack(l_queuer->unique_in_vm, (void *)l_queuer->unique_in_vm);

	struct uc_async_callback_queuer const *pconsth = queuer;
	_uc_async_callback_queuer_free(&pconsth);

	// call all function on stack with exec=false to make them able to free up resources
	while (stack)
	{
		struct async_callback_link *pop = stack;
		stack = pop->next;
		async_callback_destroy( manager->vm, &pop->callback);
	}
}

static void
async_manager_free(uc_vm_t *vm, async_manager_t *manager)
{
	while (manager->todo_list)
	{
		async_todo_t *pop = manager->todo_list;
		manager->todo_list = pop->next;
		pop->next = 0;
		async_todo_unlink( manager, pop);
	}

	async_callback_free( manager, manager->callback_queuer);
	free(manager);
}

static int
async_event_pump( struct uc_async_manager *_man, unsigned max_wait, int flags)
{
	async_manager_t *manager = async_manager_cast( _man );

	if (flags & UC_ASYNC_PUMP_PUMP)
	{
		int64_t until = 0;
		if (UINT_MAX == max_wait)
		{
			until = INT64_MAX;
		}
		else if (max_wait)
		{
			until = async_timer_current_time() + max_wait;
		}

		do
		{
			DEBUG_PRINTF("%-1.3lf Pump!\n", uptime( manager ));
			int ex = async_handle_todo( manager );
			if (EXCEPTION_NONE != ex)
			{
				if (EXCEPTION_EXIT == ex)
                {
                    manager->silent = 1;
					return STATUS_EXIT;
                }
				return ERROR_RUNTIME;
			}

			ex = async_handle_callbacks( manager );
			if (EXCEPTION_NONE != ex)
			{
				if (EXCEPTION_EXIT == ex)
                {
                    manager->silent = 1;
					return STATUS_EXIT;
                }
				return ERROR_RUNTIME;
			}

			int64_t tosleep = async_how_long_to_next_todo( manager );

			if (-1 == tosleep) // no todo list
			{
				if( 0 == manager->pending_promises_cnt ) // no pending promises
				{
					// Nothing to do anymore
					DEBUG_PRINTF("%-1.3lf Last!\n", uptime( manager ));
					break; // do {} while( )
				}
				tosleep = INT64_MAX;
			}

			if (max_wait && !async_any_callbacks_waiting( manager ))
			{
				if ((unsigned)tosleep > max_wait)
					tosleep = max_wait;
				if (tosleep > 0)
				{
#ifdef DEBUG_PRINT					
					DEBUG_PRINTF("%-1.3lf Start wait\n", uptime( manager ));
					/* The printf could have eaten a signal. 
					So look if something was added to the async stack */
					if( !async_any_callbacks_waiting( manager ) )
#endif
						async_unique_sleep(0, tosleep);
					DEBUG_PRINTF("%-1.3lf End wait\n", uptime( manager ));
				}
			}
		} while ((flags & UC_ASYNC_PUMP_CYCLIC) &&
				 (until > async_timer_current_time()));
	} // if( flags & UC_ASYNC_PUMP_PUMP )

	if (flags & UC_ASYNC_PUMP_CLEANUP)
	{
		uc_vm_t *vm = manager->vm;
		manager->vm = 0;
		uc_vm_registry_delete( vm, "async.manager" );
		async_manager_free( vm, manager );
	}

	return STATUS_OK;
}

static char *uc_cast_string(uc_vm_t *vm, uc_value_t **v, bool *freeable) {
	if (ucv_type(*v) == UC_STRING) {
		*freeable = false;

		return _ucv_string_get(v);
	}

	*freeable = true;

	return ucv_to_string(vm, *v);
}

/**
 * Creates and returns a promise. The provided resolver function will be called 
 * asynchronously.
 *
 * @function module:async#Promise
 *
  * @param {Function} callback
 * The callback used to deliver the {?module:async.resolver} object.
 *
 * @returns {?module:async.promise}
 *
 * @example
 * // Create a promise
 * async Promise( (resolver)=>
 * {
 *	 resolver.resolve( 'world' );
 *	 print( 'hello ' ); 
 * }).then( (a)=>
 * {
 *	 print( a );
 * });
 * // will output 'hello world'
 */

static uc_value_t *
Promise(uc_vm_t *vm, size_t nargs)
{
	uc_value_t *func = uc_fn_arg(0);
	if (!ucv_is_callable(func))
	{
		uc_vm_raise_exception(vm, EXCEPTION_TYPE, "arg1 needs to be callable");
		return 0;
	}

	async_manager_t *manager = async_manager_get( vm );

	async_promise_t *promise = uc_promise_new( manager );
	async_promise_resolver_t *resolver = async_promise_resolver_new(promise);
	resolver->callback = ucv_get(func);

	async_timer_t *timer = async_timer_c_int_user_flags_new( manager, uc_resolver_immediate, resolver);
	async_todo_put_in_list( manager, &timer->header);
	timer->header.refcount--;
	
	return uc_resource_new( manager->promise_type, promise );
}

/**
 * Takes an array of {@link module:async.promise|promises}, and returns a single promise.
 * 
 * When one of the promises is rejected, the new promise is rejected immediately 
 * with the reject value.
 * When all of the promises are resolved, the new promise is resolved with an array
 * of the resolve values.
 *
 * @function module:async#PromiseAll
 *
 * @param {Array} promises
 * Array of {@link module:async.promise|promises}. Elements in the array which are no promise are ignored.
 * An empty array will cause the returned promise to be resolved immediately.
 *
 * @returns {?module:async.promise}
 *
 * @example
 * function NewPromise( interval, value, reject )
 * {
 *	 return async.Promise( (resolver)=>
 *	 {
 *		  async.setTimeout( ()=>
 *		  {
 *			  if( reject ) resolver.reject( value );
 *			  resolver.resolve( value );
 *		  }, interval );
 *	 } );
 * }
 *
 * // Create an array of promises:
 * let promises = [ NewPromise(300,'A',false),
 *				  NewPromise(200,'B',false),
 *				  'hello',
 *				  NewPromise(400,'C',true) ];
 * 
 * async.PromiseAll( promises ).then( (a)=>
 * {
 *	 print( 'fullfilled ', a, '\n' );
 * }).catch( (a)=>
 * {
 *	 print( 'rejected ', a, '\n' );
 * });
 * // will output 'rejected C', however, if you change the boolean for 'C' in false,
 * // it will output 'fullfilled [ "A", "B", "C" ]'
 */

static uc_value_t *
PromiseAll(uc_vm_t *vm, size_t nargs)
{
	return async_promise_array_new( vm, nargs, ASYNC_PROMISE_ALL );
}

/**
 * Takes an array of {@link module:async.promise|promises}, and returns a single promise.
 * 
 * The first promise which resolves, resolves the new promise.
 * When none of the promises is resolved, the new promise will be rejected with
 * an array of reject objects.
 *
 * @function module:async#PromiseAny
 *
 * @param {Array} promises
 * Array of {@link module:async.promise|promises}. Elements in the array which are no promise are ignored.
 * An empty array will cause the returned promise to be rejected immediately.
 *
 * @returns {?module:async.promise}
 *
 * @example
 * function NewPromise( interval, value, reject )
 * {
 *	 return async.Promise( (resolver)=>
 *	 {
 *		  async.setTimeout( ()=>
 *		  {
 *			  if( reject ) resolver.reject( value );
 *			  resolver.resolve( value );
 *		  }, interval );
 *	 } );
 * }
 *
 * // Create an array of promises:
 * let promises = [ NewPromise(300,'A',false),
 *				  NewPromise(200,'B',false),
 *				  'hello',
 *				  NewPromise(400,'C',true) ];
 * 
 * async.PromiseAny( promises ).then( (a)=>
 * {
 *	 print( 'fullfilled ', a, '\n' );
 * }).catch( (a)=>
 * {
 *	 print( 'rejected ', a, '\n' );
 * });
 * // will output 'fullfilled B', however, if you change the booleans for 'A' and 'B'
 * // in true, it will output 'rejected [ "A", "B", "C" ]'
 */

static uc_value_t *
PromiseAny(uc_vm_t *vm, size_t nargs)
{
	return async_promise_array_new( vm, nargs, ASYNC_PROMISE_ANY );
}

/**
 * Takes an array of {@link module:async.promise|promises}, and returns a single promise.
 * 
 * The first promise which settles, settles the new promise.
 * When the array of promises contains promises which are already settled,
 * or non-promise values, the new promise will settle with the first one of those 
 * in the array.
 *
 * @function module:async#PromiseRace
 *
 * @param {Array} promises
 * Array of {@link module:async.promise|promises}. An empty array will cause the returned 
 * promise to be pending forever.
 *
 * @returns {?module:async.promise}
 *
 * @example
 * function NewPromise( interval, value, reject )
 * {
 *	 return async.Promise( (resolver)=>
 *	 {
 *		  async.setTimeout( ()=>
 *		  {
 *			  if( reject ) resolver.reject( value );
 *			  resolver.resolve( value );
 *		  }, interval );
 *	 } );
 * }
 *
 * // Create an array of promises:
 * let promises = [ NewPromise(300,'A',false),
 *				  NewPromise(200,'B',false),
 *				  'hello',
 *				  NewPromise(50,'C',true) ];
 * 
 * async.PromiseRace( promises ).then( (a)=>
 * {
 *	 print( 'fullfilled ', a, '\n' );
 * }).catch( (a)=>
 * {
 *	 print( 'rejected ', a, '\n' );
 * });
 * // will output 'fullfilled hello', however, if you remove the 'hello' value, 
 * // it will output 'rejected C'
 */

static uc_value_t *
PromiseRace(uc_vm_t *vm, size_t nargs)
{
	return async_promise_array_new( vm, nargs, ASYNC_PROMISE_RACE );
}

/**
 * Takes an array of {@link module:async.promise|promises}, and returns a single promise.
 * 
 * When all promises are settled, the new promise is fullfilled with an array of objects.
 * ```
 * {
 *	 status: 'fullfilled', // or 'rejected'
 *	 reason: 'whatever', // only set in case of 'rejected'
 *	 value: 'whatever' // only set in case of 'fullfilled'
 * }
 * ```
 * A non-promise value in the promise array will be 'fullfilled' in the result array.
 *
 * @function module:async#PromiseAllSettled
 *
 * @param {Array} promises
 * Array of {@link module:async.promise|promises}. 
 *
 * @returns {?module:async.promise}
 *
 * @example
 * function NewPromise( interval, value, reject )
 * {
 *	 return async.Promise( (resolver)=>
 *	 {
 *		  async.setTimeout( ()=>
 *		  {
 *			  if( reject ) resolver.reject( value );
 *			  resolver.resolve( value );
 *		  }, interval );
 *	 } );
 * }
 *
 * // Create an array of promises:
 * let promises = [ NewPromise(300,'A',false),
 *				  NewPromise(200,'B',false),
 *				  'hello',
 *				  NewPromise(50,'C',true) ];
 * 
 * async.PromiseAllSettled( promises ).then( (a)=>
 * {
 *	 print( 'fullfilled ', a, '\n' );
 * }).catch( (a)=>
 * {
 *	 print( 'rejected ', a, '\n' );
 * });
 * // will output 'fullfilled [ { "status": "fullfilled", "value": "A" }, { "status": "fullfilled", "value": "B" }, { "status": "fullfilled", "value": "hello" }, { "status": "rejected", "reason": "C" } ]'
 */

static uc_value_t *
PromiseAllSettled(uc_vm_t *vm, size_t nargs)
{
	return async_promise_array_new( vm, nargs, ASYNC_PROMISE_ALLSETTLED );
}

/**
 * Helper function to make it possible to throw an object from a promise method.
 * 
 * When calling this function from within a promise method, the promise is rejected, and 
 * the provided argument is delivered in the next catch handler.
 * 
 * When called outside a promise method, async.throw( a ) is more or less a synonym of
 * die( \`${a}\` );
 *
 * @function module:async#throw
 *
 * @param {Any} error
 * Object or value to be delivered in the next catch handler
 * 
 * @throws {Error}
 * 
 * @example
 * async.Promise( (resolver)=>{resolver.resolve('hello ')}).then((a)=>
 *	 print( a );
 *	 async.throw( 'world' );
 *	 print( 'this will never be printed' )
 * }).then( (a)=>
 * {
 *	 print( 'this will also never be printed' );
 * }).catch( (a)=>
 *	 print( a ) );
 * });
 * // Will output 'hello world'.
 */
 
static uc_value_t *
Throw( uc_vm_t *vm, size_t nargs )
{
	async_manager_t *manager = async_manager_get( vm );
	async_promise_t *promise = (0 == manager) ? 0 : manager->active_promise;

	if( promise )
	{
		// Throw being called from inside a promise
		promise->header.promise_state = promiseCatch;
		promise->result.value = ucv_get( uc_fn_arg(0) );
		promise->header.promise_result_is_exception = 0;
		
		// create a 'lightweight' exception, to prevent further code execution
		vm->exception.type = EXCEPTION_USER;
		return ucv_boolean_new( true );
	}

	// Create a 'fullblown' exception
	uc_value_t *v = uc_fn_arg(0);
	bool freeable = false;
	char *casted = uc_cast_string( vm, &v, &freeable );
	uc_vm_raise_exception( vm, EXCEPTION_USER, "%s", casted );
	if( freeable )
		free( casted );
	return ucv_boolean_new( false );
}

/**
 * Event pump, in which the asynchronous functions are actually executed.
 * 
 * You can call this inside your program regularly, or at the end. 
 * When omitted the async functions will be called after the script 
 * has 'ended', by the vm.
 *
 * @function module:async#PumpEvents
 *
 * @param {Number} [timespan=null]
 * Timespan in msec. The function will keep pumping events until *timespan* 
 * msec has elapsed. When no timespan is provided, PumpEvents() will keep 
 * pumping until no timers are left and no active promises are around, 
 * or an exception occurs.
 * 
 * @param {Boolean} [single=false]
 * Flag to only pump once, and then 'sleep' *timespan* msec, or 
 * return at the moment the next event is due to be executed, 
 * whatever comes first.
 * This is usable if you want to do something between each stroke of the 
 * event pump:
 * ```
 * let promise = async.Promise( (resolver)=>{ resolver.resolve( 1 ) } );
 * for( let i=0; i<5; i++ )
 * {
 *	 promise = promise.then( (num)=>{ print(num); return ++num } );
 * }
 * 
 * while( async.PumpEvents( 1000, true ) )
 * {
 *	 print( ` *${async.uptime()}* ` );
 * }
 * // will output something like '*0.002* 1 *0.003* 2 *0.003* 3 *0.004* 4 *0.005* 5 *0.005*' 
 * // and then exit.
 * ```
 * But also
 * ```
 * let timer;
 * timer = async.setPeriodic( ( cnt )=>
 * {
 *	 if( ++cnt.cnt == 5 )
 *		 async.clearTimeout( timer );
 *	 print( cnt.cnt );
 * }, 100, { cnt: 0 } );
 * 
 * while( async.PumpEvents( 1000, true ) )
 * {
 *	 print( ` *${async.uptime()}* ` );
 * }
 * // will output something like '*0.101* 1 *0.201* 2 *0.301* 3 *0.401* 4 *0.501* 5' 
 * // and then exit.
 * ```
 * 
 * @return {Boolean} 
 * True if more events are (or will be) available, False if no more events are to be expected.
 * 
 * @example
 * async.setTimeout( ()=>{}, 10000 );
 * 
 * let count = 0;
 * while( async.PumpEvents( 1000 ) )
 * { 
 *	 print( `${++count} ` );
 * }
 * // Will output '1 2 3 4 5 6 7 9' and then exit. 
 * // Maybe '10' is also printed, depending on the exact timing.
 */

static uc_value_t *
PumpEvents(uc_vm_t *vm, size_t nargs)
{
	unsigned msec = UINT_MAX;
	int flags = UC_ASYNC_PUMP_CYCLIC | UC_ASYNC_PUMP_PUMP;
	uc_value_t *pmsec = uc_fn_arg(0);
	if (pmsec)
	{
		int64_t v = ucv_int64_get(pmsec);
		if (v > 0)
		{
			if (v < UINT_MAX)
				msec = (unsigned)v;
			else
				msec = UINT_MAX - 1;
		}
	}
	uc_value_t *psingle = uc_fn_arg(1);
	if (psingle)
	{
		bool v = ucv_boolean_get(psingle);
		if (v)
			flags &= ~UC_ASYNC_PUMP_CYCLIC;
	}

	async_manager_t *manager = async_manager_get( vm );
	if( !manager )
		return ucv_boolean_new(false);
	
	async_event_pump( &manager->header, msec, flags);

	if( manager->pending_promises_cnt ||
			manager->todo_list)
			return ucv_boolean_new(true);

	return ucv_boolean_new(false);
}

/**
 * Start a timer, to execute it's callback after a delay.
 * 
 * The timer can be stopped before the callback is executed using clearTimeout()
 * 
  * @function module:async#setTimeout
 *
 * @param {Function} callback
 * 
 * @param {Number} [interval = 0]
 * Optional time to be waited (in msec) before the callback is called.
 * 
 * @param {Any} ...args
 * Optional Argument(s) to be passed to the callback function.
 * 
 * @returns {?module:async.timer}
 * 
 * @example
 * async.setTimeout( (a)=>
 * {
 *	 print( a );
 * }, 10000, 'hello world' );
 * 
 * while( async.PumpEvents() );
 * // Will output 'hello world' after 10 seconds, and then exit.
 */

static uc_value_t *
setTimeout(uc_vm_t *vm, size_t nargs)
{
	return createTimer(vm, nargs, timerTimeout);
}

/**
 * Start a periodic timer, to execute it's callback each at interval.
 * 
 * The timer can be stopped using clearTimeout()
 * 
 * @function module:async#setPeriodic
 *
 * @param {Function} callback
 * 
 * @param {Number} interval
 * Interval time in millisec.
 * 
 * @param {Any} ...args
 * Optional Argument(s) to be passed to the callback function.
 * 
 * @returns {?module:async.timer}
 * 
 * @example
 * async.setPeriodic( (a)=>
 * {
 *	 print( `${++a.count}\n` );
 * }, 1000, { count: 0 } );
 * 
 * while( async.PumpEvents() );
 * // Will output '1\n2\n3\n...' forever.
 */

static uc_value_t *
setPeriodic(uc_vm_t *vm, size_t nargs)
{
	return createTimer(vm, nargs, timerPeriodic);
}

/**
 * Let callback be executed in the next event pump stroke.
 * 
 * In theory it can be stopped using clearTimeout()
 * 
 * A *setImmediate()* is executed before *setTimeout( ()=>{}, 0 )*. 
 * Background: the setTimeout() is scheduled at *now + 0 msec*, 
 * while 'setImmediate()' is scheduled at *start of time*, 
 * which has already passed.
 * 
 * @function module:async#setImmediate
 *
 * @param {Function} callback
 * 
 * @param {Any} ...args
 * Optional Argument(s) to be passed to the callback function.
 * 
 * @returns {?module:async.timer}
 * 
 * @example
 * async.setTimeout( (a)=>
 * {
 *	 print( a );
 * }, 0, 'world' );
 * async.setImmediate( (a)=>
 * {
 *	 print( a );
 * }, 'hello ' );
 *
 * while( async.PumpEvents() );
 * // Will output 'hello world', and exit.
 */

static uc_value_t *
setImmediate(uc_vm_t *vm, size_t nargs)
{
	return createTimer(vm, nargs, timerImmediate);
}

/**
 * Clears a timer. It's safe to call it more than once on the same timer object.
 * 
 * @function module:async#clearTimeout
 *
 * @param {?module:async.timer} timer
 * 
 * @returns {Boolean} 
 * True if the timer is a valid timer object, and false if it isn't.
 * 
 * @example
 * let timer = async.setTimeout( (a)=>
 * {
 *	 print( 'hello world' );
 * }, 1000 );
 * async.clearTimeout( timer );
 * while( async.PumpEvents() );
 * // Will output nothing, and exit immediately.
 */

static uc_value_t *
clearTimeout(uc_vm_t *vm, size_t nargs)
{
	async_timer_t **ptimer = (async_timer_t **)ucv_resource_dataptr(uc_fn_arg(0), _strTimer);
	if (ptimer && *ptimer)
	{
		async_timer_t *timer = *ptimer;
		timer->header.todo_type = todoClearedTimer;
		timer->due = 0;
		timer->periodic = 0;
		async_callback_destroy( vm, &timer->callback);
		return ucv_boolean_new(true);
	}
	return ucv_boolean_new(false);
}

#ifdef HAS_UPTIME
/**
 * Returns the uptime of the script (since importing the async plugin), in seconds, 
 * with a milli seconds resolution.
 * (Actually a debug helper, but I decided to leave it)
 * 
 * @function module:async#uptime
 *
 * @returns {Number} 
 * Uptime in seconds.
 * 
 * @example
 * let timer
 * timer = async.setPeriodic( (a)=>
 * {
 *	 if( async.uptime() > 5 )
 *		 async.clearTimeout( timer );
 *	 print( `${async.uptime()} ` );
 * }, 1000 );
 * 
 * while( async.PumpEvents() );
 * // Will output something like '0.003 1.003 2.003 3.003 4.003 5.003' and then exit.
 */

static uc_value_t *
Uptime(uc_vm_t *vm, size_t args)
{
	async_manager_t *manager = async_manager_get( vm );
	return ucv_double_new(uptime(manager));
}
#endif

static const uc_function_list_t local_async_fns[] = {
	{"Promise", Promise},
	{"PromiseAll", PromiseAll},
	{"PromiseAny", PromiseAny},
	{"PromiseRace", PromiseRace},
	{"PromiseAllSettled",PromiseAllSettled},
	{"throw", Throw},
	{"PumpEvents", PumpEvents},
	{"setTimeout", setTimeout},
	{"setPeriodic", setPeriodic},
	{"setImmediate", setImmediate},
	{"clearTimeout", clearTimeout},
#ifdef HAS_UPTIME
	{"uptime", Uptime},
#endif
};

void uc_module_init(uc_vm_t *vm, uc_value_t *scope)
{
	if( async_manager_get( vm ) )
	// Initializing twice?
		return;

	async_manager_t *manager = xalloc(sizeof(async_manager_t));
	uc_value_t *uv_manager = ucv_resource_new(NULL, manager);
	uc_vm_registry_set(vm, "async.manager", uv_manager);

	manager->header.event_pump = async_event_pump;
	manager->header.new_promise = _uc_async_new_promise;
	manager->header.resolve_reject = _uc_async_resolve_reject;
	manager->header.new_callback_queuer = _uc_async_new_callback_queuer;

	manager->vm = vm;

	/* promise initializing */
	manager->promise_type = uc_type_declare(vm, _strPromise, promise_type_fns, close_promise);
	manager->resolver_type = uc_type_declare(vm, _strResolver, resolver_type_fns, close_resolver);

	uc_function_list_register(scope, local_async_fns);

#ifdef HAS_UPTIME
	manager->start_time = async_timer_current_time();
#endif
}
