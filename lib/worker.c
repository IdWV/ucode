/*
 * Copyright (C) 2024 Isaac de Wolff <idewolff@gmx.com>
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
 * # Worker functions
 *
 * The `worker` module provides an interface to start scripts in concurrent threads.
 *
 * Functions can be individually imported and directly accessed using the
 * {@link https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Statements/import#named_import named import}
 * syntax:
 *
 *   ```
 *   import { Start, isWorker } from 'worker';
 *
 *   let thread = Start( {} );
 *   thread.join();
 *   ```
 *
 * Alternatively, the module namespace can be imported
 * using a wildcard import statement:
 *
 *   ```
 *   import * as worker from 'worker';
 *
 *   let thread = worker.Start( {} );
 *   thread.join();
 *   ```
 *
 * @module worker
 */

#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <pthread.h>

#include "ucode/module.h"
#include "ucode/platform.h"
#include "ucode/async.h"
#include "ucode/lib.h"
#include "ucode/compiler.h"

typedef enum
{
    typeNone = 0,
    typeCstr,
    typeUcValue,
    typeJsonObject,
} interscript_type_t;

enum
{
    contentNone = 0,
    contentSource,
    contentArgs,
    contentResolve,
    contentReject,

    contentBuffer,
    contentFilename,
};

enum
{
    workerNone = 0,
    workerPromise = 1,
    workerObject = 2,
};

typedef struct interscript
{
    union{
        uc_value_t *uc_value;
        json_object *json;
        const char *c_str;
    };
    uint32_t type:4;
    uint32_t content:4;
    uint32_t exit:8;

} interscript_t;

static void 
interscript_cleanup( interscript_t *is )
{
    switch( (interscript_type_t)is->type )
    {
        case typeNone:
            return;
        case typeUcValue:
            ucv_put( is->uc_value );
            break;
        case typeJsonObject:
            json_object_put( is->json );
            break;
        case typeCstr:
            break;
    }
    is->c_str = 0;
    is->type = typeNone;
}

static void
interscript_copy_value( interscript_t *is, uc_value_t *value )
{
    interscript_cleanup( is );

    // tagged pointer
    if( (uintptr_t)value & 3 )
    {
        is->uc_value = value;
        is->type = typeUcValue;
        return;
    }

    switch( ucv_type( value ) )
    {
        /* These are tagged pointers
        case UC_NULL:
            is->uc_value = 0;
            is->type = typeUcValue;
            return;
        case UC_BOOLEAN:
            is->uc_value = ucv_boolean_new( ucv_boolean_get( value ) );
            is->type = typeUcValue;
            return;
        */
        case UC_STRING:
            is->uc_value = ucv_string_new( ucv_string_get( value ) );
            is->type = typeUcValue;
            return;
        case UC_DOUBLE:
            is->uc_value = ucv_double_new( ucv_double_get( value ) );
            is->type = typeUcValue;
            return;
        case UC_INTEGER:
            is->uc_value = ucv_int64_new( ucv_int64_get( value ) );
            is->type = typeUcValue;
            return;
        default:
            is->json = ucv_to_json( value );
            is->type = typeJsonObject;
            return;
    }
}

static void
interscript_set_string( interscript_t *is, const char *value )
{
    interscript_cleanup( is );
    is->c_str = value;
    is->type = typeCstr;
}

static uc_value_t *
interscript_to_value( uc_vm_t *vm, interscript_t *is )
{
    switch( (interscript_type_t)is->type )
    {
        case typeCstr:
        {
            is->type = typeUcValue;
            return ucv_get( is->uc_value = ucv_string_new( is->c_str ) );
        }
        case typeJsonObject:
        {
            uc_value_t *value = ucv_from_json( vm, is->json );
            json_object_put( is->json );
            is->type = typeUcValue;
            return ucv_get( is->uc_value = value );
        }
        case typeUcValue:
        {
            return ucv_get( is->uc_value );
        }
        default:
            break;
    }
    return 0;
}

typedef struct uc_worker
{
    uc_vm_t vm;
    uc_parse_config_t config;

    int worker_type;
    int refcount;

    interscript_t exchange;

    struct{
        char *source;
        int content;
    } script;

    pthread_t tid;

        // Only used for Promise
    uc_async_promise_resolver_t *resolver;
    const uc_async_callback_queuer_t *queuer;

} uc_worker_t;

