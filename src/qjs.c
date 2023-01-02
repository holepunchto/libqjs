#include <js.h>
#include <quickjs.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <uv.h>

typedef struct js_task_s js_task_t;
typedef struct js_callback_s js_callback_t;
typedef struct js_external_s js_external_t;
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
  JSContext *context;
  JSValue value;
  uint32_t count;
};

struct js_deferred_s {
  JSValue resolve;
  JSValue reject;
};

struct js_external_s {
  js_env_t *env;
  void *data;
  js_finalize_cb finalize_cb;
  void *finalize_hint;
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
  js_external_t *external = (js_external_t *) JS_GetOpaque(value, js_external_data_class_id);

  if (external->finalize_cb) {
    external->finalize_cb(external->env, external->data, external->finalize_hint);
  }

  free(external);
}

static void
on_prepare (uv_prepare_t *handle);

static inline void
run_microtasks (js_env_t *env) {
  JSContext *context;

  for (;;) {
    int err = JS_ExecutePendingJob(env->runtime, &context);
    if (err <= 0) break;
  }
}

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
  if (env->scope != scope) return -1;

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

int
js_escape_handle (js_env_t *env, js_escapable_handle_scope_t *scope, js_value_t *escapee, js_value_t **result) {
  if (scope->escaped) return -1;

  scope->escaped = true;

  JS_DupValue(env->context, escapee->value);

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = escapee->value;

  *result = wrapper;

  js_attach_to_handle_scope(env, scope->scope->parent, wrapper);

  return 0;
}

