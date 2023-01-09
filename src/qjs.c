#include <js.h>
#include <quickjs.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <uv.h>

typedef struct js_task_s js_task_t;
typedef struct js_callback_s js_callback_t;
typedef struct js_finalizer_s js_finalizer_t;
typedef struct js_source_text_module_s js_source_text_module_t;
typedef struct js_synthetic_module_s js_synthetic_module_t;
typedef struct js_module_resolver_s js_module_resolver_t;
typedef struct js_module_evaluator_s js_module_evaluator_t;

struct js_task_s {
  js_env_t *env;
  js_task_cb cb;
  void *data;
};

struct js_platform_s {
  js_platform_options_t options;
  uv_loop_t *loop;
};

struct js_env_s {
  uv_loop_t *loop;
  uv_prepare_t prepare;
  uv_check_t check;
  js_platform_t *platform;
  js_handle_scope_t *scope;
  JSRuntime *runtime;
  JSContext *context;
  js_module_resolver_t *resolvers;
  js_module_evaluator_t *evaluators;
  js_uncaught_exception_cb on_uncaught_exception;
  void *uncaught_exception_data;
};

struct js_value_s {
  JSValue value;
};

struct js_handle_scope_s {
  js_handle_scope_t *parent;
  js_value_t **values;
  size_t len;
  size_t capacity;
};

struct js_escapable_handle_scope_s {
  js_handle_scope_t *scope;
  bool escaped;
};

struct js_module_s {
  JSContext *context;
  JSValue bytecode;
  JSModuleDef *definition;
  void *data;
};

struct js_module_resolver_s {
  js_module_t *module;
  js_module_cb cb;
  js_module_resolver_t *next;
};

struct js_module_evaluator_s {
  js_module_t *module;
  js_synthetic_module_cb cb;
  js_module_evaluator_t *next;
};

struct js_ref_s {
  JSValue value;
  uint32_t count;
};

struct js_deferred_s {
  JSValue resolve;
  JSValue reject;
};

struct js_finalizer_s {
  js_env_t *env;
  void *data;
  js_finalize_cb cb;
  void *hint;
};

struct js_callback_s {
  js_env_t *env;
  js_function_cb cb;
  void *data;
};

struct js_callback_info_s {
  js_callback_t *callback;
  int argc;
  JSValue *argv;
  JSValue self;
};

const char *js_platform_identifier = "quickjs";

const char *js_platform_version = NULL;

static JSClassID js_job_data_class_id;
static JSClassID js_function_data_class_id;
static JSClassID js_external_data_class_id;

static uv_once_t js_platform_init = UV_ONCE_INIT;

static void
on_platform_init () {
  // Class IDs are globally allocated, so we guard their initialization with a
  // `uv_once_t`.

  JS_NewClassID(&js_job_data_class_id);
  JS_NewClassID(&js_function_data_class_id);
  JS_NewClassID(&js_external_data_class_id);
}

int
js_create_platform (uv_loop_t *loop, const js_platform_options_t *options, js_platform_t **result) {
  uv_once(&js_platform_init, on_platform_init);

  js_platform_t *platform = malloc(sizeof(js_platform_t));

  platform->loop = loop;
  platform->options = options ? *options : (js_platform_options_t){};

  *result = platform;

  return 0;
}

int
js_destroy_platform (js_platform_t *platform) {
  free(platform);

  return 0;
}

int
js_get_platform_loop (js_platform_t *platform, uv_loop_t **result) {
  *result = platform->loop;

  return 0;
}

static JSModuleDef *
on_resolve_module (JSContext *context, const char *name, void *opaque) {
  js_env_t *env = (js_env_t *) JS_GetContextOpaque(context);

  js_module_resolver_t *resolver = env->resolvers;

  js_value_t specifier = {
    .value = JS_NewString(env->context, name),
  };

  js_value_t assertions = {
    .value = JS_NULL,
  };

  js_module_t *referrer = resolver->module;

  js_handle_scope_t *scope;
  js_open_handle_scope(env, &scope);

  js_module_t *module = resolver->cb(
    env,
    &specifier,
    &assertions,
    referrer,
    referrer->data
  );

  js_close_handle_scope(env, scope);

  JS_FreeValue(env->context, specifier.value);

  return module->definition;
}

static void
on_external_finalize (JSRuntime *runtime, JSValue value) {
  js_finalizer_t *finalizer = (js_finalizer_t *) JS_GetOpaque(value, js_external_data_class_id);

  if (finalizer->cb) {
    finalizer->cb(finalizer->env, finalizer->data, finalizer->hint);
  }

  free(finalizer);
}

static void
on_uncaught_exception (JSContext *context, JSValue error) {
  js_env_t *env = (js_env_t *) JS_GetContextOpaque(context);

  if (env->on_uncaught_exception == NULL) return;

  js_value_t wrapper;
  wrapper.value = error;

  js_handle_scope_t *scope;
  js_open_handle_scope(env, &scope);

  env->on_uncaught_exception(env, &wrapper, env->uncaught_exception_data);

  js_close_handle_scope(env, scope);
}

static inline void
run_microtasks (js_env_t *env) {
  JSContext *context;

  for (;;) {
    int err = JS_ExecutePendingJob(env->runtime, &context);
    if (err <= 0) break;

    JSValue error = JS_GetException(context);

    if (JS_IsNull(error)) continue;

    on_uncaught_exception(context, error);
  }
}

static void
on_prepare (uv_prepare_t *handle);

static inline void
check_liveness (js_env_t *env) {
  if (true /* macrotask queue empty */) {
    uv_prepare_stop(&env->prepare);
  } else {
    uv_prepare_start(&env->prepare, on_prepare);
  }
}

static void
on_prepare (uv_prepare_t *handle) {
  js_env_t *env = (js_env_t *) handle->data;

  check_liveness(env);
}

static void
on_check (uv_check_t *handle) {
  js_env_t *env = (js_env_t *) handle->data;

  run_microtasks(env);

  if (uv_loop_alive(env->loop)) return;

  check_liveness(env);
}

