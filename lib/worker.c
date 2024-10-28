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
#include "ucode/lib.h"
#include "ucode/compiler.h"

typedef enum
{
    typeNone = 0,
    typeCstr,
    typeStaticCstr,
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

typedef struct interscript
{
    union{
        uc_value_t *uc_value;
        json_object *json;
        char *c_str;
        const char *static_c_str;
    };
    uint32_t type:4;
    uint32_t content:4;

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
            free( is->c_str );
            break;
        case typeStaticCstr:
            break;
    }
    is->c_str = 0;
    is->type = typeNone;
}

static void
interscript_copy_value( interscript_t *is, uc_value_t *value )
{
    interscript_cleanup( is );
    if( 0 == value )
    {
        is->uc_value = 0;
        is->type = typeUcValue;
        return;
    }
    switch( value->type )
    {
        case UC_BOOLEAN:
            is->uc_value = ucv_boolean_new( ucv_boolean_get( value ) );
            is->type = typeUcValue;
            return;
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
        case UC_NULL:
            is->uc_value = 0;
            is->type = typeUcValue;
            return;
        default:
            is->json = ucv_to_json( value );
            is->type = typeJsonObject;
            return;
    }
}

static void
interscript_copy_string( interscript_t *is, const char *value )
{
    interscript_cleanup( is );
    is->c_str = strdup( value );
    is->type = typeCstr;
}

static void
interscript_copy_static_string( interscript_t *is, const char *value )
{
    interscript_cleanup( is );
    is->static_c_str = value;
    is->type = typeStaticCstr;
}