static uc_worker_t *
uc_worker_cast( uc_vm_t *vm )
{
    return (uc_worker_t *)vm;
}

static void
uc_worker_free( uc_worker_t *worker, bool join )
{
    if( join && worker->tid )
    {
        pthread_t tid = worker->tid;
        worker->tid = 0;
        void *ret;
        pthread_join( tid, &ret );
    }

	if( 0 != __atomic_add_fetch( &worker->refcount, -1, __ATOMIC_RELAXED) )
        return;

   	/* free search module path vector */
    uc_search_path_free(&worker->config.module_search_path);

    if( worker->script.source )
        free( worker->script.source );

    interscript_cleanup( &worker->exchange );
    if( worker->queuer )
        uc_async_callback_queuer_free( &worker->queuer );
    free( worker );
}

static int 
uc_worker_callback( uc_vm_t *vm, void *arg, int flags )
{
    uc_worker_t *worker = arg;
    if( flags & UC_ASYNC_CALLBACK_FLAG_EXECUTE )
    {
        uc_value_t *result = interscript_to_value( vm, &worker->exchange );
        if( worker->exchange.content == contentResolve )
            uc_async_promise_resolve( vm, &worker->resolver, result );
        else
            uc_async_promise_reject( vm, &worker->resolver, result );
        ucv_put( result );
    }
    if( flags & UC_ASYNC_CALLBACK_FLAG_CLEANUP )
    {
        uc_worker_free( worker, true );
    }
    return EXCEPTION_NONE;
}

static void
async_exception_clear( uc_worker_t *worker, uc_exception_t *exception)
{
	exception->type = EXCEPTION_NONE;

	ucv_put(exception->stacktrace);
	exception->stacktrace = NULL;

	free(exception->message);
	exception->message = NULL;
}

static void
uc_worker_exception(uc_vm_t *vm, uc_exception_t *ex)
{
    uc_worker_t *worker = uc_worker_cast( vm );
    if( EXCEPTION_EXIT == ex->type )
    {
        async_exception_clear( worker, ex );
        return;
    }

    uc_value_t *message = ucv_string_new( ex->message );
    interscript_copy_value( &worker->exchange, message );
    ucv_put( message );
    worker->exchange.content = contentReject;
    worker->exchange.exit = ex->type;
    async_exception_clear( worker, ex );
}

static void *
uc_worker_thread_fn( void *arg )
{
    uc_worker_t *worker = arg;

    uc_source_t *src  = 0;
    if( contentBuffer == worker->script.content )
    {
        /* create a source buffer containing the program code */
	    src = uc_source_new_buffer("worker", worker->script.source, strlen(worker->script.source) );
        worker->script.source = 0;
    }
    else if( contentFilename == worker->script.content )
    {
        src = uc_source_new_file( worker->script.source );
        if( 0 == src )
        {
            uc_stringbuf_t *buf = ucv_stringbuf_new();
            ucv_stringbuf_printf( buf, "Cannot open file '%s'\n", worker->script.source );
            interscript_copy_value( &worker->exchange, ucv_stringbuf_finish( buf ) );
            worker->exchange.content = contentReject;
            if( !uc_async_request_callback( worker->queuer, uc_worker_callback, worker ) )
                uc_worker_free( worker, false );
		    return 0;
        }
    }

	/* compile source buffer into function */
	char *syntax_error = NULL;
	uc_program_t *program = uc_compile(&worker->config, src, &syntax_error);

	/* release source buffer */
	uc_source_put(src);

	/* check if compilation failed */
	if (!program) 
    {
        uc_stringbuf_t *buf = ucv_stringbuf_new();
        ucv_stringbuf_printf( buf, "Failed to compile program: %s\n", syntax_error);
        interscript_copy_value( &worker->exchange, ucv_stringbuf_finish( buf ) );
        worker->exchange.content = contentReject;
        if( !uc_async_request_callback( worker->queuer, uc_worker_callback, worker ) )
            uc_worker_free( worker, false );
		return 0;
	}

    uc_vm_t *vm = &worker->vm;
	/* initialize VM context */
	uc_vm_init( vm, &worker->config);
//    vm->trace = 1;

	/* load standard library into global VM scope */
	uc_stdlib_load(uc_vm_scope_get( vm ));

    uc_vm_registry_set( vm, "worker.isWorker", ucv_int64_new( worker->worker_type ) );

	/* register custom exception handler */
	uc_vm_exception_handler_set( vm, uc_worker_exception );

	/* execute program function */
	int return_code = uc_vm_execute( vm, program, NULL);

	/* release program */
	uc_program_put(program);

	/* free VM context */
	uc_vm_free( vm);

    switch( worker->exchange.content )
    {
        case contentNone:
        case contentArgs:
            interscript_set_string( &worker->exchange, "Done" );
            worker->exchange.content = contentResolve;
            // fallthrough
        case contentReject:
        case contentResolve:
            if( workerPromise == worker->worker_type )
            {
                if( uc_async_request_callback( worker->queuer, uc_worker_callback, worker ) )
                {
                    return 0;
                }
            }
            uc_worker_free( worker, false );
    		return 0;
        default:
            break;
    }

	/* handle return status */
	if (return_code == ERROR_COMPILE || return_code == ERROR_RUNTIME) 
    {
        interscript_set_string( &worker->exchange, "An error occurred while running the program");
        worker->exchange.content = contentReject;
    }

    if( workerPromise == worker->worker_type )
    {
        if( uc_async_request_callback( worker->queuer, uc_worker_callback, worker ) )
        {
            return 0;
        }
    }
    uc_worker_free( worker, false );
    return 0;
}