int
js_create_env (uv_loop_t *loop, js_platform_t *platform, js_env_t **result) {
  JSRuntime *runtime = JS_NewRuntime();
  JSContext *context = JS_NewContextRaw(runtime);

  JS_AddIntrinsicBaseObjects(context);
  JS_AddIntrinsicDate(context);
  JS_AddIntrinsicEval(context);
  JS_AddIntrinsicStringNormalize(context);
  JS_AddIntrinsicRegExpCompiler(context);
  JS_AddIntrinsicRegExp(context);
  JS_AddIntrinsicJSON(context);
  JS_AddIntrinsicProxy(context);
  JS_AddIntrinsicMapSet(context);
  JS_AddIntrinsicTypedArrays(context);
  JS_AddIntrinsicPromise(context);
  JS_AddIntrinsicBigInt(context);

  JS_SetCanBlock(runtime, false);
  JS_SetModuleLoaderFunc(runtime, NULL, on_resolve_module, NULL);

  uint64_t constrained_memory = uv_get_constrained_memory();
  uint64_t total_memory = uv_get_total_memory();

  if (constrained_memory > 0 && constrained_memory < total_memory) {
    total_memory = constrained_memory;
  }

  if (total_memory > 0) {
    JS_SetMemoryLimit(runtime, total_memory);
  }

  JSClassDef external_class = {
    .class_name = "External",
    .finalizer = on_external_finalize,
  };

  JS_NewClass(runtime, js_external_data_class_id, &external_class);

  js_env_t *env = malloc(sizeof(js_env_t));

  env->loop = loop;
  env->platform = platform;
  env->runtime = runtime;
  env->context = context;

  JS_SetRuntimeOpaque(runtime, env);
  JS_SetContextOpaque(context, env);

  uv_prepare_init(loop, &env->prepare);
  uv_prepare_start(&env->prepare, on_prepare);
  env->prepare.data = (void *) env;

  uv_check_init(loop, &env->check);
  uv_check_start(&env->check, on_check);
  env->check.data = (void *) env;

  // The check handle should not on its own keep the loop alive; it's simply
  // used for running any outstanding tasks that might cause additional work
  // to be queued.
  uv_unref((uv_handle_t *) (&env->check));

  js_open_handle_scope(env, &env->scope);

  *result = env;

  return 0;
}

int
js_destroy_env (js_env_t *env) {
  js_close_handle_scope(env, env->scope);

  JS_FreeContext(env->context);
  JS_FreeRuntime(env->runtime);

  free(env);

  return 0;
}

int
js_on_uncaught_exception (js_env_t *env, js_uncaught_exception_cb cb, void *data) {
  env->on_uncaught_exception = cb;
  env->uncaught_exception_data = data;

  return 0;
}

int
js_get_env_loop (js_env_t *env, uv_loop_t **result) {
  *result = env->loop;

  return 0;
}

int
js_open_handle_scope (js_env_t *env, js_handle_scope_t **result) {
  js_handle_scope_t *scope = malloc(sizeof(js_handle_scope_t));

  scope->parent = env->scope;
  scope->values = NULL;
  scope->len = 0;
  scope->capacity = 0;

  env->scope = scope;

  *result = scope;

  return 0;
}

int
js_close_handle_scope (js_env_t *env, js_handle_scope_t *scope) {
  for (size_t i = 0; i < scope->len; i++) {
    js_value_t *value = scope->values[i];

    JS_FreeValue(env->context, value->value);

    free(value);
  }

  env->scope = scope->parent;

  if (scope->values) free(scope->values);

  free(scope);

  return 0;
}

int
js_open_escapable_handle_scope (js_env_t *env, js_escapable_handle_scope_t **result) {
  js_escapable_handle_scope_t *scope = malloc(sizeof(js_escapable_handle_scope_t));

  scope->escaped = false;

  return js_open_handle_scope(env, &scope->scope);
}

int
js_close_escapable_handle_scope (js_env_t *env, js_escapable_handle_scope_t *scope) {
  int err = js_close_handle_scope(env, scope->scope);

  free(scope);

  return err;
}

static int
js_attach_to_handle_scope (js_env_t *env, js_handle_scope_t *scope, js_value_t *value) {
  if (scope->len <= scope->capacity) {
    if (scope->capacity) scope->capacity *= 2;
    else scope->capacity = 4;

    scope->values = realloc(scope->values, scope->capacity * sizeof(js_value_t *));
  }

  scope->values[scope->len++] = value;

  return 0;
}

int
js_escape_handle (js_env_t *env, js_escapable_handle_scope_t *scope, js_value_t *escapee, js_value_t **result) {
  if (scope->escaped) {
    js_throw_error(env, NULL, "Scope has already been escaped");

    return -1;
  }

  scope->escaped = true;

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = JS_DupValue(env->context, escapee->value);

  *result = wrapper;

  js_attach_to_handle_scope(env, scope->scope->parent, wrapper);

  return 0;
}

int
js_run_script (js_env_t *env, js_value_t *source, js_value_t **result) {
  size_t str_len;
  const char *str = JS_ToCStringLen(env->context, &str_len, source->value);

  JSValue value = JS_Eval(env->context, str, str_len, "<anonymous>", JS_EVAL_TYPE_GLOBAL);

  JS_FreeCString(env->context, str);

  if (JS_IsException(value)) return -1;

  if (result == NULL) JS_FreeValue(env->context, value);
  else {
    js_value_t *wrapper = malloc(sizeof(js_value_t));

    wrapper->value = value;

    *result = wrapper;

    js_attach_to_handle_scope(env, env->scope, wrapper);
  }

  return 0;
}

int
js_create_module (js_env_t *env, const char *name, size_t len, js_value_t *source, js_module_cb cb, void *data, js_module_t **result) {
  js_module_t *module = malloc(sizeof(js_module_t));

  module->context = env->context;
  module->data = data;

  js_module_resolver_t resolver = {
    .module = module,
    .cb = cb,
    .next = env->resolvers,
  };

  env->resolvers = &resolver;

  size_t str_len;
  const char *str = JS_ToCStringLen(env->context, &str_len, source->value);

  module->bytecode = JS_Eval(
    env->context,
    str,
    str_len,
    name,
    JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY
  );

  module->definition = (JSModuleDef *) JS_VALUE_GET_PTR(module->bytecode);

  JS_FreeCString(env->context, str);

  env->resolvers = resolver.next;

  *result = module;

  return 0;
}

static int
on_evaluate_module (JSContext *context, JSModuleDef *definition) {
  js_env_t *env = (js_env_t *) JS_GetContextOpaque(context);

  js_module_evaluator_t *evaluator = env->evaluators;

  while (evaluator && evaluator->module->definition != definition) {
    evaluator = evaluator->next;
  }

  js_module_t *module = evaluator->module;

  evaluator->cb(env, module, module->data);

  return 0;
}