static uc_value_t *
interscript_to_value( uc_vm_t *vm, interscript_t *is )
{
    switch( (interscript_type_t)is->type )
    {
        case typeCstr:
        {
            uc_value_t *value = ucv_string_new( is->c_str );
            free( is->c_str );
            is->type = typeUcValue;
            return ucv_get( is->uc_value = value );
        }
        case typeStaticCstr:
        {
            is->type = typeUcValue;
            return ucv_get( is->uc_value = ucv_string_new( is->static_c_str ) );
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


struct uc_worker
{
    uc_async_promise_resolver_t *resolver;
    const uc_async_callback_queuer_t *queuer;

    interscript_t data;
    struct{
        char *source;
        int content;
    } run;

    pthread_t tid;
    uc_parse_config_t config;
};

struct worker_vm
{
    uc_vm_t vm;
    struct uc_worker *worker;
};

static struct uc_worker *
uc_worker_cast( uc_vm_t *vm )
{
    return ((struct worker_vm *)vm)->worker;
}

static void
uc_worker_free( struct uc_worker *worker, bool insync )
{
    if( worker->run.source )
        free( worker->run.source );
    interscript_cleanup( &worker->data );
    uc_async_callback_queuer_free( &worker->queuer );
    if( insync )
    {
        void *ret;
        pthread_join( worker->tid, &ret );
    }
    free( worker );
}

static int 
uc_worker_callback( uc_vm_t *vm, void *arg, int flags )
{
    struct uc_worker *worker = arg;
    if( flags & UC_ASYNC_CALLBACK_FLAG_EXECUTE )
    {
        uc_value_t *result = interscript_to_value( vm, &worker->data );
        if( worker->data.content == contentResolve )
            uc_async_promise_resolve( vm, &worker->resolver, result );
        else
            uc_async_promise_reject( vm, &worker->resolver, result );
    }
    if( flags & UC_ASYNC_CALLBACK_FLAG_CLEANUP )
    {
        uc_worker_free( worker, true );
    }
    return EXCEPTION_NONE;
}

static void *
uc_worker_thread( void *arg )
{
    struct uc_worker *worker = arg;

    uc_source_t *src  = 0;
    if( contentBuffer == worker->run.content )
    {
        /* create a source buffer containing the program code */
	    src = uc_source_new_buffer("worker", worker->run.source, strlen(worker->run.source) );
        worker->run.source = 0;
    }
    else if( contentFilename == worker->run.content )
    {
        src = uc_source_new_file( worker->run.source );
    }

	/* compile source buffer into function */
	char *syntax_error = NULL;
	uc_program_t *program = uc_compile(&worker->config, src, &syntax_error);

	/* release source buffer */
	uc_source_put(src);

	/* check if compilation failed */
	if (!program) 
    {
    	/* free search module path vector */
	    uc_search_path_free(&worker->config.module_search_path);

        char buffer[ 256 ];
        snprintf( buffer, 255, "Failed to compile program: %s\n", syntax_error);
        interscript_copy_string( &worker->data, buffer );
        worker->data.content = contentReject;
        if( !uc_async_request_callback( worker->queuer, uc_worker_callback, worker ) )
            uc_worker_free( worker, false );
		return 0;
	}

    struct worker_vm myvm;
    memset( &myvm, 0, sizeof( struct worker_vm ) );
    myvm.worker = worker;
    uc_vm_t *vm = &myvm.vm;
	/* initialize VM context */
	uc_vm_init( vm, &worker->config);

	/* load standard library into global VM scope */
	uc_stdlib_load(uc_vm_scope_get( vm ));

    uc_vm_registry_set( vm, "worker.isPromise", ucv_boolean_new( true ) );

	/* execute program function */
	int return_code = uc_vm_execute( vm, program, NULL);

	/* release program */
	uc_program_put(program);

	/* free VM context */
	uc_vm_free( vm);

   	/* free search module path vector */
    uc_search_path_free(&worker->config.module_search_path);

	/* handle return status */
	if (return_code == ERROR_COMPILE || return_code == ERROR_RUNTIME) 
    {
        interscript_copy_static_string( &worker->data, "An error occurred while running the program");
        worker->data.content = contentReject;
        if( !uc_async_request_callback( worker->queuer, uc_worker_callback, worker ) )
        {
            uc_worker_free( worker, false );
        }
		return 0;
	}

	/* free search module path vector */
	uc_search_path_free(&worker->config.module_search_path);

    if( contentNone == worker->data.content ||
        contentArgs == worker->data.content )
    {
        interscript_copy_static_string( &worker->data, "Done" );
        worker->data.content = contentResolve;
    }

    if( !uc_async_request_callback( worker->queuer, uc_worker_callback, worker ) )
    {
        uc_worker_free( worker, false );
    }
	return 0;
}

static uc_value_t *
uc_Promise( uc_vm_t *vm, size_t nargs )
{
    if( !uc_async_manager_get( vm ) )
    {
        uc_vm_raise_exception( vm, EXCEPTION_RUNTIME, "worker.Promise() needs async to be imported" );
        return 0;
    }


    struct uc_worker *worker = xalloc( sizeof( struct uc_worker ) );

    uc_value_t *promise = uc_async_promise_new( vm, &worker->resolver );
    if( !promise )
    {
        uc_worker_free( worker, false );
        uc_vm_raise_exception( vm, EXCEPTION_RUNTIME, "Cannot create promise" );
        return 0;
    }

    interscript_copy_value( &worker->data, uc_fn_arg(0) );
    worker->data.content = contentArgs;

    uc_source_t *source = 0;

    if( !vm->callframes.count ||
        !vm->callframes.entries->closure ||
        !vm->callframes.entries->closure->function ||
        !vm->callframes.entries->closure->function->program ||
        !vm->callframes.entries->closure->function->program->sources.count )
    {
        uc_async_promise_reject( vm, &worker->resolver, 
            ucv_string_new( "Cannot get script source" ) );
        uc_worker_free( worker, false );
        return promise;
    }
    else 
    {
        source = (*vm->callframes.entries->closure->function->program->sources.entries);
    }

    if( !source->filename && !source->buffer )
    {
        uc_async_promise_reject( vm, &worker->resolver, 
            ucv_string_new( "Cannot get script source" ) );
        uc_worker_free( worker, false );
        return promise;
    }

    if( source->buffer )
    {
        worker->run.source = xstrdup( source->buffer );
        worker->run.content = contentBuffer;
    }
    else 
    {
        worker->run.source = xstrdup( source->filename );
        worker->run.content = contentFilename;
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
    
    worker->queuer = uc_async_callback_queuer_new( vm );

    // start thread
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_create( &worker->tid,&attr,uc_worker_thread,worker);
    pthread_attr_destroy(&attr);

    // and return promise
    return promise; 
}

inline static bool _isPromise( uc_vm_t *vm )
{
    return uc_vm_registry_exists( vm, "worker.isPromise" );
}

static uc_value_t *
uc_isPromise( uc_vm_t *vm, size_t nargs )
{
    return ucv_boolean_new( _isPromise( vm ) );
}

static uc_value_t *
uc_Args( uc_vm_t *vm, size_t nargs )
{
    struct uc_worker *worker = uc_worker_cast( vm );
    if( worker->data.content == contentArgs )
    {
        return interscript_to_value( vm, &worker->data );
    }
    return 0;
}

static uc_value_t *
uc_ResolveReject( uc_vm_t *vm, size_t nargs, int content )
{
    struct uc_worker *worker = uc_worker_cast( vm );
    uc_value_t *value = uc_fn_arg(0);
    interscript_copy_value( &worker->data, value );
    worker->data.content = content;

	vm->arg.s32 = (int32_t)0;
	uc_vm_raise_exception(vm, EXCEPTION_EXIT, " " );
    return 0;

}

static uc_value_t *
uc_Reject( uc_vm_t *vm, size_t nargs )
{
    return uc_ResolveReject( vm, nargs, contentReject );
}

static uc_value_t *
uc_Resolve( uc_vm_t *vm, size_t nargs )
{
    return uc_ResolveReject( vm, nargs, contentResolve );
}

static const uc_function_list_t global_fns[] = {
	{ "Promise", uc_Promise },
    { "isPromise", uc_isPromise }
};

static const uc_function_list_t worker_fns[] = {
    { "Args", uc_Args }
};

static const uc_function_list_t promise_fns[] = {
	{ "reject",	uc_Reject },
    { "resolve", uc_Resolve }
};


void uc_module_init(uc_vm_t *vm, uc_value_t *scope)
{
   	uc_function_list_register(scope, global_fns);
    if( _isPromise( vm ) )
    {
        uc_function_list_register( scope, promise_fns );
        uc_function_list_register( scope, worker_fns );
    }
}