static void 
close_object( void *obj )
{
    uc_worker_t *worker = obj;
    uc_worker_free( worker, true );
}

static uc_value_t *
uc_JoinCommon( uc_vm_t *vm, size_t nargs, bool try )
{
   	uc_worker_t **pworker = uc_fn_this( "worker.thread" );
    if( !pworker || !*pworker )
        return 0;

    uc_worker_t *worker = *pworker;
    if( worker->tid )
    {
        pthread_t tid = worker->tid;
        worker->tid = 0;
        void *ret;
        if( try )
        {
            if( 0 != pthread_tryjoin_np( tid, &ret ) )
            {
                worker->tid = tid;
                return 0;
            }
        }
        else 
            pthread_join( tid, &ret );
    }

    if( contentResolve == worker->exchange.content )
    {
        uc_value_t *ret = interscript_to_value( vm, &worker->exchange );
        if( try && 0 == ret )
            ret = ucv_string_new( "null" );
        return ret;
    }

    int exit = worker->exchange.exit;
    if( EXCEPTION_NONE == exit || EXCEPTION_EXIT == exit )
        exit = EXCEPTION_RUNTIME;
    const char *reason = "Worker ended abnormally";
    char *todelete = 0;
    switch( worker->exchange.type )
    {
        case typeCstr:
            reason = worker->exchange.c_str;
            break;
        case typeUcValue:
            reason = todelete = ucv_to_string( vm, worker->exchange.uc_value );
            break;
        case typeJsonObject:
            break;
    }

    uc_vm_raise_exception( vm, exit, reason, 0 );
    if( todelete )
        free( todelete );
    return 0;
}

static uc_value_t *
uc_Join( uc_vm_t *vm, size_t nargs )
{
    return uc_JoinCommon( vm, nargs, false );
}

static uc_value_t *
uc_TryJoin( uc_vm_t *vm, size_t nargs )
{
    return uc_JoinCommon( vm, nargs, true );
}


static const uc_function_list_t object_fns[] = {
	{ "join", uc_Join },
    { "tryjoin", uc_TryJoin }
};

static void
uc_compile_parse_config(uc_parse_config_t *config, uc_value_t *spec)
{
	uc_value_t *v, *p;
	size_t i, j;
	bool found;

	struct {
		const char *key;
		bool *flag;
		uc_search_path_t *path;
	} fields[] = {
		{ "lstrip_blocks",       &config->lstrip_blocks,       NULL },
		{ "trim_blocks",         &config->trim_blocks,         NULL },
		{ "strict_declarations", &config->strict_declarations, NULL },
		{ "raw_mode",            &config->raw_mode,            NULL },
		{ "module_search_path",  NULL, &config->module_search_path  },
		{ "force_dynlink_list",  NULL, &config->force_dynlink_list  }
	};

	for (i = 0; i < ARRAY_SIZE(fields); i++) {
		v = ucv_object_get(spec, fields[i].key, &found);

		if (!found)
			continue;

		if (fields[i].flag) {
			*fields[i].flag = ucv_is_truish(v);
		}
		else if (fields[i].path) {
			fields[i].path->count = 0;
			fields[i].path->entries = NULL;

			for (j = 0; j < ucv_array_length(v); j++) {
				p = ucv_array_get(v, j);

				if (ucv_type(p) != UC_STRING)
					continue;

				uc_vector_push(fields[i].path, ucv_string_get(p));
			}
		}
	}
}