int
js_create_synthetic_module (js_env_t *env, const char *name, size_t len, js_value_t *const export_names[], size_t names_len, js_synthetic_module_cb cb, void *data, js_module_t **result) {
  js_module_t *module = malloc(sizeof(js_module_t));

  module->context = env->context;
  module->data = data;
  module->definition = JS_NewCModule(env->context, name, on_evaluate_module);

  for (size_t i = 0; i < names_len; i++) {
    const char *str = JS_ToCString(env->context, export_names[i]->value);

    JS_AddModuleExport(env->context, module->definition, str);

    JS_FreeCString(env->context, str);
  }

  js_module_evaluator_t *evaluator = malloc(sizeof(js_module_evaluator_t));

  evaluator->module = module;
  evaluator->cb = cb;
  evaluator->next = env->evaluators;

  env->evaluators = evaluator;

  *result = module;

  return 0;
}

int
js_delete_module (js_env_t *env, js_module_t *module) {
  free(module);

  return 0;
}

int
js_set_module_export (js_env_t *env, js_module_t *module, js_value_t *name, js_value_t *value) {
  const char *str = JS_ToCString(env->context, name->value);

  JS_SetModuleExport(env->context, module->definition, str, JS_DupValue(env->context, value->value));

  JS_FreeCString(env->context, str);

  return 0;
}