int
js_run_script (js_env_t *env, js_value_t *source, js_value_t **result) {
  size_t str_len;
  const char *str = JS_ToCStringLen(env->context, &str_len, source->value);

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = JS_Eval(env->context, str, str_len, "<anonymous>", JS_EVAL_TYPE_GLOBAL);

  JS_FreeCString(env->context, str);

  *result = wrapper;

  js_attach_to_handle_scope(env, env->scope, wrapper);

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
js_create_synthetic_module (js_env_t *env, const char *name, size_t len, const js_value_t *export_names[], size_t names_len, js_synthetic_module_cb cb, void *data, js_module_t **result) {
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

  JS_DupValue(env->context, value->value);

  JS_SetModuleExport(env->context, module->definition, str, value->value);

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

int
js_create_reference (js_env_t *env, js_value_t *value, uint32_t count, js_ref_t **result) {
  js_ref_t *reference = malloc(sizeof(js_ref_t));

  reference->context = env->context;
  reference->value = value->value;
  reference->count = count;

  if (reference->count > 0) JS_DupValue(reference->context, reference->value);

  *result = reference;

  return 0;
}

int
js_delete_reference (js_env_t *env, js_ref_t *reference) {
  if (reference->count > 0) JS_FreeValue(reference->context, reference->value);

  free(reference);

  return 0;
}

int
js_reference_ref (js_env_t *env, js_ref_t *reference, uint32_t *result) {
  if (reference->count == 0) return -1;

  reference->count++;

  if (result != NULL) {
    *result = reference->count;
  }

  return 0;
}

int
js_reference_unref (js_env_t *env, js_ref_t *reference, uint32_t *result) {
  if (reference->count == 0) return -1;

  reference->count--;

  if (reference->count == 0) JS_FreeValue(reference->context, reference->value);

  if (result != NULL) {
    *result = reference->count;
  }

  return 0;
}

int
js_get_reference_value (js_env_t *env, js_ref_t *reference, js_value_t **result) {
  if (reference->count == 0) {
    *result = NULL;
  } else {
    JS_DupValue(reference->context, reference->value);

    js_value_t *wrapper = malloc(sizeof(js_value_t));

    wrapper->value = reference->value;

    *result = wrapper;

    js_attach_to_handle_scope(env, env->scope, wrapper);
  }

  return 0;
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

  JSValue value;

  if (result == NULL) value = JS_UNDEFINED;
  else {
    value = result->value;

    JS_DupValue(env->context, value);
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
js_create_external (js_env_t *env, void *data, js_finalize_cb finalize_cb, void *finalize_hint, js_value_t **result) {
  js_external_t *external = malloc(sizeof(js_external_t));

  external->env = env;
  external->data = data;
  external->finalize_cb = finalize_cb;
  external->finalize_hint = finalize_hint;

  JSValue value = JS_NewObjectClass(env->context, js_external_data_class_id);

  JS_SetOpaque(value, external);

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = value;

  *result = wrapper;

  js_attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_create_promise (js_env_t *env, js_deferred_t **deferred, js_value_t **promise) {
  *deferred = malloc(sizeof(js_deferred_t));

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = JS_NewPromiseCapability(
    env->context,
    (JSValue[]){
      (*deferred)->resolve,
      (*deferred)->reject,
    }
  );

  *promise = wrapper;

  js_attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_resolve_deferred (js_env_t *env, js_deferred_t *deferred, js_value_t *resolution) {
  JS_DupValue(env->context, resolution->value);

  JSValue result = JS_Call(env->context, deferred->resolve, JS_UNDEFINED, 1, &resolution->value);

  JS_FreeValue(env->context, result);

  JS_FreeValue(env->context, deferred->resolve);
  JS_FreeValue(env->context, deferred->reject);

  free(deferred);

  return 0;
}

int
js_reject_deferred (js_env_t *env, js_deferred_t *deferred, js_value_t *resolution) {
  JS_DupValue(env->context, resolution->value);

  JSValue result = JS_Call(env->context, deferred->reject, JS_UNDEFINED, 1, &resolution->value);

  JS_FreeValue(env->context, result);

  JS_FreeValue(env->context, deferred->resolve);
  JS_FreeValue(env->context, deferred->reject);

  free(deferred);

  return 0;
}

int
js_get_promise_state (js_env_t *env, js_value_t *promise, js_promise_state_t *result) {
  return -1;
}

int
js_get_promise_result (js_env_t *env, js_value_t *promise, js_value_t **result) {
  return -1;
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
js_is_array (js_env_t *env, js_value_t *value, bool *result) {
  *result = JS_IsArray(env->context, value->value);

  return 0;
}

int
js_is_arraybuffer (js_env_t *env, js_value_t *value, bool *result) {
  return -1;
}

int
js_is_number (js_env_t *env, js_value_t *value, bool *result) {
  *result = JS_IsNumber(value->value);

  return 0;
}

int
js_is_bigint (js_env_t *env, js_value_t *value, bool *result) {
  *result = JS_IsBigInt(env->context, value->value);

  return 0;
}

int
js_is_null (js_env_t *env, js_value_t *value, bool *result) {
  *result = JS_IsNull(value->value);

  return 0;
}

int
js_is_undefined (js_env_t *env, js_value_t *value, bool *result) {
  *result = JS_IsUndefined(value->value);

  return 0;
}

int
js_is_symbol (js_env_t *env, js_value_t *value, bool *result) {
  *result = JS_IsSymbol(value->value);

  return 0;
}

int
js_is_boolean (js_env_t *env, js_value_t *value, bool *result) {
  *result = JS_IsBool(value->value);

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
js_is_string (js_env_t *env, js_value_t *value, bool *result) {
  *result = JS_IsString(value->value);

  return 0;
}

int
js_is_function (js_env_t *env, js_value_t *value, bool *result) {
  *result = JS_IsFunction(env->context, value->value);

  return 0;
}

int
js_is_object (js_env_t *env, js_value_t *value, bool *result) {
  *result = JS_IsObject(value->value);

  return 0;
}

int
js_is_date (js_env_t *env, js_value_t *value, bool *result) {
  return -1;
}

int
js_is_error (js_env_t *env, js_value_t *value, bool *result) {
  return -1;
}

int
js_is_typedarray (js_env_t *env, js_value_t *value, bool *result) {
  return -1;
}

int
js_is_dataview (js_env_t *env, js_value_t *value, bool *result) {
  return -1;
}

int
js_is_promise (js_env_t *env, js_value_t *value, bool *result) {
  return -1;
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
js_get_null (js_env_t *env, js_value_t **result) {
  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = JS_NULL;

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
js_get_boolean (js_env_t *env, bool value, js_value_t **result) {
  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = value ? JS_TRUE : JS_FALSE;

  *result = wrapper;

  js_attach_to_handle_scope(env, env->scope, wrapper);

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
js_get_value_string_utf8 (js_env_t *env, js_value_t *value, char *str, size_t len, size_t *result) {
  size_t cstr_len;
  const char *cstr = JS_ToCStringLen(env->context, &cstr_len, value->value);

  if (str == NULL) {
    *result = cstr_len;
  } else if (len != 0) {
    len = cstr_len < len - 1 ? cstr_len : len - 1;

    strncpy(str, cstr, len);

    str[len] = '\0';

    if (result != NULL) {
      *result = len;
    }
  } else if (result != NULL) {
    *result = 0;
  }

  JS_FreeCString(env->context, cstr);

  return 0;
}

int
js_get_value_external (js_env_t *env, js_value_t *value, void **result) {
  js_external_t *external = (js_external_t *) JS_GetOpaque(value->value, js_external_data_class_id);

  *result = external->data;

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
js_set_named_property (js_env_t *env, js_value_t *object, const char *name, js_value_t *value) {
  JS_DupValue(env->context, value->value);

  JS_SetPropertyStr(env->context, object->value, name, value->value);

  return 0;
}

int
js_call_function (js_env_t *env, js_value_t *recv, js_value_t *fn, size_t argc, const js_value_t *argv[], js_value_t **result) {
  JSValue *args = malloc(argc * sizeof(JSValue));

  for (size_t i = 0; i < argc; i++) {
    args[i] = argv[i]->value;
  }

  JSValue value = JS_Call(env->context, fn->value, recv->value, argc, args);

  free(args);

  if (JS_IsException(value)) {
    JS_FreeValue(env->context, value); // TODO: Expose this

    return -1;
  }

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = value;

  *result = wrapper;

  js_attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_make_callback (js_env_t *env, js_value_t *recv, js_value_t *fn, size_t argc, const js_value_t *argv[], js_value_t **result) {
  int err = js_call_function(env, recv, fn, argc, argv, result);

  run_microtasks(env);

  return err;
}

int
js_get_callback_info (js_env_t *env, const js_callback_info_t *info, size_t *argc, js_value_t *argv[], js_value_t **self, void **data) {
  if (argv != NULL) {
    size_t n = info->argc < *argc ? info->argc : *argc;

    for (size_t i = 0; i < n; i++) {
      JS_DupValue(env->context, info->argv[i]);

      js_value_t *wrapper = malloc(sizeof(js_value_t));

      wrapper->value = info->argv[i];

      argv[i] = wrapper;

      js_attach_to_handle_scope(env, env->scope, wrapper);
    }
  }

  if (argc != NULL) {
    *argc = info->argc;
  }

  if (self != NULL) {
    JS_DupValue(env->context, info->self);

    js_value_t *wrapper = malloc(sizeof(js_value_t));

    wrapper->value = info->self;

    *self = wrapper;

    js_attach_to_handle_scope(env, env->scope, wrapper);
  }

  if (data != NULL) {
    *data = info->callback->data;
  }

  return 0;
}

int
js_get_arraybuffer_info (js_env_t *env, js_value_t *arraybuffer, void **data, size_t *len) {
  return -1;
}

int
js_get_typedarray_info (js_env_t *env, js_value_t *typedarray, js_typedarray_type_t *type, size_t *len, void **data, js_value_t **arraybuffer, size_t *offset) {
  return -1;
}

int
js_get_dataview_info (js_env_t *env, js_value_t *dataview, size_t *len, void **data, js_value_t **arraybuffer, size_t *offset) {
  return -1;
}

int
js_throw (js_env_t *env, js_value_t *error) {
  JSValue result = JS_Throw(env->context, error->value);

  JS_FreeValue(env->context, result);

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
js_request_garbage_collection (js_env_t *env) {
  if (!env->platform->options.expose_garbage_collection) return -1;

  JS_RunGC(env->runtime);

  return 0;
}
