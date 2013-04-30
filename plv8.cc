/*-------------------------------------------------------------------------
 *
 * plv8.cc : PL/v8 handler routines.
 *
 * Copyright (c) 2009-2012, the PLV8JS Development Group.
 *-------------------------------------------------------------------------
 */
#include "plv8.h"
#include <new>

extern "C" {
#define delete		delete_
#define namespace	namespace_
#define	typeid		typeid_
#define	typename	typename_
#define	using		using_

#if PG_VERSION_NUM >= 90300
#include "access/htup_details.h"
#endif
#include "access/xact.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/trigger.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#undef delete
#undef namespace
#undef typeid
#undef typename
#undef using

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(plv8_call_handler);
PG_FUNCTION_INFO_V1(plv8_call_validator);
PG_FUNCTION_INFO_V1(plcoffee_call_handler);
PG_FUNCTION_INFO_V1(plcoffee_call_validator);
PG_FUNCTION_INFO_V1(plls_call_handler);
PG_FUNCTION_INFO_V1(plls_call_validator);

Datum	plv8_call_handler(PG_FUNCTION_ARGS) throw();
Datum	plv8_call_validator(PG_FUNCTION_ARGS) throw();
Datum	plcoffee_call_handler(PG_FUNCTION_ARGS) throw();
Datum	plcoffee_call_validator(PG_FUNCTION_ARGS) throw();
Datum	plls_call_handler(PG_FUNCTION_ARGS) throw();
Datum	plls_call_validator(PG_FUNCTION_ARGS) throw();

void _PG_init(void);

#if PG_VERSION_NUM >= 90000
PG_FUNCTION_INFO_V1(plv8_inline_handler);
PG_FUNCTION_INFO_V1(plcoffee_inline_handler);
PG_FUNCTION_INFO_V1(plls_inline_handler);
Datum	plv8_inline_handler(PG_FUNCTION_ARGS) throw();
Datum	plcoffee_inline_handler(PG_FUNCTION_ARGS) throw();
Datum	plls_inline_handler(PG_FUNCTION_ARGS) throw();
#endif
} // extern "C"

using namespace v8;

typedef struct plv8_proc_cache
{
	Oid						fn_oid;

	Persistent<Function>	function;
	char					proname[NAMEDATALEN];
	char				   *prosrc;

	TransactionId			fn_xmin;
	ItemPointerData			fn_tid;
	Oid						user_id;

	int						nargs;
	bool					retset;		/* true if SRF */
	Oid						rettype;
	Oid						argtypes[FUNC_MAX_ARGS];
} plv8_proc_cache;

/*
 * The function and context are created at the first invocation.  Their
 * lifetime is same as plv8_proc, but they are not palloc'ed memory,
 * so we need to clear them at the end of transaction.
 */
typedef struct plv8_exec_env
{
	Persistent<Object>		recv;
	Persistent<Context>		context;
	struct plv8_exec_env   *next;
} plv8_exec_env;

/*
 * We cannot cache plv8_type inter executions because it has FmgrInfo fields.
 * So, we cache rettype and argtype in fn_extra only during one execution.
 */
typedef struct plv8_proc
{
	plv8_proc_cache		   *cache;
	plv8_exec_env		   *xenv;
	TypeFuncClass			functypclass;			/* For SRF */
	plv8_type				rettype;
	plv8_type				argtypes[FUNC_MAX_ARGS];
} plv8_proc;

/*
 * For the security reasons, the global context is separated
 * between users and it's associated with user id.
 */
typedef struct plv8_context
{
	Persistent<Context>		context;
	Oid						user_id;
} plv8_context;

static HTAB *plv8_proc_cache_hash = NULL;

static plv8_exec_env		   *exec_env_head = NULL;

extern const unsigned char coffee_script_binary_data[];
extern const unsigned char livescript_binary_data[];

/*
 * lower_case_functions are postgres-like C functions.
 * They could raise errors with elog/ereport(ERROR).
 */
static plv8_proc *plv8_get_proc(Oid fn_oid, FunctionCallInfo fcinfo,
		bool validate, char ***argnames) throw();
static void plv8_xact_cb(XactEvent event, void *arg);

/*
 * CamelCaseFunctions are C++ functions.
 * They could raise errors with C++ throw statements, or never throw exceptions.
 */
static plv8_exec_env *CreateExecEnv(Handle<Function> script);
static plv8_proc *Compile(Oid fn_oid, FunctionCallInfo fcinfo,
					bool validate, bool is_trigger, Dialect dialect);
static Local<Function> CompileFunction(const char *proname, int proarglen,
					const char *proargs[], const char *prosrc,
					bool is_trigger, bool retset, Dialect dialect);
static Datum CallFunction(PG_FUNCTION_ARGS, plv8_exec_env *xenv,
		int nargs, plv8_type argtypes[], plv8_type *rettype);
static Datum CallSRFunction(PG_FUNCTION_ARGS, plv8_exec_env *xenv,
		int nargs, plv8_type argtypes[], plv8_type *rettype);
static Datum CallTrigger(PG_FUNCTION_ARGS, plv8_exec_env *xenv);
static Persistent<Context> GetGlobalContext();
static Persistent<ObjectTemplate> GetGlobalObjectTemplate();

/* A GUC to specify a custom start up function to call */
static char *plv8_start_proc = NULL;

/* A GUC to specify the remote debugger port */
static int plv8_debugger_port;
/*
 * We use vector instead of hash since the size of this array
 * is expected to be short in most cases.
 */
static std::vector<plv8_context *> ContextVector;

#ifdef ENABLE_DEBUGGER_SUPPORT
v8::Persistent<v8::Context> debug_message_context;

void DispatchDebugMessages() {
  // We are in some random thread. We should already have v8::Locker acquired
  // (we requested this when registered this callback). We was called
  // because new debug messages arrived; they may have already been processed,
  // but we shouldn't worry about this.
  //
  // All we have to do is to set context and call ProcessDebugMessages.
  //
  // We should decide which V8 context to use here. This is important for
  // "evaluate" command, because it must be executed some context.
  // In our sample we have only one context, so there is nothing really to
  // think about.
  v8::Context::Scope scope(debug_message_context);

  v8::Debug::ProcessDebugMessages();
}
#endif  // ENABLE_DEBUGGER_SUPPORT