static uc_source_t *
uc_GetScriptSource( uc_vm_t *vm )
{
    if( !vm->callframes.count ||
        !vm->callframes.entries->closure ||
        !vm->callframes.entries->closure->function ||
        !vm->callframes.entries->closure->function->program ||
        !vm->callframes.entries->closure->function->program->sources.count )
    {
        return 0;
    }

    uc_source_t *source = (*vm->callframes.entries->closure->function->program->sources.entries); 

    if( !source->filename && !source->buffer )
        return 0;
    
    return source;
}

/**
 * Represents a thread object as returned by
 * {@link module:worker#Start|Start()}
 *
 * @class module:worker.thread
 * @hideconstructor
 *
 * @implements join(), tryjoin()
 * 
 * join() blocks until the thread is terminated.
 * tryjoin() returns null if the thread is still running. 
 * When the thread actually returns a null, tryjoin() returns a string 
 * 'null' instead.
 * 
 * @example
 *
 * const thread = worker.Start();
 *
 * let result = thread.join();
 */

static uc_value_t *
uc_WorkerStart( uc_vm_t *vm, size_t nargs, int type )
{
    uc_worker_t *worker = xalloc( sizeof( uc_worker_t ) );
    worker->refcount = 1;
    worker->worker_type = type;

    uc_value_t *ret = 0;

    if( workerPromise == type )
    {
        ret = uc_async_promise_new( vm, &worker->resolver );
        if( !ret )
        {
            uc_worker_free( worker, false );
            uc_vm_raise_exception( vm, EXCEPTION_RUNTIME, "Cannot create promise" );
            return 0;
        }
        worker->queuer = uc_async_callback_queuer_new( vm );
    }
    else if( workerObject == type )
    {
        uc_resource_type_t *object = ucv_resource_type_lookup( vm, "worker.thread" );
        if( 0 == object )
        {
            object = uc_type_declare( vm, "worker.thread", object_fns, close_object );
        }
        worker->refcount++;
        ret = ucv_resource_new( object, worker );
    }

    interscript_copy_value( &worker->exchange, uc_fn_arg(0) );
    worker->exchange.content = contentArgs;

    uc_value_t *source = 0;
    uc_value_t *config = 0;

    if( nargs > 2 )
    {
        source = uc_fn_arg(1);
        config = uc_fn_arg(2);
    }
    else if( nargs > 1 ) 
    {
        source = uc_fn_arg(1);
        if( ucv_type(source) == UC_OBJECT )
        {
            config = source;
            source = 0;
        }
    }
    
    memcpy( &worker->config, vm->config, sizeof( worker->config ) );
    worker->config.module_search_path.count = 0;
    worker->config.module_search_path.entries = 0;
    worker->config.force_dynlink_list.count = 0;
    worker->config.force_dynlink_list.entries = 0;
    uc_vector_foreach( &vm->config->module_search_path, iter )
    {
        uc_search_path_add( &worker->config.module_search_path, *iter );
    }

    if( config )
    {
        uc_compile_parse_config( &worker->config, config );
    }    


    if( source )
    {
        char *strsource = ucv_to_string( vm, source );
        if( '/' != *strsource )
        {
            // No absolute path. Add the directory of the current script
            uc_source_t *source = uc_GetScriptSource( vm );
            if( source->runpath )
            {
                const char *slash = strrchr( source->runpath, '/' );
                if( slash )
                {
                    int dirlen = (slash - source->runpath) + 1;
                    char *newpath = xalloc( dirlen + strlen( strsource ) + 1 );
                    memcpy( newpath, source->runpath, dirlen );
                    strcpy( &newpath[ dirlen ], strsource );
                    free( strsource );
                    strsource = newpath;
                }
            }
        }
        worker->script.source = strsource;
        worker->script.content = contentFilename;
    }
    else
    {
        uc_source_t *source = uc_GetScriptSource( vm );
        static const char *_strCannotGetScriptSource = "Cannot get script source";

        if( 0 == source )
        {
            if( type == workerPromise )
            {
                uc_async_promise_reject( vm, &worker->resolver, 
                    ucv_string_new( _strCannotGetScriptSource ) );
                uc_worker_free( worker, false );
                return ret;
            }
            worker->refcount--;
            ucv_put( ret );
            uc_vm_raise_exception( vm, EXCEPTION_RUNTIME, _strCannotGetScriptSource, 0 );
            return 0;
        }

        if( source->buffer )
        {
            worker->script.source = xstrdup( source->buffer );
            worker->script.content = contentBuffer;
        }
        else 
        {
            worker->script.source = xstrdup( source->filename );
            worker->script.content = contentFilename;
        }
    }

    // start thread
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_create( &worker->tid,&attr,uc_worker_thread_fn,worker);
    pthread_attr_destroy(&attr);

    // and return promise or worker.thread
    return ret; 
}