int
js_run_module (js_env_t *env, js_module_t *module, js_value_t **result) {
  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = JS_EvalFunction(env->context, module->bytecode);

  *result = wrapper;

  js_attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

static inline void
js_set_weak_reference (js_env_t *env, js_ref_t *reference) {
  // TODO: Attach symbol with finalizer
}

static inline void
js_clear_weak_reference (js_env_t *env, js_ref_t *reference) {
  // TODO: Detach symbol with finalizer
}

int
js_create_reference (js_env_t *env, js_value_t *value, uint32_t count, js_ref_t **result) {
  js_ref_t *reference = malloc(sizeof(js_ref_t));

  reference->value = JS_DupValue(env->context, value->value);
  reference->count = count;

  if (reference->count == 0) js_set_weak_reference(env, reference);

  *result = reference;

  return 0;
}

int
js_delete_reference (js_env_t *env, js_ref_t *reference) {
  JS_FreeValue(env->context, reference->value);

  free(reference);

  return 0;
}

int
js_reference_ref (js_env_t *env, js_ref_t *reference, uint32_t *result) {
  reference->count++;

  if (reference->count == 1) js_clear_weak_reference(env, reference);

  if (result) {
    *result = reference->count;
  }

  return 0;
}

int
js_reference_unref (js_env_t *env, js_ref_t *reference, uint32_t *result) {
  if (reference->count == 0) {
    js_throw_error(env, NULL, "Cannot decrease reference count");

    return -1;
  }

  reference->count--;

  if (reference->count == 0) js_set_weak_reference(env, reference);

  if (result) {
    *result = reference->count;
  }

  return 0;
}

int
js_get_reference_value (js_env_t *env, js_ref_t *reference, js_value_t **result) {
  if (JS_IsNull(reference->value)) {
    *result = NULL;
  } else {
    js_value_t *wrapper = malloc(sizeof(js_value_t));

    wrapper->value = JS_DupValue(env->context, reference->value);

    *result = wrapper;

    js_attach_to_handle_scope(env, env->scope, wrapper);
  }

  return 0;
}

int
js_wrap (js_env_t *env, js_value_t *object, void *data, js_finalize_cb finalize_cb, void *finalize_hint, js_ref_t **result) {
  return -1;
}

int
js_unwrap (js_env_t *env, js_value_t *object, void **result) {
  return -1;
}

int
js_remove_wrap (js_env_t *env, js_value_t *object, void **result) {
  return -1;
}

int
js_create_int32 (js_env_t *env, int32_t value, js_value_t **result) {
  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = JS_NewInt32(env->context, value);

  *result = wrapper;

  js_attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_create_uint32 (js_env_t *env, uint32_t value, js_value_t **result) {
  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = JS_NewUint32(env->context, value);

  *result = wrapper;

  js_attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_create_int64 (js_env_t *env, int64_t value, js_value_t **result) {
  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = JS_NewInt64(env->context, value);

  *result = wrapper;

  js_attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_create_double (js_env_t *env, double value, js_value_t **result) {
  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = JS_NewFloat64(env->context, value);

  *result = wrapper;

  js_attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_create_bigint_int64 (js_env_t *env, int64_t value, js_value_t **result) {
  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = JS_NewBigInt64(env->context, value);

  *result = wrapper;

  js_attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_create_bigint_uint64 (js_env_t *env, uint64_t value, js_value_t **result) {
  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = JS_NewBigUint64(env->context, value);

  *result = wrapper;

  js_attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_create_string_utf8 (js_env_t *env, const char *str, size_t len, js_value_t **result) {
  js_value_t *wrapper = malloc(sizeof(js_value_t));

  if (len == (size_t) -1) {
    wrapper->value = JS_NewString(env->context, str);
  } else {
    wrapper->value = JS_NewStringLen(env->context, str, len);
  }

  *result = wrapper;

  js_attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_create_symbol (js_env_t *env, js_value_t *description, js_value_t **result) {
  JSValue global = JS_GetGlobalObject(env->context);
  JSValue constructor = JS_GetPropertyStr(env->context, global, "Symbol");

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  JSValue arg = description->value;

  wrapper->value = JS_CallConstructor(env->context, constructor, 1, &arg);

  *result = wrapper;

  js_attach_to_handle_scope(env, env->scope, wrapper);

  JS_FreeValue(env->context, constructor);
  JS_FreeValue(env->context, global);

  return 0;
}

int
js_create_object (js_env_t *env, js_value_t **result) {
  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = JS_NewObject(env->context);

  *result = wrapper;

  js_attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

static JSValue
on_function_call (JSContext *context, JSValueConst self, int argc, JSValueConst *argv, int magic, JSValue *data) {
  js_callback_t *callback = (js_callback_t *) JS_GetOpaque(*data, js_function_data_class_id);

  js_env_t *env = callback->env;

  js_callback_info_t callback_info = {
    .callback = callback,
    .argc = argc,
    .argv = argv,
    .self = self,
  };

  js_handle_scope_t *scope;
  js_open_handle_scope(env, &scope);

  js_value_t *result = callback->cb(env, &callback_info);

  JSValue value, error;

  if (result == NULL) value = JS_UNDEFINED;
  else value = JS_DupValue(env->context, result->value);

  error = JS_GetException(env->context);

  if (!JS_IsNull(error)) {
    JS_FreeValue(env->context, value);

    value = JS_Throw(env->context, JS_DupValue(env->context, error));
  }

  js_close_handle_scope(env, scope);

  return value;
}

int
js_create_function (js_env_t *env, const char *name, size_t len, js_function_cb cb, void *data, js_value_t **result) {
  js_callback_t *callback = malloc(sizeof(js_callback_t));

  callback->env = env;
  callback->cb = cb;
  callback->data = data;

  JSValue external = JS_NewObjectClass(env->context, js_function_data_class_id);

  JS_SetOpaque(external, callback);

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = JS_NewCFunctionData(env->context, on_function_call, 0, 0, 1, &external);

  JS_FreeValue(env->context, external);

  *result = wrapper;

  js_attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_create_array (js_env_t *env, js_value_t **result) {
  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = JS_NewArray(env->context);

  *result = wrapper;

  js_attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_create_array_with_length (js_env_t *env, size_t len, js_value_t **result) {
  JSValue global = JS_GetGlobalObject(env->context);
  JSValue constructor = JS_GetPropertyStr(env->context, global, "Array");

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  JSValue arg = JS_NewUint32(env->context, len);

  wrapper->value = JS_CallConstructor(env->context, constructor, 1, &arg);

  *result = wrapper;

  js_attach_to_handle_scope(env, env->scope, wrapper);

  JS_FreeValue(env->context, arg);
  JS_FreeValue(env->context, constructor);
  JS_FreeValue(env->context, global);

  return 0;
}

int
js_create_external (js_env_t *env, void *data, js_finalize_cb finalize_cb, void *finalize_hint, js_value_t **result) {
  js_finalizer_t *finalizer = malloc(sizeof(js_finalizer_t));

  finalizer->env = env;
  finalizer->data = data;
  finalizer->cb = finalize_cb;
  finalizer->hint = finalize_hint;

  JSValue external = JS_NewObjectClass(env->context, js_external_data_class_id);

  JS_SetOpaque(external, finalizer);

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = external;

  *result = wrapper;

  js_attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_create_date (js_env_t *env, double time, js_value_t **result) {
  JSValue global = JS_GetGlobalObject(env->context);
  JSValue constructor = JS_GetPropertyStr(env->context, global, "Date");

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  JSValue arg = JS_NewFloat64(env->context, time);

  wrapper->value = JS_CallConstructor(env->context, constructor, 1, &arg);

  *result = wrapper;

  js_attach_to_handle_scope(env, env->scope, wrapper);

  JS_FreeValue(env->context, arg);
  JS_FreeValue(env->context, constructor);
  JS_FreeValue(env->context, global);

  return 0;
}

int
js_create_error (js_env_t *env, js_value_t *code, js_value_t *message, js_value_t **result) {
  JSValue global = JS_GetGlobalObject(env->context);
  JSValue constructor = JS_GetPropertyStr(env->context, global, "Error");

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  JSValue arg = message->value;

  JSValue error = JS_CallConstructor(env->context, constructor, 1, &arg);

  if (code) {
    JS_SetPropertyStr(env->context, error, "code", JS_DupValue(env->context, code->value));
  }

  wrapper->value = error;

  *result = wrapper;

  js_attach_to_handle_scope(env, env->scope, wrapper);

  JS_FreeValue(env->context, constructor);
  JS_FreeValue(env->context, global);

  return 0;
}

int
js_create_type_error (js_env_t *env, js_value_t *code, js_value_t *message, js_value_t **result) {
  JSValue global = JS_GetGlobalObject(env->context);
  JSValue constructor = JS_GetPropertyStr(env->context, global, "TypeError");

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  JSValue arg = message->value;

  JSValue error = JS_CallConstructor(env->context, constructor, 1, &arg);

  if (code) {
    JS_SetPropertyStr(env->context, error, "code", JS_DupValue(env->context, code->value));
  }

  wrapper->value = error;

  *result = wrapper;

  js_attach_to_handle_scope(env, env->scope, wrapper);

  JS_FreeValue(env->context, constructor);
  JS_FreeValue(env->context, global);

  return 0;
}

int
js_create_range_error (js_env_t *env, js_value_t *code, js_value_t *message, js_value_t **result) {
  JSValue global = JS_GetGlobalObject(env->context);
  JSValue constructor = JS_GetPropertyStr(env->context, global, "RangeError");

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  JSValue arg = message->value;

  JSValue error = JS_CallConstructor(env->context, constructor, 1, &arg);

  if (code) {
    JS_SetPropertyStr(env->context, error, "code", JS_DupValue(env->context, code->value));
  }

  wrapper->value = error;

  *result = wrapper;

  js_attach_to_handle_scope(env, env->scope, wrapper);

  JS_FreeValue(env->context, constructor);
  JS_FreeValue(env->context, global);

  return 0;
}

int
js_create_syntax_error (js_env_t *env, js_value_t *code, js_value_t *message, js_value_t **result) {
  JSValue global = JS_GetGlobalObject(env->context);
  JSValue constructor = JS_GetPropertyStr(env->context, global, "SyntaxError");

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  JSValue arg = message->value;

  JSValue error = JS_CallConstructor(env->context, constructor, 1, &arg);

  if (code) {
    JS_SetPropertyStr(env->context, error, "code", JS_DupValue(env->context, code->value));
  }

  wrapper->value = error;

  *result = wrapper;

  js_attach_to_handle_scope(env, env->scope, wrapper);

  JS_FreeValue(env->context, constructor);
  JS_FreeValue(env->context, global);

  return 0;
}

int
js_create_promise (js_env_t *env, js_deferred_t **deferred, js_value_t **promise) {
  *deferred = malloc(sizeof(js_deferred_t));

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  JSValue functions[2];

  wrapper->value = JS_NewPromiseCapability(env->context, functions);

  (*deferred)->resolve = functions[0];
  (*deferred)->reject = functions[1];

  *promise = wrapper;

  js_attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_resolve_deferred (js_env_t *env, js_deferred_t *deferred, js_value_t *resolution) {
  JSValue global = JS_GetGlobalObject(env->context);

  JS_DupValue(env->context, resolution->value);

  JSValue result = JS_Call(env->context, deferred->resolve, global, 1, &resolution->value);

  JS_FreeValue(env->context, global);
  JS_FreeValue(env->context, result);
  JS_FreeValue(env->context, deferred->resolve);
  JS_FreeValue(env->context, deferred->reject);

  free(deferred);

  return 0;
}

int
js_reject_deferred (js_env_t *env, js_deferred_t *deferred, js_value_t *resolution) {
  JSValue global = JS_GetGlobalObject(env->context);

  JS_DupValue(env->context, resolution->value);

  JSValue result = JS_Call(env->context, deferred->reject, global, 1, &resolution->value);

  JS_FreeValue(env->context, global);
  JS_FreeValue(env->context, result);
  JS_FreeValue(env->context, deferred->resolve);
  JS_FreeValue(env->context, deferred->reject);

  free(deferred);

  return 0;
}

int
js_get_promise_state (js_env_t *env, js_value_t *promise, js_promise_state_t *result) {
  switch (JS_GetPromiseState(env->context, promise->value)) {
  case JS_PROMISE_PENDING:
    *result = js_promise_pending;
    break;
  case JS_PROMISE_FULFILLED:
    *result = js_promise_fulfilled;
    break;
  case JS_PROMISE_REJECTED:
    *result = js_promise_rejected;
    break;
  }

  return 0;
}

int
js_get_promise_result (js_env_t *env, js_value_t *promise, js_value_t **result) {
  if (JS_GetPromiseState(env->context, promise->value) == JS_PROMISE_PENDING) {
    js_throw_error(env, NULL, "Promise is pending");

    return -1;
  }

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = JS_GetPromiseResult(env->context, promise->value);

  *result = wrapper;

  js_attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_create_arraybuffer (js_env_t *env, size_t len, void **data, js_value_t **result) {
  JSValue arraybuffer = JS_NewArrayBufferCopy(env->context, NULL, len);

  if (data) {
    size_t len;

    *data = JS_GetArrayBuffer(env->context, &len, arraybuffer);
  }

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = arraybuffer;

  *result = wrapper;

  js_attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

static void
on_arraybuffer_finalize (JSRuntime *rt, void *opaque, void *ptr) {
  js_finalizer_t *finalizer = (js_finalizer_t *) opaque;

  if (finalizer->cb) {
    finalizer->cb(finalizer->env, finalizer->data, finalizer->hint);
  }

  free(finalizer);
}

int
js_create_external_arraybuffer (js_env_t *env, void *data, size_t len, js_finalize_cb finalize_cb, void *finalize_hint, js_value_t **result) {
  js_finalizer_t *finalizer = malloc(sizeof(js_finalizer_t));

  finalizer->env = env;
  finalizer->data = data;
  finalizer->cb = finalize_cb;
  finalizer->hint = finalize_hint;

  JSValue arraybuffer = JS_NewArrayBuffer(env->context, (uint8_t *) data, len, on_arraybuffer_finalize, (void *) finalizer, false);

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = arraybuffer;

  *result = wrapper;

  js_attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_detach_arraybuffer (js_env_t *env, js_value_t *arraybuffer) {
  JS_DetachArrayBuffer(env->context, arraybuffer->value);

  return 0;
}

int
js_typeof (js_env_t *env, js_value_t *value, js_value_type_t *result) {
  if (JS_IsNumber(value->value)) {
    *result = js_number;
  } else if (JS_IsBigInt(env->context, value->value)) {
    *result = js_bigint;
  } else if (JS_IsString(value->value)) {
    *result = js_string;
  } else if (JS_IsFunction(env->context, value->value)) {
    *result = js_function;
  } else if (JS_IsObject(value->value)) {
    bool is_external;

    int err = js_is_external(env, value, &is_external);
    if (err < 0) return err;

    *result = is_external ? js_external : js_object;
  } else if (JS_IsBool(value->value)) {
    *result = js_boolean;
  } else if (JS_IsUndefined(value->value)) {
    *result = js_undefined;
  } else if (JS_IsSymbol(value->value)) {
    *result = js_symbol;
  } else if (JS_IsNull(value->value)) {
    *result = js_null;
  }

  return 0;
}

int
js_is_undefined (js_env_t *env, js_value_t *value, bool *result) {
  *result = JS_IsUndefined(value->value);

  return 0;
}

int
js_is_null (js_env_t *env, js_value_t *value, bool *result) {
  *result = JS_IsNull(value->value);

  return 0;
}

int
js_is_boolean (js_env_t *env, js_value_t *value, bool *result) {
  *result = JS_IsBool(value->value);

  return 0;
}

int
js_is_number (js_env_t *env, js_value_t *value, bool *result) {
  *result = JS_IsNumber(value->value);

  return 0;
}

int
js_is_string (js_env_t *env, js_value_t *value, bool *result) {
  *result = JS_IsString(value->value);

  return 0;
}

int
js_is_symbol (js_env_t *env, js_value_t *value, bool *result) {
  *result = JS_IsSymbol(value->value);

  return 0;
}

int
js_is_object (js_env_t *env, js_value_t *value, bool *result) {
  *result = JS_IsObject(value->value);

  return 0;
}

int
js_is_function (js_env_t *env, js_value_t *value, bool *result) {
  *result = JS_IsFunction(env->context, value->value);

  return 0;
}

int
js_is_array (js_env_t *env, js_value_t *value, bool *result) {
  *result = JS_IsArray(env->context, value->value);

  return 0;
}

int
js_is_external (js_env_t *env, js_value_t *value, bool *result) {
  JSValue external = JS_GetClassProto(env->context, js_external_data_class_id);

  *result = JS_IsInstanceOf(env->context, value->value, external);

  JS_FreeValue(env->context, external);

  return 0;
}

int
js_is_bigint (js_env_t *env, js_value_t *value, bool *result) {
  *result = JS_IsBigInt(env->context, value->value);

  return 0;
}

int
js_is_date (js_env_t *env, js_value_t *value, bool *result) {
  JSValue global = JS_GetGlobalObject(env->context);
  JSValue constructor = JS_GetPropertyStr(env->context, global, "Date");

  *result = JS_IsInstanceOf(env->context, value->value, constructor);

  JS_FreeValue(env->context, constructor);
  JS_FreeValue(env->context, global);

  return 0;
}

int
js_is_error (js_env_t *env, js_value_t *value, bool *result) {
  *result = JS_IsError(env->context, value->value);

  return 0;
}

int
js_is_promise (js_env_t *env, js_value_t *value, bool *result) {
  JSValue global = JS_GetGlobalObject(env->context);
  JSValue constructor = JS_GetPropertyStr(env->context, global, "Promise");

  *result = JS_IsInstanceOf(env->context, value->value, constructor);

  JS_FreeValue(env->context, constructor);
  JS_FreeValue(env->context, global);

  return 0;
}

int
js_is_arraybuffer (js_env_t *env, js_value_t *value, bool *result) {
  JSValue global = JS_GetGlobalObject(env->context);
  JSValue constructor = JS_GetPropertyStr(env->context, global, "ArrayBuffer");

  *result = JS_IsInstanceOf(env->context, value->value, constructor);

  JS_FreeValue(env->context, constructor);
  JS_FreeValue(env->context, global);

  return 0;
}

int
js_is_typedarray (js_env_t *env, js_value_t *value, bool *result) {
  JSValue global = JS_GetGlobalObject(env->context);

#define check_type(class) \
  { \
    JSValue constructor = JS_GetPropertyStr(env->context, global, class); \
\
    if (JS_IsInstanceOf(env->context, value->value, constructor)) { \
      *result = true; \
\
      JS_FreeValue(env->context, constructor); \
\
      goto done; \
    } \
\
    JS_FreeValue(env->context, constructor); \
  }

  check_type("Int8Array");
  check_type("Uint8Array");
  check_type("Uint8ClampedArray");
  check_type("Int16Array");
  check_type("Uint16Array");
  check_type("Int32Array");
  check_type("Uint32Array");
  check_type("Float32Array");
  check_type("Float64Array");
  check_type("BigInt64Array");
  check_type("BigUint64Array");

#undef check_type

  *result = false;

done:
  JS_FreeValue(env->context, global);

  return 0;
}

int
js_is_dataview (js_env_t *env, js_value_t *value, bool *result) {
  JSValue global = JS_GetGlobalObject(env->context);
  JSValue constructor = JS_GetPropertyStr(env->context, global, "DataView");

  *result = JS_IsInstanceOf(env->context, value->value, constructor);

  JS_FreeValue(env->context, constructor);
  JS_FreeValue(env->context, global);

  return 0;
}

int
js_strict_equals (js_env_t *env, js_value_t *a, js_value_t *b, bool *result) {
  return -1;
}

int
js_get_global (js_env_t *env, js_value_t **result) {
  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = JS_GetGlobalObject(env->context);

  *result = wrapper;

  js_attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_get_undefined (js_env_t *env, js_value_t **result) {
  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = JS_UNDEFINED;

  *result = wrapper;

  js_attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_get_null (js_env_t *env, js_value_t **result) {
  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = JS_NULL;

  *result = wrapper;

  js_attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_get_boolean (js_env_t *env, bool value, js_value_t **result) {
  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = value ? JS_TRUE : JS_FALSE;

  *result = wrapper;

  js_attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_get_value_bool (js_env_t *env, js_value_t *value, bool *result) {
  *result = JS_ToBool(env->context, value->value);

  return 0;
}

int
js_get_value_int32 (js_env_t *env, js_value_t *value, int32_t *result) {
  JS_ToInt32(env->context, result, value->value);

  return 0;
}

int
js_get_value_uint32 (js_env_t *env, js_value_t *value, uint32_t *result) {
  JS_ToUint32(env->context, result, value->value);

  return 0;
}

int
js_get_value_int64 (js_env_t *env, js_value_t *value, int64_t *result) {
  JS_ToInt64(env->context, result, value->value);

  return 0;
}

int
js_get_value_double (js_env_t *env, js_value_t *value, double *result) {
  JS_ToFloat64(env->context, result, value->value);

  return 0;
}

int
js_get_value_bigint_int64 (js_env_t *env, js_value_t *value, int64_t *result) {
  JS_ToBigInt64(env->context, result, value->value);

  return 0;
}

int
js_get_value_bigint_uint64 (js_env_t *env, js_value_t *value, uint64_t *result) {
  JS_ToBigInt64(env->context, (int64_t *) result, value->value);

  return 0;
}

int
js_get_value_string_utf8 (js_env_t *env, js_value_t *value, char *str, size_t len, size_t *result) {
  size_t cstr_len;
  const char *cstr = JS_ToCStringLen(env->context, &cstr_len, value->value);

  if (str == NULL) {
    *result = cstr_len;
  } else if (len != 0) {
    len = cstr_len < len - 1 ? cstr_len : len - 1;

    strncpy(str, cstr, len);

    str[len] = '\0';

    if (result) {
      *result = len;
    }
  } else if (result) {
    *result = 0;
  }

  JS_FreeCString(env->context, cstr);

  return 0;
}

int
js_get_value_external (js_env_t *env, js_value_t *value, void **result) {
  js_finalizer_t *finalizer = (js_finalizer_t *) JS_GetOpaque(value->value, js_external_data_class_id);

  *result = finalizer->data;

  return 0;
}

int
js_get_value_date (js_env_t *env, js_value_t *value, double *result) {
  return -1;
}

int
js_get_array_length (js_env_t *env, js_value_t *value, uint32_t *result) {
  return -1;
}

int
js_get_property (js_env_t *env, js_value_t *object, js_value_t *key, js_value_t **result) {
  JSAtom atom = JS_ValueToAtom(env->context, key->value);

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = JS_GetProperty(env->context, object->value, atom);

  *result = wrapper;

  js_attach_to_handle_scope(env, env->scope, wrapper);

  JS_FreeAtom(env->context, atom);

  return 0;
}

int
js_has_property (js_env_t *env, js_value_t *object, js_value_t *key, bool *result) {
  JSAtom atom = JS_ValueToAtom(env->context, key->value);

  *result = JS_HasProperty(env->context, object->value, atom);

  JS_FreeAtom(env->context, atom);

  return 0;
}

int
js_set_property (js_env_t *env, js_value_t *object, js_value_t *key, js_value_t *value) {
  JSAtom atom = JS_ValueToAtom(env->context, key->value);

  JS_SetProperty(env->context, object->value, atom, JS_DupValue(env->context, value->value));

  JS_FreeAtom(env->context, atom);

  return 0;
}

int
js_delete_property (js_env_t *env, js_value_t *object, js_value_t *key, bool *result) {
  JSAtom atom = JS_ValueToAtom(env->context, key->value);

  *result = JS_HasProperty(env->context, object->value, atom);

  JS_DeleteProperty(env->context, object->value, atom, 0);

  JS_FreeAtom(env->context, atom);

  return 0;
}

int
js_get_named_property (js_env_t *env, js_value_t *object, const char *name, js_value_t **result) {
  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = JS_GetPropertyStr(env->context, object->value, name);

  *result = wrapper;

  js_attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_has_named_property (js_env_t *env, js_value_t *object, const char *name, bool *result) {
  JSAtom atom = JS_NewAtom(env->context, name);

  *result = JS_HasProperty(env->context, object->value, atom);

  JS_FreeAtom(env->context, atom);

  return 0;
}

int
js_set_named_property (js_env_t *env, js_value_t *object, const char *name, js_value_t *value) {
  JS_SetPropertyStr(env->context, object->value, name, JS_DupValue(env->context, value->value));

  return 0;
}

int
js_delete_named_property (js_env_t *env, js_value_t *object, const char *name, bool *result) {
  JSAtom atom = JS_NewAtom(env->context, name);

  *result = JS_HasProperty(env->context, object->value, atom);

  JS_DeleteProperty(env->context, object->value, atom, 0);

  JS_FreeAtom(env->context, atom);

  return 0;
}

int
js_get_element (js_env_t *env, js_value_t *object, uint32_t index, js_value_t **result) {
  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = JS_GetPropertyUint32(env->context, object->value, index);

  *result = wrapper;

  js_attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_has_element (js_env_t *env, js_value_t *object, uint32_t index, bool *result) {
  JSAtom atom = JS_NewAtomUInt32(env->context, index);

  *result = JS_HasProperty(env->context, object->value, atom);

  JS_FreeAtom(env->context, atom);

  return 0;
}

int
js_set_element (js_env_t *env, js_value_t *object, uint32_t index, js_value_t *value) {
  JS_SetPropertyUint32(env->context, object->value, index, JS_DupValue(env->context, value->value));

  return 0;
}

int
js_delete_element (js_env_t *env, js_value_t *object, uint32_t index, bool *result) {
  JSAtom atom = JS_NewAtomUInt32(env->context, index);

  *result = JS_HasProperty(env->context, object->value, atom);

  JS_DeleteProperty(env->context, object->value, atom, 0);

  JS_FreeAtom(env->context, atom);

  return 0;
}

int
js_get_callback_info (js_env_t *env, const js_callback_info_t *info, size_t *argc, js_value_t *argv[], js_value_t **self, void **data) {
  if (argv) {
    size_t i = 0, n = info->argc < *argc ? info->argc : *argc;

    for (; i < n; i++) {
      js_value_t *wrapper = malloc(sizeof(js_value_t));

      wrapper->value = JS_DupValue(env->context, info->argv[i]);

      argv[i] = wrapper;

      js_attach_to_handle_scope(env, env->scope, wrapper);
    }

    n = *argc;

    if (i < n) {
      js_value_t *wrapper = malloc(sizeof(js_value_t));

      wrapper->value = JS_UNDEFINED;

      js_attach_to_handle_scope(env, env->scope, wrapper);

      for (; i < n; i++) {
        argv[i] = wrapper;
      }
    }
  }

  if (argc) {
    *argc = info->argc;
  }

  if (self) {
    js_value_t *wrapper = malloc(sizeof(js_value_t));

    wrapper->value = JS_DupValue(env->context, info->self);

    *self = wrapper;

    js_attach_to_handle_scope(env, env->scope, wrapper);
  }

  if (data) {
    *data = info->callback->data;
  }

  return 0;
}

int
js_get_arraybuffer_info (js_env_t *env, js_value_t *arraybuffer, void **pdata, size_t *plen) {
  size_t len;

  uint8_t *data = JS_GetArrayBuffer(env->context, &len, arraybuffer->value);

  if (pdata) {
    *pdata = data;
  }

  if (plen) {
    *plen = len;
  }

  return 0;
}

int
js_get_typedarray_info (js_env_t *env, js_value_t *typedarray, js_typedarray_type_t *ptype, void **pdata, size_t *plen, js_value_t **parraybuffer, size_t *poffset) {
  size_t len, offset, byte_len, bytes_per_element;

  JSValue arraybuffer = JS_GetTypedArrayBuffer(env->context, typedarray->value, &offset, &byte_len, &bytes_per_element);

  if (ptype) {
    JSValue global = JS_GetGlobalObject(env->context);

#define check_type(class, type) \
  { \
    JSValue constructor = JS_GetPropertyStr(env->context, global, class); \
\
    if (JS_IsInstanceOf(env->context, typedarray->value, constructor)) { \
      *ptype = type; \
\
      JS_FreeValue(env->context, constructor); \
\
      goto done; \
    } \
\
    JS_FreeValue(env->context, constructor); \
  }

    check_type("Int8Array", js_int8_array);
    check_type("Uint8Array", js_uint8_array);
    check_type("Uint8ClampedArray", js_uint8_clamped_array);
    check_type("Int16Array", js_int16_array);
    check_type("Uint16Array", js_uint16_array);
    check_type("Int32Array", js_int32_array);
    check_type("Uint32Array", js_uint32_array);
    check_type("Float32Array", js_float32_array);
    check_type("Float64Array", js_float64_array);
    check_type("BigInt64Array", js_bigint64_array);
    check_type("BigUint64Array", js_biguint64_array);

#undef check_type

  done:
    JS_FreeValue(env->context, global);
  }

  uint8_t *data;

  if (pdata || parraybuffer) {
    data = JS_GetArrayBuffer(env->context, &len, arraybuffer);
  }

  if (pdata) {
    *pdata = data;
  }

  if (plen) {
    *plen = byte_len / bytes_per_element;
  }

  if (parraybuffer) {
    js_value_t *wrapper = malloc(sizeof(js_value_t));

    wrapper->value = JS_DupValue(env->context, arraybuffer);

    *parraybuffer = wrapper;

    js_attach_to_handle_scope(env, env->scope, wrapper);
  }

  if (poffset) {
    *poffset = offset;
  }

  JS_FreeValue(env->context, arraybuffer);

  return 0;
}

int
js_get_dataview_info (js_env_t *env, js_value_t *dataview, void **pdata, size_t *plen, js_value_t **parraybuffer, size_t *poffset) {
  return -1;
}

int
js_call_function (js_env_t *env, js_value_t *recv, js_value_t *fn, size_t argc, js_value_t *const argv[], js_value_t **result) {
  JSValue *args = malloc(argc * sizeof(JSValue));

  for (size_t i = 0; i < argc; i++) {
    args[i] = argv[i]->value;
  }

  JSValue value = JS_Call(env->context, fn->value, recv->value, argc, args);

  free(args);

  if (JS_IsException(value)) return -1;

  if (result == NULL) JS_FreeValue(env->context, value);
  else {
    js_value_t *wrapper = malloc(sizeof(js_value_t));

    wrapper->value = value;

    *result = wrapper;

    js_attach_to_handle_scope(env, env->scope, wrapper);
  }

  return 0;
}

int
js_make_callback (js_env_t *env, js_value_t *recv, js_value_t *fn, size_t argc, js_value_t *const argv[], js_value_t **result) {
  int err = js_call_function(env, recv, fn, argc, argv, result);

  run_microtasks(env);

  return err;
}

int
js_throw (js_env_t *env, js_value_t *error) {
  JS_Throw(env->context, error->value);

  return 0;
}

int
js_vformat (char **result, size_t *size, const char *message, va_list args) {
  int res = vsnprintf(NULL, 0, message, args);
  if (res < 0) return res;

  *size = res + 1 /* NULL */;
  *result = malloc(*size);

  vsnprintf(*result, *size, message, args);

  return 0;
}

int
js_throw_error (js_env_t *env, const char *code, const char *message) {
  JSValue global = JS_GetGlobalObject(env->context);
  JSValue constructor = JS_GetPropertyStr(env->context, global, "Error");

  JSValue arg = JS_NewString(env->context, message);

  JSValue error = JS_CallConstructor(env->context, constructor, 1, &arg);

  if (code) {
    JS_SetPropertyStr(env->context, error, "code", JS_NewString(env->context, code));
  }

  JS_FreeValue(env->context, arg);
  JS_FreeValue(env->context, constructor);
  JS_FreeValue(env->context, global);

  JS_Throw(env->context, error);

  return 0;
}

int
js_throw_verrorf (js_env_t *env, const char *code, const char *message, va_list args) {
  size_t len;
  char *formatted;
  js_vformat(&formatted, &len, message, args);

  int err = js_throw_error(env, code, formatted);

  free(formatted);

  return err;
}

int
js_throw_type_error (js_env_t *env, const char *code, const char *message) {
  JSValue global = JS_GetGlobalObject(env->context);
  JSValue constructor = JS_GetPropertyStr(env->context, global, "TypeError");

  JSValue arg = JS_NewString(env->context, message);

  JSValue error = JS_CallConstructor(env->context, constructor, 1, &arg);

  if (code) {
    JS_SetPropertyStr(env->context, error, "code", JS_NewString(env->context, code));
  }

  JS_FreeValue(env->context, arg);
  JS_FreeValue(env->context, constructor);
  JS_FreeValue(env->context, global);

  JS_Throw(env->context, error);

  return 0;
}

int
js_throw_type_verrorf (js_env_t *env, const char *code, const char *message, va_list args) {
  size_t len;
  char *formatted;
  js_vformat(&formatted, &len, message, args);

  int err = js_throw_type_error(env, code, formatted);

  free(formatted);

  return err;
}

int
js_throw_range_error (js_env_t *env, const char *code, const char *message) {
  JSValue global = JS_GetGlobalObject(env->context);
  JSValue constructor = JS_GetPropertyStr(env->context, global, "RangeError");

  JSValue arg = JS_NewString(env->context, message);

  JSValue error = JS_CallConstructor(env->context, constructor, 1, &arg);

  if (code) {
    JS_SetPropertyStr(env->context, error, "code", JS_NewString(env->context, code));
  }

  JS_FreeValue(env->context, arg);
  JS_FreeValue(env->context, constructor);
  JS_FreeValue(env->context, global);

  JS_Throw(env->context, error);

  return 0;
}

int
js_throw_range_verrorf (js_env_t *env, const char *code, const char *message, va_list args) {
  size_t len;
  char *formatted;
  js_vformat(&formatted, &len, message, args);

  int err = js_throw_range_error(env, code, formatted);

  free(formatted);

  return err;
}

int
js_throw_syntax_error (js_env_t *env, const char *code, const char *message) {
  JSValue global = JS_GetGlobalObject(env->context);
  JSValue constructor = JS_GetPropertyStr(env->context, global, "SyntaxError");

  JSValue arg = JS_NewString(env->context, message);

  JSValue error = JS_CallConstructor(env->context, constructor, 1, &arg);

  if (code) {
    JS_SetPropertyStr(env->context, error, "code", JS_NewString(env->context, code));
  }

  JS_FreeValue(env->context, arg);
  JS_FreeValue(env->context, constructor);
  JS_FreeValue(env->context, global);

  JS_Throw(env->context, error);

  return 0;
}

int
js_throw_syntax_verrorf (js_env_t *env, const char *code, const char *message, va_list args) {
  size_t len;
  char *formatted;
  js_vformat(&formatted, &len, message, args);

  int err = js_throw_range_error(env, code, formatted);

  free(formatted);

  return err;
}

int
js_is_exception_pending (js_env_t *env, bool *result) {
  JSValue error = JS_GetException(env->context);

  if (JS_IsNull(error)) *result = false;
  else {
    JS_Throw(env->context, error);

    *result = true;
  }

  return 0;
}

int
js_get_and_clear_last_exception (js_env_t *env, js_value_t **result) {
  JSValue error = JS_GetException(env->context);

  if (JS_IsNull(error)) return js_get_undefined(env, result);

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = error;

  *result = wrapper;

  js_attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_fatal_exception (js_env_t *env, js_value_t *error) {
  on_uncaught_exception(env->context, error->value);

  return 0;
}

static JSValue
on_job (JSContext *context, int argc, JSValueConst *argv) {
  js_env_t *env = (js_env_t *) JS_GetContextOpaque(context);

  js_task_t *task = (js_task_t *) JS_GetOpaque(*argv, js_job_data_class_id);

  task->cb(env, task->data);

  return JS_NULL;
}

int
js_queue_microtask (js_env_t *env, js_task_cb cb, void *data) {
  js_task_t *task = malloc(sizeof(js_task_t));

  task->env = env;
  task->cb = cb;
  task->data = data;

  JSValue external = JS_NewObjectClass(env->context, js_job_data_class_id);

  JS_SetOpaque(external, task);

  JS_EnqueueJob(env->context, on_job, 1, &external);

  JS_FreeValue(env->context, external);

  return 0;
}

int
js_queue_macrotask (js_env_t *env, js_task_cb cb, void *data, uint64_t delay) {
  return -1;
}

int
js_adjust_external_memory (js_env_t *env, int64_t change_in_bytes, int64_t *result) {
  return -1;
}

int
js_request_garbage_collection (js_env_t *env) {
  if (!env->platform->options.expose_garbage_collection) {
    js_throw_error(env, NULL, "Garbage collection is unavailable");

    return -1;
  }

  JS_RunGC(env->runtime);

  return 0;
}