void
_PG_init(void)
{
	HASHCTL    hash_ctl = { 0 };
	
	hash_ctl.keysize = sizeof(Oid);
	hash_ctl.entrysize = sizeof(plv8_proc_cache);
	hash_ctl.hash = oid_hash;
	plv8_proc_cache_hash = hash_create("PLv8 Procedures", 32,
									   &hash_ctl, HASH_ELEM | HASH_FUNCTION);

	DefineCustomStringVariable("plv8.start_proc",
							   gettext_noop("PLV8 function to run once when PLV8 is first used."),
							   NULL,
							   &plv8_start_proc,
							   NULL,
							   PGC_USERSET, 0,
#if PG_VERSION_NUM >= 90100
							   NULL,
#endif
							   NULL,
							   NULL);

	DefineCustomIntVariable("plv8.debugger_port",
							gettext_noop("V8 remote debug port."),
							gettext_noop("The default value is 35432.  "
										 "This is effective only if PLV8 is built with ENABLE_DEBUGGER_SUPPORT."),
							&plv8_debugger_port,
							35432, 0, 65536,
							PGC_USERSET, 0,
#if PG_VERSION_NUM >= 90100
							NULL,
#endif
							NULL,
							NULL);

	RegisterXactCallback(plv8_xact_cb, NULL);

	EmitWarningsOnPlaceholders("plv8");
}

static void
plv8_xact_cb(XactEvent event, void *arg)
{
	plv8_exec_env	   *env = exec_env_head;

	while (env)
	{
		if (!env->recv.IsEmpty())
		{
			env->recv.Dispose();
			env->recv.Clear();
		}
		env = env->next;
		/*
		 * Each item was allocated in TopTransactionContext, so
		 * it will be freed eventually.
		 */
	}
	exec_env_head = NULL;
}

static inline plv8_exec_env *
plv8_new_exec_env()
{
	plv8_exec_env	   *xenv = (plv8_exec_env *)
		MemoryContextAllocZero(TopTransactionContext, sizeof(plv8_exec_env));

	new(&xenv->context) Persistent<Context>();
	new(&xenv->recv) Persistent<Object>();

	/*
	 * Add it to the list, which will be freed in the end of top transaction.
	 */
	xenv->next = exec_env_head;
	exec_env_head = xenv;

	return xenv;
}

static Datum
common_pl_call_handler(PG_FUNCTION_ARGS, Dialect dialect) throw()
{
	Oid		fn_oid = fcinfo->flinfo->fn_oid;
	bool	is_trigger = CALLED_AS_TRIGGER(fcinfo);

	try
	{
#ifdef ENABLE_DEBUGGER_SUPPORT
		Locker				lock;
#endif  // ENABLE_DEBUGGER_SUPPORT
		HandleScope	handle_scope;

		if (!fcinfo->flinfo->fn_extra)
		{
			plv8_proc	   *proc = Compile(fn_oid, fcinfo,
										   false, is_trigger, dialect);
			proc->xenv = CreateExecEnv(proc->cache->function);
			fcinfo->flinfo->fn_extra = proc;
		}

		plv8_proc *proc = (plv8_proc *) fcinfo->flinfo->fn_extra;
		plv8_proc_cache *cache = proc->cache;

		if (is_trigger)
			return CallTrigger(fcinfo, proc->xenv);
		else if (cache->retset)
			return CallSRFunction(fcinfo, proc->xenv,
						cache->nargs, proc->argtypes, &proc->rettype);
		else
			return CallFunction(fcinfo, proc->xenv,
						cache->nargs, proc->argtypes, &proc->rettype);
	}
	catch (js_error& e)	{ e.rethrow(); }
	catch (pg_error& e)	{ e.rethrow(); }

	return (Datum) 0;	// keep compiler quiet
}

Datum
plv8_call_handler(PG_FUNCTION_ARGS) throw()
{
	return common_pl_call_handler(fcinfo, PLV8_DIALECT_NONE);
}

Datum
plcoffee_call_handler(PG_FUNCTION_ARGS) throw()
{
	return common_pl_call_handler(fcinfo, PLV8_DIALECT_COFFEE);
}

Datum
plls_call_handler(PG_FUNCTION_ARGS) throw()
{
	return common_pl_call_handler(fcinfo, PLV8_DIALECT_LIVESCRIPT);
}

#if PG_VERSION_NUM >= 90000
static Datum
common_pl_inline_handler(PG_FUNCTION_ARGS, Dialect dialect) throw()
{
	InlineCodeBlock *codeblock = (InlineCodeBlock *) DatumGetPointer(PG_GETARG_DATUM(0));

	Assert(IsA(codeblock, InlineCodeBlock));

	try
	{
#ifdef ENABLE_DEBUGGER_SUPPORT
		Locker				lock;
#endif  // ENABLE_DEBUGGER_SUPPORT
		HandleScope			handle_scope;
		char			   *source_text = codeblock->source_text;

		Handle<Function>	function = CompileFunction(NULL, 0, NULL,
										source_text, false, false, dialect);
		plv8_exec_env	   *xenv = CreateExecEnv(function);
		return CallFunction(fcinfo, xenv, 0, NULL, NULL);
	}
	catch (js_error& e)	{ e.rethrow(); }
	catch (pg_error& e)	{ e.rethrow(); }

	return (Datum) 0;	// keep compiler quiet
}

Datum
plv8_inline_handler(PG_FUNCTION_ARGS) throw()
{
	return common_pl_inline_handler(fcinfo, PLV8_DIALECT_NONE);
}

Datum
plcoffee_inline_handler(PG_FUNCTION_ARGS) throw()
{
	return common_pl_inline_handler(fcinfo, PLV8_DIALECT_COFFEE);
}

Datum
plls_inline_handler(PG_FUNCTION_ARGS) throw()
{
	return common_pl_inline_handler(fcinfo, PLV8_DIALECT_LIVESCRIPT);
}
#endif

/*
 * DoCall -- Call a JS function with SPI support.
 *
 * This function could throw C++ exceptions, but must not throw PG exceptions.
 */