/**
 * This function starts a worker promise script. It will raise an exception
 * if the async plugin is not loaded.
 *  
 * @function module:worker#Promise
 *
 * @param {Any} arg 
 * Argument to be given to the worker thread. Can be anything. 
 * Is passed to the other thread by means of a json object.
 * 
 * @param {String} [filename]
 * Filename of the script to be started. Is an absolute path, or relative to
 * the calling script. If omitted, the current script is restarted as worker.
 * 
 * @param {module:core.ParseConfig} [options]
 * The options for compilation.
 * 
 * @returns {module:async.promise}
 *
 * @example
 * worker.Promise( {}, './worker.uc' )
 * .then( (result)=>
 * {
 * })
 * .catch( (err)=>
 * {
 * });
 */

static uc_value_t *
uc_Promise( uc_vm_t *vm, size_t nargs )
{
    if( !uc_async_manager_get( vm ) )
    {
        uc_vm_raise_exception( vm, EXCEPTION_RUNTIME, "worker.Promise() needs async to be imported" );
        return 0;
    }
    return uc_WorkerStart( vm, nargs, workerPromise );
}

/**
 * This function starts a worker script.
 *  
 * @function module:worker#Start
 *
 * @param {Any} arg 
 * Argument to be given to the worker thread. Can be anything. 
 * Is passed to the other thread by means of a json object.
 * 
 * @param {String} [filename]
 * Filename of the script to be started. Is an absolute path, or relative to
 * the calling script. If omitted, the current script is restarted as worker.
 * 
 * @param {module:core.ParseConfig} [options]
 * The options for compilation.
 * 
 * @returns {?module:worker.thread}
 *
 * @example
 * let thread = worker.Start( {}, './worker.uc' );
 * 
 * // do some work
 * 
 * let ret = thread.join();
 */

static uc_value_t *
uc_Start( uc_vm_t *vm, size_t nargs )
{
    return uc_WorkerStart( vm, nargs, workerObject );
}

inline static int _isWorker( uc_vm_t *vm )
{
    uc_value_t *value = uc_vm_registry_get( vm, "worker.isWorker" );
    if( !value )
        return workerNone;
    return ucv_int64_get( value );
}

/**
 * This function returns true if the current script is a worker promise script.
 *  
 * @function module:worker#isPromise
 *
 * @return {Boolean}
 *
 * @example
 * if( worker.isPromise() )
 * {
 *    let obj = HeavyFunction();
 *    if( obj )
 *        worker.resolve( obj )
 *    worker.reject( 'failed' );
 * }
 * else
 * {
 *    worker.Promise( {} )
 *    .then( (obj)=>
 *    {
 *    })
 *    .catch( (err)=>
 *    {
 *    });
 * }
 * // The obj in the else{} is a copy of the obj in the if{}
 */

static uc_value_t *
uc_isPromise( uc_vm_t *vm, size_t nargs )
{
    bool ispromise = workerPromise == _isWorker( vm );
    return ucv_boolean_new( ispromise );
}

/**
 * This function returns true if the current script is a worker script.
 *  
 * @function module:worker#isWorker
 *
 * @return {Boolean}
 *
 * @example
 * if( worker.isWorker() )
 * {
 *   let obj = HeavyFunction();
 *   worker.return( obj );
 * }
 * else 
 * { 
 *    let thread = worker.Start( {} );
 *    let obj = thread.join();
 * }
 * // The obj in the else{} is a copy of the obj in the if{}
 */
static uc_value_t *
uc_isWorker( uc_vm_t *vm, size_t nargs )
{
    return ucv_boolean_new( workerNone != _isWorker( vm ) );
}

/**
 * This function is only available if {@link module:worker#isWorker|isWorker()} returns true.
 * It returns the value provided by the calling script.
 *  
 * @function module:worker#Args
 *
 * @return {Any}
 *
 * @example
 * if( worker.isWorker() )
 * {
 *   let args = worker.Args();
 *   if( args.filename )
 *   {
 *      // whatever
 *   }
 * }
 */

static uc_value_t *
uc_Args( uc_vm_t *vm, size_t nargs )
{
    uc_worker_t *worker = uc_worker_cast( vm );
    if( worker->exchange.content == contentArgs )
    {
        return interscript_to_value( vm, &worker->exchange );
    }
    return 0;
}

static uc_value_t *
uc_ResolveReject( uc_vm_t *vm, size_t nargs, int content )
{
    uc_worker_t *worker = uc_worker_cast( vm );
    uc_value_t *value = uc_fn_arg(0);
    interscript_copy_value( &worker->exchange, value );
    worker->exchange.content = content;

	vm->arg.s32 = (int32_t)0;
	uc_vm_raise_exception(vm, EXCEPTION_EXIT, " " );
    return 0;

}

/**
 * This function is only available if {@link module:worker#isPromise|isPromise()} returns true.
 * Takes any value, to be returned to the caller thread, and doesn't return.
 * 
 * @function module:worker#reject
 *
 * @param {Any} arg
 * Reject value to be given to the worker thread. Can be anything. 
 * Is passed to the other thread by means of a json object.
 *
 * @example
 * if( worker.isPromise() )
 * {
 *   // do some heavy work
 *   let obj = HeavyFunction();
 *   if( obj )
 *       worker.resolve( obj );
 *   worker.reject( 'failed' );
 * }
 */

static uc_value_t *
uc_Reject( uc_vm_t *vm, size_t nargs )
{
    return uc_ResolveReject( vm, nargs, contentReject );
}

/**
 * This function is only available if {@link module:worker#isPromise|isPromise()} returns true.
 * Takes any value, to be returned to the caller thread, and doesn't return.
 * 
 * @function module:worker#resolve
 *
 * @param {Any} arg
 * Return value to be given to the worker thread. Can be anything. 
 * Is passed to the other thread by means of a json object.
 *
 * @example
 * if( worker.isPromise() )
 * {
 *   // do some heavy work
 *   let obj = HeavyFunction();
 *   worker.resolve( obj );
 * }
 */

/**
 * This function is only available if {@link module:worker#isWorker|isWorker()} returns true.
 * Takes any value, to be returned to the caller thread, and doesn't return.
 * 
 * @function module:worker#return
 *
 * @param {Any} arg
 * Return value to be given to the worker thread. Can be anything. 
 * Is passed to the other thread by means of a json object.
 *
 * @example
 * if( worker.isWorker() )
 * {
 *   // do some heavy work
 *   let obj = HeavyFunction();
 *   worker.return( obj );
 * }
 */

static uc_value_t *
uc_Resolve( uc_vm_t *vm, size_t nargs )
{
    return uc_ResolveReject( vm, nargs, contentResolve );
}

static const uc_function_list_t global_fns[] = {
	{ "Promise", uc_Promise },
    { "Start", uc_Start },
    { "isWorker", uc_isWorker },
    { "isPromise", uc_isPromise }
};

static const uc_function_list_t worker_fns[] = {
    { "Args", uc_Args },
};

static const uc_function_list_t promise_fns[] = {
	{ "reject",	uc_Reject },
    { "resolve", uc_Resolve }
};

static const uc_function_list_t thread_fns[] = {
    { "return", uc_Resolve }
};

void uc_module_init(uc_vm_t *vm, uc_value_t *scope)
{
   	uc_function_list_register(scope, global_fns);
    int worker = _isWorker( vm );
    if( workerNone != worker )
    {
        uc_function_list_register( scope, worker_fns );
        if( workerPromise == worker )
        {
            uc_function_list_register( scope, promise_fns );
        }
        uc_function_list_register( scope, thread_fns );
    }
}