static Local<v8::Value>
DoCall(Handle<Function> fn, Handle<Object> receiver,
	int nargs, Handle<v8::Value> args[])
{
	TryCatch		try_catch;

	if (SPI_connect() != SPI_OK_CONNECT)
		throw js_error("could not connect to SPI manager");
	Local<v8::Value> result = fn->Call(receiver, nargs, args);
	int	status = SPI_finish();

	if (result.IsEmpty())
		throw js_error(try_catch);

	if (status < 0)
		throw js_error(FormatSPIStatus(status));

	return result;
}

static Datum
CallFunction(PG_FUNCTION_ARGS, plv8_exec_env *xenv,
	int nargs, plv8_type argtypes[], plv8_type *rettype)
{
	Handle<Context>		context = xenv->context;
	Context::Scope		context_scope(context);
	Handle<v8::Value>	args[FUNC_MAX_ARGS];
	Handle<Object>		plv8obj;

	WindowFunctionSupport support(context, fcinfo);

	/*
	 * In window function case, we cannot see the argument datum
	 * in fcinfo.  Instead, get them by WinGetFuncArgCurrent().
	 */
	if (support.IsWindowCall())
	{
		WindowObject winobj = support.GetWindowObject();
		for (int i = 0; i < nargs; i++)
		{
			bool isnull;
			Datum arg = WinGetFuncArgCurrent(winobj, i, &isnull);
			args[i] = ToValue(arg, isnull, &argtypes[i]);
		}
	}
	else
	{
		for (int i = 0; i < nargs; i++)
			args[i] = ToValue(fcinfo->arg[i], fcinfo->argnull[i], &argtypes[i]);
	}

	Local<Function>		fn =
		Local<Function>::Cast(xenv->recv->GetInternalField(0));
	Local<v8::Value> result =
		DoCall(fn, xenv->recv, nargs, args);

	if (rettype)
		return ToDatum(result, &fcinfo->isnull, rettype);
	else
		PG_RETURN_VOID();
}

static Tuplestorestate *
CreateTupleStore(PG_FUNCTION_ARGS, TupleDesc *tupdesc)
{
	Tuplestorestate	   *tupstore;

	PG_TRY();
	{
		ReturnSetInfo  *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
		MemoryContext	per_query_ctx;
		MemoryContext	oldcontext;
		plv8_proc	   *proc = (plv8_proc *) fcinfo->flinfo->fn_extra;

		/* check to see if caller supports us returning a tuplestore */
		if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("set-valued function called in context that cannot accept a set")));
		if (!(rsinfo->allowedModes & SFRM_Materialize))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("materialize mode required, but it is not " \
							"allowed in this context")));

		if (!proc->functypclass)
			proc->functypclass = get_call_result_type(fcinfo, NULL, NULL);

		per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
		oldcontext = MemoryContextSwitchTo(per_query_ctx);

		tupstore = tuplestore_begin_heap(true, false, work_mem);
		rsinfo->returnMode = SFRM_Materialize;
		rsinfo->setResult = tupstore;
		/* Build a tuple descriptor for our result type */
		if (proc->rettype.typid == RECORDOID)
		{
			if (proc->functypclass != TYPEFUNC_COMPOSITE)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("function returning record called in context "
								"that cannot accept type record")));
		}
		if (!rsinfo->setDesc)
		{
			*tupdesc = CreateTupleDescCopy(rsinfo->expectedDesc);
			rsinfo->setDesc = *tupdesc;
		}
		else
			*tupdesc = rsinfo->setDesc;

		MemoryContextSwitchTo(oldcontext);
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();

	return tupstore;
}

static Datum
CallSRFunction(PG_FUNCTION_ARGS, plv8_exec_env *xenv,
	int nargs, plv8_type argtypes[], plv8_type *rettype)
{
	plv8_proc		   *proc = (plv8_proc *) fcinfo->flinfo->fn_extra;
	TupleDesc			tupdesc;
	Tuplestorestate	   *tupstore;

	tupstore = CreateTupleStore(fcinfo, &tupdesc);

	Handle<Context>		context = xenv->context;
	Context::Scope		context_scope(context);
	Converter			conv(tupdesc, proc->functypclass == TYPEFUNC_SCALAR);
	Handle<v8::Value>	args[FUNC_MAX_ARGS + 1];

	/*
	 * In case this is nested via SPI, stash pre-registered converters
	 * for the previous SRF.
	 */
	SRFSupport support(context, &conv, tupstore);

	for (int i = 0; i < nargs; i++)
		args[i] = ToValue(fcinfo->arg[i], fcinfo->argnull[i], &argtypes[i]);

	Local<Function>		fn =
		Local<Function>::Cast(xenv->recv->GetInternalField(0));

	Handle<v8::Value> result = DoCall(fn, xenv->recv, nargs, args);

	if (result->IsUndefined())
	{
		// no additional values
	}
	else if (result->IsArray())
	{
		Handle<Array> array = Handle<Array>::Cast(result);
		// return an array of records.
		int	length = array->Length();
		for (int i = 0; i < length; i++)
			conv.ToDatum(array->Get(i), tupstore);
	}
	else
	{
		// return a record or a scalar
		conv.ToDatum(result, tupstore);
	}

	/* clean up and return the tuplestore */
	tuplestore_donestoring(tupstore);

	return (Datum) 0;
}

static Datum
CallTrigger(PG_FUNCTION_ARGS, plv8_exec_env *xenv)
{
	// trigger arguments are:
	//	0: NEW
	//	1: OLD
	//	2: TG_NAME
	//	3: TG_WHEN
	//	4: TG_LEVEL
	//	5: TG_OP
	//	6: TG_RELID
	//	7: TG_TABLE_NAME
	//	8: TG_TABLE_SCHEMA
	//	9: TG_ARGV
	TriggerData		   *trig = (TriggerData *) fcinfo->context;
	Relation			rel = trig->tg_relation;
	TriggerEvent		event = trig->tg_event;
	Handle<v8::Value>	args[10];
	Datum				result = (Datum) 0;

	Handle<Context>		context = xenv->context;
	Context::Scope		context_scope(context);

	if (TRIGGER_FIRED_FOR_ROW(event))
	{
		TupleDesc		tupdesc = RelationGetDescr(rel);
		Converter		conv(tupdesc);

		if (TRIGGER_FIRED_BY_INSERT(event))
		{
			result = PointerGetDatum(trig->tg_trigtuple);
			// NEW
			args[0] = conv.ToValue(trig->tg_trigtuple);
			// OLD
			args[1] = Undefined();
		}
		else if (TRIGGER_FIRED_BY_DELETE(event))
		{
			result = PointerGetDatum(trig->tg_trigtuple);
			// NEW
			args[0] = Undefined();
			// OLD
			args[1] = conv.ToValue(trig->tg_trigtuple);
		}
		else if (TRIGGER_FIRED_BY_UPDATE(event))
		{
			result = PointerGetDatum(trig->tg_newtuple);
			// NEW
			args[0] = conv.ToValue(trig->tg_newtuple);
			// OLD
			args[1] = conv.ToValue(trig->tg_trigtuple);
		}
	}
	else
	{
		args[0] = args[1] = Undefined();
	}

	// 2: TG_NAME
	args[2] = ToString(trig->tg_trigger->tgname);

	// 3: TG_WHEN
	if (TRIGGER_FIRED_BEFORE(event))
		args[3] = String::New("BEFORE");
	else
		args[3] = String::New("AFTER");

	// 4: TG_LEVEL
	if (TRIGGER_FIRED_FOR_ROW(event))
		args[4] = String::New("ROW");
	else
		args[4] = String::New("STATEMENT");

	// 5: TG_OP
	if (TRIGGER_FIRED_BY_INSERT(event))
		args[5] = String::New("INSERT");
	else if (TRIGGER_FIRED_BY_DELETE(event))
		args[5] = String::New("DELETE");
	else if (TRIGGER_FIRED_BY_UPDATE(event))
		args[5] = String::New("UPDATE");
#ifdef TRIGGER_FIRED_BY_TRUNCATE
	else if (TRIGGER_FIRED_BY_TRUNCATE(event))
		args[5] = String::New("TRUNCATE");
#endif
	else
		args[5] = String::New("?");

	// 6: TG_RELID
	args[6] = Uint32::New(RelationGetRelid(rel));

	// 7: TG_TABLE_NAME
	args[7] = ToString(RelationGetRelationName(rel));

	// 8: TG_TABLE_SCHEMA
	args[8] = ToString(get_namespace_name(RelationGetNamespace(rel)));

	// 9: TG_ARGV
	Handle<Array> tgargs = Array::New(trig->tg_trigger->tgnargs);
	for (int i = 0; i < trig->tg_trigger->tgnargs; i++)
		tgargs->Set(i, ToString(trig->tg_trigger->tgargs[i]));
	args[9] = tgargs;

	TryCatch			try_catch;
	Local<Function>		fn =
		Local<Function>::Cast(xenv->recv->GetInternalField(0));
	Handle<v8::Value> newtup =
		DoCall(fn, xenv->recv, lengthof(args), args);

	if (newtup.IsEmpty())
		throw js_error(try_catch);

	/*
	 * If the function specifically returned null, return NULL to
	 * tell executor to skip the operation.  Otherwise, the function
	 * result is the tuple to be returned.
	 */
	if (newtup->IsNull() || !TRIGGER_FIRED_FOR_ROW(event))
	{
		result = PointerGetDatum(NULL);
	}
	else if (!newtup->IsUndefined())
	{
		TupleDesc		tupdesc = RelationGetDescr(rel);
		Converter		conv(tupdesc);
		HeapTupleHeader	header;

		header = DatumGetHeapTupleHeader(conv.ToDatum(newtup));

		/* We know it's there; heap_form_tuple stores with this layout. */
		result = PointerGetDatum((char *) header - HEAPTUPLESIZE);
	}

	return result;
}

static Datum
common_pl_call_validator(PG_FUNCTION_ARGS, Dialect dialect) throw()
{
	Oid				fn_oid = PG_GETARG_OID(0);
	HeapTuple		tuple;
	Form_pg_proc	proc;
	char			functyptype;
	bool			is_trigger = false;

	/* Get the new function's pg_proc entry */
	tuple = SearchSysCache(PROCOID, ObjectIdGetDatum(fn_oid), 0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for function %u", fn_oid);
	proc = (Form_pg_proc) GETSTRUCT(tuple);

	functyptype = get_typtype(proc->prorettype);

	/* Disallow pseudotype result */
	/* except for TRIGGER, RECORD, INTERNAL, VOID or polymorphic types */
	if (functyptype == TYPTYPE_PSEUDO)
	{
		/* we assume OPAQUE with no arguments means a trigger */
		if (proc->prorettype == TRIGGEROID ||
			(proc->prorettype == OPAQUEOID && proc->pronargs == 0))
			is_trigger = true;
		else if (proc->prorettype != RECORDOID &&
			proc->prorettype != VOIDOID &&
			proc->prorettype != INTERNALOID &&
			!IsPolymorphicType(proc->prorettype))
			ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("PL/v8 functions cannot return type %s",
						format_type_be(proc->prorettype))));
	}

	ReleaseSysCache(tuple);

	try
	{
#ifdef ENABLE_DEBUGGER_SUPPORT
		Locker				lock;
#endif  // ENABLE_DEBUGGER_SUPPORT
		/* Don't use validator's fcinfo */
		plv8_proc	   *proc = Compile(fn_oid, NULL,
									   true, is_trigger, dialect);
		(void) CreateExecEnv(proc->cache->function);
		/* the result of a validator is ignored */
		PG_RETURN_VOID();
	}
	catch (js_error& e)	{ e.rethrow(); }
	catch (pg_error& e)	{ e.rethrow(); }

	return (Datum) 0;	// keep compiler quiet
}

Datum
plv8_call_validator(PG_FUNCTION_ARGS) throw()
{
	return common_pl_call_validator(fcinfo, PLV8_DIALECT_NONE);
}

Datum
plcoffee_call_validator(PG_FUNCTION_ARGS) throw()
{
	return common_pl_call_validator(fcinfo, PLV8_DIALECT_COFFEE);
}

Datum
plls_call_validator(PG_FUNCTION_ARGS) throw()
{
	return common_pl_call_validator(fcinfo, PLV8_DIALECT_LIVESCRIPT);
}

static plv8_proc *
plv8_get_proc(Oid fn_oid, FunctionCallInfo fcinfo, bool validate, char ***argnames) throw()
{
	HeapTuple			procTup;
	plv8_proc_cache	   *cache;
	bool				found;
	bool				isnull;
	Datum				prosrc;
	Oid				   *argtypes;
	char			   *argmodes;
	MemoryContext		oldcontext;

	procTup = SearchSysCache(PROCOID, ObjectIdGetDatum(fn_oid), 0, 0, 0);
	if (!HeapTupleIsValid(procTup))
		elog(ERROR, "cache lookup failed for function %u", fn_oid);

	cache = (plv8_proc_cache *)
		hash_search(plv8_proc_cache_hash,&fn_oid, HASH_ENTER, &found);

	if (found)
	{
		bool	uptodate;

		/*
		 * We need to check user id and dispose it if it's different from
		 * the previous cache user id, as the V8 function is associated
		 * with the context where it was generated.  In most cases,
		 * we can expect this doesn't affect runtime performance.
		 */
		uptodate = (!cache->function.IsEmpty() &&
			cache->fn_xmin == HeapTupleHeaderGetXmin(procTup->t_data) &&
			ItemPointerEquals(&cache->fn_tid, &procTup->t_self) &&
			cache->user_id == GetUserId());

		if (!uptodate)
		{
			if (cache->prosrc)
			{
				pfree(cache->prosrc);
				cache->prosrc = NULL;
			}
			cache->function.Dispose();
			cache->function.Clear();
		}
		else
		{
			ReleaseSysCache(procTup);
		}
	}
	else
	{
		new(&cache->function) Persistent<Function>();
		cache->prosrc = NULL;
	}

	if (cache->function.IsEmpty())
	{
		Form_pg_proc	procStruct;

		procStruct = (Form_pg_proc) GETSTRUCT(procTup);

		prosrc = SysCacheGetAttr(PROCOID, procTup, Anum_pg_proc_prosrc, &isnull);
		if (isnull)
			elog(ERROR, "null prosrc");

		cache->retset = procStruct->proretset;
		cache->rettype = procStruct->prorettype;

		strlcpy(cache->proname, NameStr(procStruct->proname), NAMEDATALEN);
		cache->fn_xmin = HeapTupleHeaderGetXmin(procTup->t_data);
		cache->fn_tid = procTup->t_self;
		cache->user_id = GetUserId();

		int nargs = get_func_arg_info(procTup, &argtypes, argnames, &argmodes);

		if (validate)
		{
			/*
			 * Disallow non-polymorphic pseudotypes in arguments
			 * (either IN or OUT).  Internal type is used to declare
			 * js functions for find_function().
			 */
			for (int i = 0; i < nargs; i++)
			{
				if (get_typtype(argtypes[i]) == TYPTYPE_PSEUDO &&
						argtypes[i] != INTERNALOID &&
						!IsPolymorphicType(argtypes[i]))
					ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("PL/v8 functions cannot accept type %s",
								format_type_be(argtypes[i]))));
			}
		}

		oldcontext = MemoryContextSwitchTo(TopMemoryContext);
		cache->prosrc = TextDatumGetCString(prosrc);
		MemoryContextSwitchTo(oldcontext);

		ReleaseSysCache(procTup);

		int	inargs = 0;
		for (int i = 0; i < nargs; i++)
		{
			Oid		argtype = argtypes[i];
			char	argmode = argmodes ? argmodes[i] : PROARGMODE_IN;

			switch (argmode)
			{
			case PROARGMODE_IN:
			case PROARGMODE_INOUT:
			case PROARGMODE_VARIADIC:
				break;
			default:
				continue;
			}

			if (*argnames)
				(*argnames)[inargs] = (*argnames)[i];
			cache->argtypes[inargs] = argtype;
			inargs++;
		}
		cache->nargs = inargs;
	}

	MemoryContext mcxt = CurrentMemoryContext;
	if (fcinfo)
		mcxt = fcinfo->flinfo->fn_mcxt;

	plv8_proc *proc = (plv8_proc *) MemoryContextAllocZero(mcxt,
		offsetof(plv8_proc, argtypes) + sizeof(plv8_type) * cache->nargs);

	proc->cache = cache;
	for (int i = 0; i < cache->nargs; i++)
	{
		Oid		argtype = cache->argtypes[i];
		/* Resolve polymorphic types, if this is an actual call context. */
		if (fcinfo && IsPolymorphicType(argtype))
			argtype = get_fn_expr_argtype(fcinfo->flinfo, i);
		plv8_fill_type(&proc->argtypes[i], argtype, mcxt);
	}

	Oid		rettype = cache->rettype;
	/* Resolve polymorphic return type if this is an actual call context. */
	if (fcinfo && IsPolymorphicType(rettype))
		rettype = get_fn_expr_rettype(fcinfo->flinfo);
	plv8_fill_type(&proc->rettype, rettype, mcxt);

	return proc;
}

static plv8_exec_env *
CreateExecEnv(Handle<Function> function)
{
	plv8_exec_env	   *xenv;
	HandleScope			handle_scope;

	PG_TRY();
	{
		xenv = plv8_new_exec_env();
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();

	xenv->context = GetGlobalContext();
	Context::Scope		scope(xenv->context);

	static Persistent<ObjectTemplate> recv_templ;
	if (recv_templ.IsEmpty())
	{
		recv_templ = Persistent<ObjectTemplate>::New(ObjectTemplate::New());
		recv_templ->SetInternalFieldCount(1);
	}
	xenv->recv = Persistent<Object>::New(recv_templ->NewInstance());

	xenv->recv->SetInternalField(0, function);

	return xenv;
}

/* Source transformation from a dialect (coffee or ls) to js */
static char *
CompileDialect(const char *src, Dialect dialect)
{
	HandleScope		handle_scope;
	static Persistent<Context>	context = Context::New(NULL);
	Context::Scope	context_scope(context);
	TryCatch		try_catch;
	Local<String>	key;
	char		   *cresult;
	const char	   *dialect_binary_data;

	switch (dialect)
	{
		case PLV8_DIALECT_COFFEE:
			if (coffee_script_binary_data[0] == '\0')
				throw js_error("CoffeeScript is not enabled");
			key = String::NewSymbol("CoffeeScript");
			dialect_binary_data = (const char *) coffee_script_binary_data;
			break;
		case PLV8_DIALECT_LIVESCRIPT:
			if (livescript_binary_data[0] == '\0')
				throw js_error("LiveScript is not enabled");
			key = String::NewSymbol("LiveScript");
			dialect_binary_data = (const char *) livescript_binary_data;
			break;
		default:
			throw js_error("Unknown Dialect");
	}

	if (context->Global()->Get(key)->IsUndefined())
	{
		HandleScope		handle_scope;
		Local<Script>	script =
			Script::New(ToString(dialect_binary_data), key);
		if (script.IsEmpty())
			throw js_error(try_catch);
		Local<v8::Value>	result = script->Run();
		if (result.IsEmpty())
			throw js_error(try_catch);
	}

	Local<Object>	compiler = Local<Object>::Cast(context->Global()->Get(key));
	Local<Function>	func = Local<Function>::Cast(
			compiler->Get(String::NewSymbol("compile")));
	const int		nargs = 1;
	Handle<v8::Value>	args[nargs];

	args[0] = ToString(src);
	Local<v8::Value>	value = func->Call(compiler, nargs, args);

	if (value.IsEmpty())
		throw js_error(try_catch);
	CString		result(value);

	PG_TRY();
	{
		MemoryContext	oldcontext = MemoryContextSwitchTo(TopMemoryContext);
		cresult = pstrdup(result.str());
		MemoryContextSwitchTo(oldcontext);
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();

	return cresult;
}

/*
 * fcinfo should be passed if this is an actual function call context, where
 * we can resolve polymorphic types and use function's memory context.
 */
static plv8_proc *
Compile(Oid fn_oid, FunctionCallInfo fcinfo, bool validate, bool is_trigger,
		Dialect dialect)
{
	plv8_proc  *proc;
	char	  **argnames;

	PG_TRY();
	{
		proc = plv8_get_proc(fn_oid, fcinfo, validate, &argnames);
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();

	HandleScope		handle_scope;
	plv8_proc_cache *cache = proc->cache;

	if (cache->function.IsEmpty())
		cache->function = Persistent<Function>::New(CompileFunction(
						cache->proname,
						cache->nargs,
						(const char **) argnames,
						cache->prosrc,
						is_trigger,
						cache->retset,
						dialect));

	return proc;
}

static Local<Function>
CompileFunction(
	const char *proname,
	int proarglen,
	const char *proargs[],
	const char *prosrc,
	bool is_trigger,
	bool retset,
	Dialect dialect)
{
	HandleScope		handle_scope;
	StringInfoData	src;
	Handle<Context>	global_context = GetGlobalContext();

	initStringInfo(&src);

	if (dialect != PLV8_DIALECT_NONE)
		prosrc = CompileDialect(prosrc, dialect);
	/*
	 *  (function (<arg1, ...>){
	 *    <prosrc>
	 *  })
	 */
	appendStringInfo(&src, "(function (");
	if (is_trigger)
	{
		if (proarglen != 0)
			throw js_error("trigger function cannot have arguments");
		// trigger function has special arguments.
		appendStringInfo(&src,
			"NEW, OLD, TG_NAME, TG_WHEN, TG_LEVEL, TG_OP, "
			"TG_RELID, TG_TABLE_NAME, TG_TABLE_SCHEMA, TG_ARGV");
	}
	else
	{
		for (int i = 0; i < proarglen; i++)
		{
			if (i > 0)
				appendStringInfoChar(&src, ',');
			if (proargs && proargs[i])
				appendStringInfoString(&src, proargs[i]);
			else
				appendStringInfo(&src, "$%d", i + 1);	// unnamed argument to $N
		}
	}
	if (dialect)
		appendStringInfo(&src, "){\nreturn %s\n})", prosrc);
	else
		appendStringInfo(&src, "){\n%s\n})", prosrc);

	Handle<v8::Value> name;
	if (proname)
		name = ToString(proname);
	else
		name = Undefined();
	Local<String> source = ToString(src.data, src.len);
	pfree(src.data);

	Context::Scope	context_scope(global_context);
	TryCatch		try_catch;
	Local<Script>	script = Script::New(source, name);

	if (script.IsEmpty())
		throw js_error(try_catch);

	Local<v8::Value>	result = script->Run();
	if (result.IsEmpty())
		throw js_error(try_catch);

	return handle_scope.Close(Local<Function>::Cast(result));
}

Local<Function>
find_js_function(Oid fn_oid)
{
	HeapTuple		tuple;
	Form_pg_proc	proc;
	Oid				prolang;
	NameData		langnames[] = { {"plv8"}, {"plcoffee"}, {"plls"} };
	int				langno;
	int				langlen = sizeof(langnames) / sizeof(NameData);
	Local<Function> func;


	tuple = SearchSysCache(PROCOID, ObjectIdGetDatum(fn_oid), 0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for function %u", fn_oid);
	proc = (Form_pg_proc) GETSTRUCT(tuple);
	prolang = proc->prolang;
	ReleaseSysCache(tuple);

	/* Should not happen? */
	if (!OidIsValid(prolang))
		return func;

	/* See if the function language is a compatible one */
	for (langno = 0; langno < langlen; langno++)
	{
		tuple = SearchSysCache(LANGNAME, NameGetDatum(&langnames[langno]), 0, 0, 0);
		if (HeapTupleIsValid(tuple))
		{
			Oid langtupoid = HeapTupleGetOid(tuple);
			ReleaseSysCache(tuple);
			if (langtupoid == prolang)
				break;
		}
	}

	/* Not found or non-JS function */
	if (langno >= langlen)
		return func;

	try
	{
		plv8_proc		   *proc = Compile(fn_oid, NULL,
										   true, false,
										   (Dialect) (PLV8_DIALECT_NONE + langno));

		TryCatch			try_catch;

		func = Local<Function>::New(proc->cache->function);
	}
	catch (js_error& e) { e.rethrow(); }
	catch (pg_error& e) { e.rethrow(); }

	return func;
}

/*
 * The signature can be either of regproc or regprocedure format.
 */
Local<Function>
find_js_function_by_name(const char *signature)
{
	Oid					funcoid;
	Local<Function>		func;

	if (strchr(signature, '(') == NULL)
		funcoid = DatumGetObjectId(
				DirectFunctionCall1(regprocin, CStringGetDatum(signature)));
	else
		funcoid = DatumGetObjectId(
				DirectFunctionCall1(regprocedurein, CStringGetDatum(signature)));
	func = find_js_function(funcoid);
	if (func.IsEmpty())
		elog(ERROR, "javascript function is not found for \"%s\"", signature);

	return func;
}

/*
 * NOTICE: the returned buffer could be an internal static buffer.
 */
const char *
FormatSPIStatus(int status) throw()
{
	static char	private_buf[1024];

	if (status > 0)
		return "OK";

	switch (status)
	{
		case SPI_ERROR_CONNECT:
			return "SPI_ERROR_CONNECT";
		case SPI_ERROR_COPY:
			return "SPI_ERROR_COPY";
		case SPI_ERROR_OPUNKNOWN:
			return "SPI_ERROR_OPUNKNOWN";
		case SPI_ERROR_UNCONNECTED:
		case SPI_ERROR_TRANSACTION:
			return "current transaction is aborted, "
				   "commands ignored until end of transaction block";
		case SPI_ERROR_CURSOR:
			return "SPI_ERROR_CURSOR";
		case SPI_ERROR_ARGUMENT:
			return "SPI_ERROR_ARGUMENT";
		case SPI_ERROR_PARAM:
			return "SPI_ERROR_PARAM";
		case SPI_ERROR_NOATTRIBUTE:
			return "SPI_ERROR_NOATTRIBUTE";
		case SPI_ERROR_NOOUTFUNC:
			return "SPI_ERROR_NOOUTFUNC";
		case SPI_ERROR_TYPUNKNOWN:
			return "SPI_ERROR_TYPUNKNOWN";
		default:
			snprintf(private_buf, sizeof(private_buf),
				"SPI_ERROR: %d", status);
			return private_buf;
	}
}

Handle<v8::Value>
ThrowError(const char *message) throw()
{
	return ThrowException(Exception::Error(String::New(message)));
}

static Persistent<Context>
GetGlobalContext()
{
	Oid					user_id = GetUserId();
	Persistent<Context>	global_context;
	unsigned int		i;

	for (i = 0; i < ContextVector.size(); i++)
	{
		if (ContextVector[i]->user_id == user_id)
		{
			global_context = ContextVector[i]->context;
			break;
		}
	}
	if (global_context.IsEmpty())
	{
		HandleScope				handle_scope;
		Handle<ObjectTemplate>	global = GetGlobalObjectTemplate();
		plv8_context		   *my_context;

		global_context = Context::New(NULL, global);
		my_context = (plv8_context *) MemoryContextAlloc(TopMemoryContext,
														 sizeof(plv8_context));
		my_context->context = global_context;
		my_context->user_id = user_id;

		/*
		 * Need to register it before running any code, as the code
		 * recursively may want to the global context.
		 */
		ContextVector.push_back(my_context);

		/*
		 * Run the start up procedure if configured.
		 */
		if (plv8_start_proc != NULL)
		{
			Local<Function>		func;

			HandleScope			handle_scope;
			Context::Scope		context_scope(global_context);
			TryCatch			try_catch;
			MemoryContext		ctx = CurrentMemoryContext;

			PG_TRY();
			{
				func = find_js_function_by_name(plv8_start_proc);
			}
			PG_CATCH();
			{
				ErrorData	   *edata;

				MemoryContextSwitchTo(ctx);
				edata = CopyErrorData();
				elog(WARNING, "failed to find js function %s", edata->message);
				FlushErrorState();
				FreeErrorData(edata);
			}
			PG_END_TRY();

			if (!func.IsEmpty())
			{
				Handle<v8::Value>	result =
					DoCall(func, global_context->Global(), 0, NULL);
				if (result.IsEmpty())
					throw js_error(try_catch);
			}
		}

#ifdef ENABLE_DEBUGGER_SUPPORT
		debug_message_context = v8::Persistent<v8::Context>::New(global_context);

		v8::Locker locker;

		v8::Debug::SetDebugMessageDispatchHandler(DispatchDebugMessages, true);

		v8::Debug::EnableAgent("plv8", plv8_debugger_port, false);
#endif  // ENABLE_DEBUGGER_SUPPORT
	}

	return global_context;
}

static Persistent<ObjectTemplate>
GetGlobalObjectTemplate()
{
	static Persistent<ObjectTemplate>	global;

	if (global.IsEmpty())
	{
		HandleScope				handle_scope;

		global = Persistent<ObjectTemplate>::New(ObjectTemplate::New());
		// ERROR levels for elog
		global->Set(String::NewSymbol("DEBUG5"), Int32::New(DEBUG5));
		global->Set(String::NewSymbol("DEBUG4"), Int32::New(DEBUG4));
		global->Set(String::NewSymbol("DEBUG3"), Int32::New(DEBUG3));
		global->Set(String::NewSymbol("DEBUG2"), Int32::New(DEBUG2));
		global->Set(String::NewSymbol("DEBUG1"), Int32::New(DEBUG1));
		global->Set(String::NewSymbol("DEBUG"), Int32::New(DEBUG5));
		global->Set(String::NewSymbol("LOG"), Int32::New(LOG));
		global->Set(String::NewSymbol("INFO"), Int32::New(INFO));
		global->Set(String::NewSymbol("NOTICE"), Int32::New(NOTICE));
		global->Set(String::NewSymbol("WARNING"), Int32::New(WARNING));
		global->Set(String::NewSymbol("ERROR"), Int32::New(ERROR));

		Handle<ObjectTemplate>	plv8 = ObjectTemplate::New();

		SetupPlv8Functions(plv8);
		plv8->Set(String::NewSymbol("version"), String::New(PLV8_VERSION));

		global->Set(String::NewSymbol("plv8"), plv8);
	}

	return global;
}

/*
 * Accessor to plv8_type stored in fcinfo.
 */
plv8_type *
get_plv8_type(PG_FUNCTION_ARGS, int argno)
{
	plv8_proc *proc = (plv8_proc *) fcinfo->flinfo->fn_extra;
	return &proc->argtypes[argno];
}

Converter::Converter(TupleDesc tupdesc) :
	m_tupdesc(tupdesc),
	m_colnames(tupdesc->natts),
	m_coltypes(tupdesc->natts),
	m_is_scalar(false),
	m_memcontext(NULL)
{
	Init();
}

Converter::Converter(TupleDesc tupdesc, bool is_scalar) :
	m_tupdesc(tupdesc),
	m_colnames(tupdesc->natts),
	m_coltypes(tupdesc->natts),
	m_is_scalar(is_scalar),
	m_memcontext(NULL)
{
	Init();
}

Converter::~Converter()
{
	if (m_memcontext != NULL)
	{
		MemoryContext ctx = CurrentMemoryContext;

		PG_TRY();
		{
			MemoryContextDelete(m_memcontext);
		}
		PG_CATCH();
		{
			ErrorData	   *edata;

			MemoryContextSwitchTo(ctx);
			// don't throw out from deconstructor
			edata = CopyErrorData();
			elog(WARNING, "~Converter: %s", edata->message);
			FlushErrorState();
			FreeErrorData(edata);
		}
		PG_END_TRY();
		m_memcontext = NULL;
	}
}

void
Converter::Init()
{
	for (int c = 0; c < m_tupdesc->natts; c++)
	{
		m_colnames[c] = ToString(NameStr(m_tupdesc->attrs[c]->attname));
		PG_TRY();
		{
			if (m_memcontext == NULL)
				m_memcontext = AllocSetContextCreate(
									CurrentMemoryContext,
									"ConverterContext",
									ALLOCSET_SMALL_MINSIZE,
									ALLOCSET_SMALL_INITSIZE,
									ALLOCSET_SMALL_MAXSIZE);
			plv8_fill_type(&m_coltypes[c],
						   m_tupdesc->attrs[c]->atttypid,
						   m_memcontext);
		}
		PG_CATCH();
		{
			throw pg_error();
		}
		PG_END_TRY();
	}
}

// TODO: use prototype instead of per tuple fields to reduce
// memory consumption.
Local<Object>
Converter::ToValue(HeapTuple tuple)
{
	Local<Object>	obj = Object::New();

	for (int c = 0; c < m_tupdesc->natts; c++)
	{
		Datum		datum;
		bool		isnull;

#if PG_VERSION_NUM >= 90000
		datum = heap_getattr(tuple, c + 1, m_tupdesc, &isnull);
#else
		/*
		 * Due to the difference between C and C++ rules,
		 * we cannot call heap_getattr from < 9.0 unfortunately.
		 */
		datum = nocachegetattr(tuple, c + 1, m_tupdesc, &isnull);
#endif

		obj->Set(m_colnames[c], ::ToValue(datum, isnull, &m_coltypes[c]));
	}

	return obj;
}

Datum
Converter::ToDatum(Handle<v8::Value> value, Tuplestorestate *tupstore)
{
	Datum			result;
	TryCatch		try_catch;
	Handle<Object>	obj;

	if (!m_is_scalar)
	{
		if (!value->IsObject())
			throw js_error("argument must be an object");
		obj = Handle<Object>::Cast(value);
		if (obj.IsEmpty())
			throw js_error(try_catch);
	}

	/*
	 * Use vector<char> instead of vector<bool> because <bool> version is
	 * s specialized and different from bool[].
	 */
	Datum  *values = (Datum *) palloc(sizeof(Datum) * m_tupdesc->natts);
	bool   *nulls = (bool *) palloc(sizeof(bool) * m_tupdesc->natts);

	if (!m_is_scalar)
	{
		Handle<Array> names = obj->GetPropertyNames();
		if ((int) names->Length() != m_tupdesc->natts)
			throw js_error("expected fields and property names have different cardinality");

		for (int c = 0; c < m_tupdesc->natts; c++)
		{
			bool found = false;
			CString  colname(m_colnames[c]);
			for (int d = 0; d < m_tupdesc->natts; d++)
			{
				CString fname(names->Get(d));
				if (strcmp(colname, fname) == 0)
				{
					found = true;
					break;
				}
			}
			if (!found)
				throw js_error("field name / property name mismatch");
		}
	}

	for (int c = 0; c < m_tupdesc->natts; c++)
	{
		Handle<v8::Value> attr = m_is_scalar ? value : obj->Get(m_colnames[c]);
		if (attr.IsEmpty() || attr->IsUndefined() || attr->IsNull())
			nulls[c] = true;
		else
			values[c] = ::ToDatum(attr, &nulls[c], &m_coltypes[c]);
	}

	if (tupstore)
	{
		tuplestore_putvalues(tupstore, m_tupdesc, values, nulls);
		result = (Datum) 0;
	}
	else
	{
		result = HeapTupleGetDatum(heap_form_tuple(m_tupdesc, values, nulls));
	}

	pfree(values);
	pfree(nulls);

	return result;
}

js_error::js_error() throw()
	: m_msg(NULL), m_detail(NULL)
{
}

js_error::js_error(const char *msg) throw()
{
	m_msg = pstrdup(msg);
	m_detail = NULL;
}

js_error::js_error(TryCatch &try_catch) throw()
{
	HandleScope			handle_scope;
	String::Utf8Value	exception(try_catch.Exception());
	Handle<Message>		message = try_catch.Message();

	m_msg = NULL;
	m_detail = NULL;

	try
	{
		m_msg = ToCStringCopy(exception);

		if (!message.IsEmpty())
		{
			StringInfoData	str;
			CString			script(message->GetScriptResourceName());
			int				lineno = message->GetLineNumber();
			CString			source(message->GetSourceLine());

			/*
			 * Report lineno - 1 because "function _(...){" was added
			 * at the first line to the javascript code.
			 */
			initStringInfo(&str);
			appendStringInfo(&str, "%s() LINE %d: %s",
				script.str("?"), lineno - 1, source.str("?"));
			m_detail = str.data;
		}
	}
	catch (...)
	{
		// nested error, keep quiet.
	}
}

Local<v8::Value>
js_error::error_object()
{
	char *msg = pstrdup(m_msg ? m_msg : "unknown exception");
	/*
	 * Trim leading "Error: ", in case the message is generated from
	 * another Error.
	 */
	if (strstr(msg, "Error: ") == msg)
		msg += 7;
	Local<String> message = ToString(msg);
	return Exception::Error(message);
}

__attribute__((noreturn))
void
js_error::rethrow() throw()
{
	ereport(ERROR,
		(m_msg ? errmsg("%s", m_msg) : 0,
			m_detail ? errdetail("%s", m_detail) : 0));
	exit(0);	// keep compiler quiet
}

__attribute__((noreturn))
void
pg_error::rethrow() throw()
{
	PG_RE_THROW();
	exit(0);	// keep compiler quiet
}
