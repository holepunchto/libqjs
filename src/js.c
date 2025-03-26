#include <assert.h>
#include <intrusive.h>
#include <intrusive/list.h>
#include <js.h>
#include <math.h>
#include <quickjs.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <utf.h>
#include <uv.h>
#include <wchar.h>

typedef struct js_callback_s js_callback_t;
typedef struct js_finalizer_s js_finalizer_t;
typedef struct js_finalizer_list_s js_finalizer_list_t;
typedef struct js_delegate_s js_delegate_t;
typedef struct js_module_resolver_s js_module_resolver_t;
typedef struct js_module_evaluator_s js_module_evaluator_t;
typedef struct js_arraybuffer_header_s js_arraybuffer_header_t;
typedef struct js_promise_rejection_s js_promise_rejection_t;
typedef struct js_threadsafe_queue_s js_threadsafe_queue_t;
typedef struct js_teardown_task_s js_teardown_task_t;
typedef struct js_teardown_queue_s js_teardown_queue_t;

struct js_deferred_teardown_s {
  js_env_t *env;
};

struct js_teardown_task_s {
  enum {
    js_immediate_teardown,
    js_deferred_teardown,
  } type;

  union {
    struct {
      js_teardown_cb cb;
    } immediate;

    struct {
      js_deferred_teardown_t handle;
      js_deferred_teardown_cb cb;
    } deferred;
  };

  void *data;
  intrusive_list_node_t list;
};

struct js_teardown_queue_s {
  intrusive_list_t tasks;
};

struct js_platform_s {
  js_platform_options_t options;
  uv_loop_t *loop;
};

struct js_env_s {
  uv_loop_t *loop;
  uv_prepare_t prepare;
  uv_check_t check;
  uv_async_t teardown;
  int active_handles;

  js_platform_t *platform;
  js_handle_scope_t *scope;

  uint32_t refs;
  uint32_t depth;

  JSRuntime *runtime;
  JSContext *context;
  JSValue bindings;

  int64_t external_memory;

  js_module_resolver_t *resolvers;
  js_module_evaluator_t *evaluators;

  bool destroying;

  js_promise_rejection_t *promise_rejections;

  js_teardown_queue_t teardown_queue;

  struct {
    JSClassID external;
    JSClassID finalizer;
    JSClassID type_tag;
    JSClassID function;
    JSClassID constructor;
    JSClassID delegate;
  } classes;

  struct {
    js_uncaught_exception_cb uncaught_exception;
    void *uncaught_exception_data;

    js_unhandled_rejection_cb unhandled_rejection;
    void *unhandled_rejection_data;

    js_dynamic_import_cb dynamic_import;
    void *dynamic_import_data;
  } callbacks;
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
  js_handle_scope_t *parent;
};

struct js_module_s {
  JSContext *context;
  JSValue source;
  JSValue bytecode;
  JSModuleDef *definition;
  js_module_meta_cb meta;
  void *meta_data;
  char *name;
};

struct js_module_resolver_s {
  js_module_t *module;
  js_module_resolve_cb cb;
  void *data;
  js_module_resolver_t *next;
};

struct js_module_evaluator_s {
  js_module_t *module;
  js_module_evaluate_cb cb;
  void *data;
  js_module_evaluator_t *next;
};

struct js_ref_s {
  JSValue value;
  JSValue symbol;
  uint32_t count;
  bool finalized;
};

struct js_deferred_s {
  JSValue resolve;
  JSValue reject;
};

struct js_string_view_s {
  const char *value;
};

struct js_finalizer_s {
  void *data;
  js_finalize_cb finalize_cb;
  void *finalize_hint;
};

struct js_finalizer_list_s {
  js_finalizer_t finalizer;
  js_finalizer_list_t *next;
};

struct js_delegate_s {
  js_delegate_callbacks_t callbacks;
  void *data;
  js_finalize_cb finalize_cb;
  void *finalize_hint;
};

struct js_callback_s {
  js_function_cb cb;
  void *data;
};

struct js_callback_info_s {
  js_callback_t *callback;
  int argc;
  JSValue *argv;
  JSValue receiver;
  JSValue new_target;
};

struct js_arraybuffer_header_s {
  atomic_int references;
  size_t len;
  uint8_t data[];
};

struct js_arraybuffer_backing_store_s {
  atomic_int references;
  size_t len;
  uint8_t *data;
  JSValue owner;
};

struct js_promise_rejection_s {
  JSValue promise;
  JSValue reason;
  js_promise_rejection_t *next;
};

static const uint8_t js_threadsafe_function_idle = 0x0;
static const uint8_t js_threadsafe_function_running = 0x1;
static const uint8_t js_threadsafe_function_pending = 0x2;

struct js_threadsafe_queue_s {
  void **queue;
  size_t len;
  size_t capacity;
  bool closed;
  uv_mutex_t lock;
};

struct js_threadsafe_function_s {
  JSValue function;
  js_env_t *env;
  uv_async_t async;
  js_threadsafe_queue_t queue;
  atomic_int state;
  atomic_int thread_count;
  void *context;
  js_finalize_cb finalize_cb;
  void *finalize_hint;
  js_threadsafe_function_cb cb;
};

static const char *js_platform_identifier = "quickjs";

static const char *js_platform_version = "2021-03-27";

int
js_create_platform(uv_loop_t *loop, const js_platform_options_t *options, js_platform_t **result) {
  js_platform_t *platform = malloc(sizeof(js_platform_t));

  platform->loop = loop;
  platform->options = options ? *options : (js_platform_options_t) {};

  *result = platform;

  return 0;
}

int
js_destroy_platform(js_platform_t *platform) {
  free(platform);

  return 0;
}

int
js_get_platform_identifier(js_platform_t *platform, const char **result) {
  *result = js_platform_identifier;

  return 0;
}

int
js_get_platform_version(js_platform_t *platform, const char **result) {
  *result = js_platform_version;

  return 0;
}

int
js_get_platform_loop(js_platform_t *platform, uv_loop_t **result) {
  *result = platform->loop;

  return 0;
}

static void
js__on_external_finalize(JSRuntime *runtime, JSValue value);

static JSClassDef js_external_class = {
  .class_name = "External",
  .finalizer = js__on_external_finalize,
};

static void
js__on_finalizer_finalize(JSRuntime *runtime, JSValue value);

static JSClassDef js_finalizer_class = {
  .class_name = "Finalizer",
  .finalizer = js__on_finalizer_finalize,
};

static void
js__on_type_tag_finalize(JSRuntime *runtime, JSValue value);

static JSClassDef js_type_tag_class = {
  .class_name = "TypeTag",
  .finalizer = js__on_type_tag_finalize,
};

static void
js__on_function_finalize(JSRuntime *runtime, JSValue value);

static JSClassDef js_function_class = {
  .class_name = "Function",
  .finalizer = js__on_function_finalize,
};

static void
js__on_constructor_finalize(JSRuntime *runtime, JSValue value);

static JSClassDef js_constructor_class = {
  .class_name = "Constructor",
  .finalizer = js__on_constructor_finalize,
};

static int
js__on_delegate_get_own_property(JSContext *context, JSPropertyDescriptor *descriptor, JSValueConst object, JSAtom property);

static int
js__on_delegate_get_own_property_names(JSContext *context, JSPropertyEnum **properties, uint32_t *len, JSValueConst object);

static int
js__on_delegate_delete_property(JSContext *context, JSValueConst object, JSAtom property);

static int
js__on_delegate_set_property(JSContext *context, JSValueConst object, JSAtom property, JSValueConst value, JSValueConst receiver, int flags);

static void
js__on_delegate_finalize(JSRuntime *runtime, JSValue value);

static JSClassDef js_delegate_class = {
  .class_name = "Delegate",
  .finalizer = js__on_delegate_finalize,
  .exotic = &(JSClassExoticMethods) {
    .get_own_property = js__on_delegate_get_own_property,
    .get_own_property_names = js__on_delegate_get_own_property_names,
    .delete_property = js__on_delegate_delete_property,
    .set_property = js__on_delegate_set_property,
  },
};

static JSModuleDef *
js__on_resolve_module(JSContext *context, const char *name, void *opaque) {
  int err;

  js_env_t *env = (js_env_t *) JS_GetContextOpaque(context);

  js_module_resolver_t *resolver = env->resolvers;

  js_handle_scope_t *scope;
  err = js_open_handle_scope(env, &scope);
  assert(err == 0);

  js_module_t *module;

  js_value_t specifier = {
    .value = JS_NewString(env->context, name),
  };

  js_value_t assertions = {
    .value = JS_NULL,
  };

  JSModuleDef *definition = NULL;

  if (resolver) {
    module = resolver->cb(
      env,
      &specifier,
      &assertions,
      resolver->module,
      resolver->data
    );

    if (module == NULL) goto done;

    if (module->definition == NULL) {
      err = js_instantiate_module(
        env,
        module,
        resolver->cb,
        resolver->data
      );

      if (err < 0) goto done;
    }

    definition = module->definition;
  } else {
    if (env->callbacks.dynamic_import == NULL) {
      err = js_throw_error(env, NULL, "Dynamic import() is not supported");
      assert(err == 0);

      goto done;
    }

    js_value_t referrer = {
      .value = JS_NULL,
    };

    module = env->callbacks.dynamic_import(
      env,
      &specifier,
      &assertions,
      &referrer,
      env->callbacks.dynamic_import_data
    );

    if (module == NULL) goto done;

    definition = module->definition;
  }

done:
  JS_FreeValue(env->context, specifier.value);

  err = js_close_handle_scope(env, scope);
  assert(err == 0);

  return definition;
}

static void
js__on_uncaught_exception(JSContext *context, JSValue error) {
  int err;

  js_env_t *env = (js_env_t *) JS_GetContextOpaque(context);

  if (env->callbacks.uncaught_exception) {
    js_handle_scope_t *scope;
    err = js_open_handle_scope(env, &scope);
    assert(err == 0);

    env->callbacks.uncaught_exception(
      env,
      &(js_value_t) {error},
      env->callbacks.uncaught_exception_data
    );

    err = js_close_handle_scope(env, scope);
    assert(err == 0);

    JS_FreeValue(context, error);
  } else {
    JS_Throw(context, error);
  }
}

static void
js__on_unhandled_rejection(JSContext *context, JSValue promise, JSValue reason) {
  int err;

  js_env_t *env = (js_env_t *) JS_GetContextOpaque(context);

  if (env->callbacks.unhandled_rejection) {
    js_handle_scope_t *scope;
    err = js_open_handle_scope(env, &scope);
    assert(err == 0);

    env->callbacks.unhandled_rejection(
      env,
      &(js_value_t) {reason},
      &(js_value_t) {promise},
      env->callbacks.unhandled_rejection_data
    );

    err = js_close_handle_scope(env, scope);
    assert(err == 0);
  }

  JS_FreeValue(context, promise);
  JS_FreeValue(context, reason);
}

static void
js__on_promise_rejection(JSContext *context, JSValueConst promise, JSValueConst reason, bool is_handled, void *opaque) {
  js_env_t *env = (js_env_t *) JS_GetContextOpaque(context);

  if (env->callbacks.unhandled_rejection == NULL) return;

  if (is_handled) {
    js_promise_rejection_t *next = env->promise_rejections;
    js_promise_rejection_t *prev = NULL;

    while (next) {
      if (JS_VALUE_GET_OBJ(next->promise) == JS_VALUE_GET_OBJ(promise)) {
        JS_FreeValue(context, next->promise);
        JS_FreeValue(context, next->reason);

        if (prev) prev->next = next->next;
        else env->promise_rejections = next->next;

        return free(next);
      }

      prev = next;
      next = next->next;
    }
  } else {
    js_promise_rejection_t *node = malloc(sizeof(js_promise_rejection_t));

    node->promise = JS_DupValue(context, promise);
    node->reason = JS_DupValue(context, reason);
    node->next = env->promise_rejections;

    env->promise_rejections = node;
  }
}

static inline void
js__on_run_microtasks(js_env_t *env) {
  int err;

  js_handle_scope_t *scope;
  err = js_open_handle_scope(env, &scope);
  assert(err == 0);

  JSContext *context;

  for (;;) {
    err = JS_ExecutePendingJob(env->runtime, &context);

    if (err == 0) break;

    if (err < 0) {
      JSValue error = JS_GetException(context);

      js__on_uncaught_exception(context, error);
    }
  }

  js_promise_rejection_t *next = env->promise_rejections;
  js_promise_rejection_t *prev = NULL;

  env->promise_rejections = NULL;

  while (next) {
    js__on_unhandled_rejection(env->context, next->promise, next->reason);

    prev = next;
    next = next->next;

    free(prev);
  }

  err = js_close_handle_scope(env, scope);
  assert(err == 0);
}

static void
js__on_prepare(uv_prepare_t *handle);

static inline void
js__on_check_liveness(js_env_t *env) {
  int err;

  if (true /* macrotask queue empty */) {
    err = uv_prepare_stop(&env->prepare);
  } else {
    err = uv_prepare_start(&env->prepare, js__on_prepare);
  }

  assert(err == 0);
}

static void
js__on_prepare(uv_prepare_t *handle) {
  js_env_t *env = (js_env_t *) handle->data;

  js__on_check_liveness(env);
}

static void
js__on_check(uv_check_t *handle) {
  js_env_t *env = (js_env_t *) handle->data;

  if (uv_loop_alive(env->loop)) return;

  js__on_check_liveness(env);
}

static void *
js__on_calloc(void *opaque, size_t count, size_t size) {
  return calloc(count, size);
}

static void *
js__on_malloc(void *opaque, size_t size) {
  return malloc(size);
}

static void
js__on_free(void *opaque, void *ptr) {
  free(ptr);
}

static void *
js__on_realloc(void *opaque, void *ptr, size_t size) {
  return realloc(ptr, size);
}

static size_t
js__on_usable_size(const void *ptr) {
  return 0;
}

static void *
js__on_shared_malloc(void *opaque, size_t size) {
  js_arraybuffer_header_t *header = malloc(sizeof(js_arraybuffer_header_t) + size);

  header->references = 1;
  header->len = size;

  return header->data;
}

static void
js__on_shared_free(void *opaque, void *ptr) {
  js_arraybuffer_header_t *header = (js_arraybuffer_header_t *) ((char *) ptr - sizeof(js_arraybuffer_header_t));

  if (--header->references == 0) {
    free(header);
  }
}

static void
js__on_shared_dup(void *opaque, void *ptr) {
  js_arraybuffer_header_t *header = (js_arraybuffer_header_t *) ((char *) ptr - sizeof(js_arraybuffer_header_t));

  header->references++;
}

static void
js__on_handle_close(uv_handle_t *handle) {
  js_env_t *env = (js_env_t *) handle->data;

  if (--env->active_handles == 0) {
    js_module_evaluator_t *evaluator = env->evaluators;

    while (evaluator) {
      js_module_evaluator_t *next = evaluator->next;

      free(evaluator);

      evaluator = next;
    }

    free(env);
  }
}

static void
js__close_env(js_env_t *env) {
  JS_FreeValue(env->context, env->bindings);
  JS_FreeContext(env->context);
  JS_FreeRuntime(env->runtime);

  uv_close((uv_handle_t *) &env->prepare, js__on_handle_close);
  uv_close((uv_handle_t *) &env->check, js__on_handle_close);
  uv_close((uv_handle_t *) &env->teardown, js__on_handle_close);
}

static void
js__on_teardown(uv_async_t *handle) {
  js_env_t *env = (js_env_t *) handle->data;

  if (env->refs == 0) js__close_env(env);
}

int
js_create_env(uv_loop_t *loop, js_platform_t *platform, const js_env_options_t *options, js_env_t **result) {
  int err;

  JSRuntime *runtime = JS_NewRuntime2(
    &(JSMallocFunctions) {
      .js_calloc = js__on_calloc,
      .js_malloc = js__on_malloc,
      .js_free = js__on_free,
      .js_realloc = js__on_realloc,
      .js_malloc_usable_size = js__on_usable_size,
    },
    NULL
  );

  JS_SetSharedArrayBufferFunctions(
    runtime,
    &(JSSharedArrayBufferFunctions) {
      .sab_alloc = js__on_shared_malloc,
      .sab_free = js__on_shared_free,
      .sab_dup = js__on_shared_dup,
      .sab_opaque = NULL,
    }
  );

  JS_SetMaxStackSize(runtime, 0);
  JS_SetCanBlock(runtime, false);
  JS_SetModuleLoaderFunc(runtime, NULL, js__on_resolve_module, NULL);
  JS_SetHostPromiseRejectionTracker(runtime, js__on_promise_rejection, NULL);

  if (options && options->memory_limit) {
    JS_SetMemoryLimit(runtime, options->memory_limit);
  } else {
    uint64_t constrained_memory = uv_get_constrained_memory();
    uint64_t total_memory = uv_get_total_memory();

    if (constrained_memory > 0 && constrained_memory < total_memory) {
      total_memory = constrained_memory;
    }

    if (total_memory > 0) {
      JS_SetMemoryLimit(runtime, total_memory);
    }
  }

  js_env_t *env = malloc(sizeof(js_env_t));

  env->loop = loop;
  env->active_handles = 3;

  env->platform = platform;
  env->scope = NULL;

  env->refs = 0;
  env->depth = 0;

  env->runtime = runtime;
  env->context = JS_NewContext(runtime);
  env->bindings = JS_NewObject(env->context);

  env->external_memory = 0;

  env->resolvers = NULL;
  env->evaluators = NULL;

  env->destroying = false;

  env->promise_rejections = NULL;

  intrusive_list_init(&env->teardown_queue.tasks);

  env->classes.external = 0;
  env->classes.finalizer = 0;
  env->classes.type_tag = 0;
  env->classes.function = 0;
  env->classes.constructor = 0;
  env->classes.delegate = 0;

  env->callbacks.uncaught_exception = NULL;
  env->callbacks.uncaught_exception_data = NULL;

  env->callbacks.unhandled_rejection = NULL;
  env->callbacks.unhandled_rejection_data = NULL;

  env->callbacks.dynamic_import = NULL;
  env->callbacks.dynamic_import_data = NULL;

  JS_SetRuntimeOpaque(env->runtime, env);
  JS_SetContextOpaque(env->context, env);

  JS_NewClassID(runtime, &env->classes.external);
  err = JS_NewClass(runtime, env->classes.external, &js_external_class);
  assert(err == 0);

  JS_NewClassID(runtime, &env->classes.finalizer);
  err = JS_NewClass(runtime, env->classes.finalizer, &js_finalizer_class);
  assert(err == 0);

  JS_NewClassID(runtime, &env->classes.type_tag);
  err = JS_NewClass(runtime, env->classes.type_tag, &js_type_tag_class);
  assert(err == 0);

  JS_NewClassID(runtime, &env->classes.function);
  err = JS_NewClass(runtime, env->classes.function, &js_function_class);
  assert(err == 0);

  JS_NewClassID(runtime, &env->classes.constructor);
  err = JS_NewClass(runtime, env->classes.constructor, &js_constructor_class);
  assert(err == 0);

  JS_NewClassID(runtime, &env->classes.delegate);
  err = JS_NewClass(runtime, env->classes.delegate, &js_delegate_class);
  assert(err == 0);

  err = uv_prepare_init(loop, &env->prepare);
  assert(err == 0);

  err = uv_prepare_start(&env->prepare, js__on_prepare);
  assert(err == 0);

  env->prepare.data = (void *) env;

  err = uv_check_init(loop, &env->check);
  assert(err == 0);

  err = uv_check_start(&env->check, js__on_check);
  assert(err == 0);

  env->check.data = (void *) env;

  // The check handle should not on its own keep the loop alive; it's simply
  // used for running any outstanding tasks that might cause additional work
  // to be queued.
  uv_unref((uv_handle_t *) &env->check);

  err = uv_async_init(loop, &env->teardown, js__on_teardown);
  assert(err == 0);

  env->teardown.data = (void *) env;

  uv_unref((uv_handle_t *) &env->teardown);

  *result = env;

  return 0;
}

int
js_destroy_env(js_env_t *env) {
  env->destroying = true;

  intrusive_list_for_each(next, &env->teardown_queue.tasks) {
    js_teardown_task_t *task = intrusive_entry(next, js_teardown_task_t, list);

    if (task->type == js_deferred_teardown) {
      task->deferred.cb(&task->deferred.handle, task->data);
    } else {
      task->immediate.cb(task->data);

      intrusive_list_remove(&env->teardown_queue.tasks, &task->list);

      free(task);
    }
  }

  if (env->refs == 0) {
    js__close_env(env);
  } else {
    uv_ref((uv_handle_t *) &env->teardown);
  }

  return 0;
}

int
js_on_uncaught_exception(js_env_t *env, js_uncaught_exception_cb cb, void *data) {
  env->callbacks.uncaught_exception = cb;
  env->callbacks.uncaught_exception_data = data;

  return 0;
}

int
js_on_unhandled_rejection(js_env_t *env, js_unhandled_rejection_cb cb, void *data) {
  env->callbacks.unhandled_rejection = cb;
  env->callbacks.unhandled_rejection_data = data;

  return 0;
}

int
js_on_dynamic_import(js_env_t *env, js_dynamic_import_cb cb, void *data) {
  env->callbacks.dynamic_import = cb;
  env->callbacks.dynamic_import_data = data;

  return 0;
}

int
js_get_env_loop(js_env_t *env, uv_loop_t **result) {
  *result = env->loop;

  return 0;
}

int
js_get_env_platform(js_env_t *env, js_platform_t **result) {
  *result = env->platform;

  return 0;
}

static inline int
js__error(js_env_t *env) {
  return JS_HasException(env->context) ? js_pending_exception : js_uncaught_exception;
}

int
js_open_handle_scope(js_env_t *env, js_handle_scope_t **result) {
  // Allow continuing even with a pending exception

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
js_close_handle_scope(js_env_t *env, js_handle_scope_t *scope) {
  // Allow continuing even with a pending exception

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
js_open_escapable_handle_scope(js_env_t *env, js_escapable_handle_scope_t **result) {
  return js_open_handle_scope(env, (js_handle_scope_t **) result);
}

int
js_close_escapable_handle_scope(js_env_t *env, js_escapable_handle_scope_t *scope) {
  return js_close_handle_scope(env, (js_handle_scope_t *) scope);
}

static inline void
js__attach_to_handle_scope(js_env_t *env, js_handle_scope_t *scope, js_value_t *value) {
  assert(scope);

  if (scope->len >= scope->capacity) {
    if (scope->capacity) scope->capacity *= 2;
    else scope->capacity = 4;

    scope->values = realloc(scope->values, scope->capacity * sizeof(js_value_t *));
  }

  scope->values[scope->len++] = value;
}

int
js_escape_handle(js_env_t *env, js_escapable_handle_scope_t *scope, js_value_t *escapee, js_value_t **result) {
  // Allow continuing even with a pending exception

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = JS_DupValue(env->context, escapee->value);

  *result = wrapper;

  js__attach_to_handle_scope(env, scope->parent, wrapper);

  return 0;
}

int
js_create_context(js_env_t *env, js_context_t **result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_destroy_context(js_env_t *env, js_context_t *context) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_enter_context(js_env_t *env, js_context_t *context) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_exit_context(js_env_t *env, js_context_t *context) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_get_bindings(js_env_t *env, js_value_t **result) {
  if (JS_HasException(env->context)) return js__error(env);

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = JS_DupValue(env->context, env->bindings);

  *result = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_run_script(js_env_t *env, const char *file, size_t len, int offset, js_value_t *source, js_value_t **result) {
  if (JS_HasException(env->context)) return js__error(env);

  size_t str_len;
  const char *str = JS_ToCStringLen(env->context, &str_len, source->value);

  env->depth++;

  if (file == NULL) file = "";

  JSValue value = JS_Eval(
    env->context,
    str,
    str_len,
    file,
    JS_EVAL_TYPE_GLOBAL
  );

  JS_FreeCString(env->context, str);

  if (env->depth == 1) js__on_run_microtasks(env);

  env->depth--;

  if (JS_IsException(value)) {
    if (env->depth == 0) {
      JSValue error = JS_GetException(env->context);

      js__on_uncaught_exception(env->context, error);
    }

    return js__error(env);
  }

  if (result == NULL) JS_FreeValue(env->context, value);
  else {
    js_value_t *wrapper = malloc(sizeof(js_value_t));

    wrapper->value = value;

    *result = wrapper;

    js__attach_to_handle_scope(env, env->scope, wrapper);
  }

  return 0;
}

int
js_create_module(js_env_t *env, const char *name, size_t len, int offset, js_value_t *source, js_module_meta_cb cb, void *data, js_module_t **result) {
  if (JS_HasException(env->context)) return js__error(env);

  js_module_t *module = malloc(sizeof(js_module_t));

  module->context = env->context;
  module->source = JS_DupValue(env->context, source->value);
  module->bytecode = JS_NULL;
  module->definition = NULL;
  module->meta = cb;
  module->meta_data = data;

  if (len == (size_t) -1) {
    module->name = strdup(name);
  } else {
    module->name = malloc(len + 1);
    module->name[len] = '\0';

    memcpy(module->name, name, len);
  }

  *result = module;

  return 0;
}

static int
js__on_evaluate_module(JSContext *context, JSModuleDef *definition) {
  int err;

  js_env_t *env = (js_env_t *) JS_GetContextOpaque(context);

  js_module_evaluator_t *evaluator = env->evaluators;

  while (evaluator && evaluator->module->definition != definition) {
    evaluator = evaluator->next;
  }

  js_handle_scope_t *scope;
  err = js_open_handle_scope(env, &scope);
  assert(err == 0);

  evaluator->cb(env, evaluator->module, evaluator->data);

  err = js_close_handle_scope(env, scope);
  assert(err == 0);

  return 0;
}

int
js_create_synthetic_module(js_env_t *env, const char *name, size_t len, js_value_t *const export_names[], size_t names_len, js_module_evaluate_cb cb, void *data, js_module_t **result) {
  if (JS_HasException(env->context)) return js__error(env);

  js_module_t *module = malloc(sizeof(js_module_t));

  module->context = env->context;
  module->source = JS_NULL;
  module->bytecode = JS_NULL;
  module->definition = JS_NewCModule(env->context, name, js__on_evaluate_module);
  module->meta = NULL;
  module->meta_data = NULL;

  if (len == (size_t) -1) {
    module->name = strdup(name);
  } else {
    module->name = malloc(len + 1);
    module->name[len] = '\0';

    memcpy(module->name, name, len);
  }

  for (size_t i = 0; i < names_len; i++) {
    const char *str = JS_ToCString(env->context, export_names[i]->value);

    JS_AddModuleExport(env->context, module->definition, str);

    JS_FreeCString(env->context, str);
  }

  js_module_evaluator_t *evaluator = malloc(sizeof(js_module_evaluator_t));

  evaluator->module = module;
  evaluator->cb = cb;
  evaluator->data = data;
  evaluator->next = env->evaluators;

  env->evaluators = evaluator;

  *result = module;

  return 0;
}

int
js_delete_module(js_env_t *env, js_module_t *module) {
  // Allow continuing even with a pending exception

  JS_FreeValue(env->context, module->source);
  JS_FreeValue(env->context, module->bytecode);

  free(module->name);
  free(module);

  return 0;
}

int
js_get_module_name(js_env_t *env, js_module_t *module, const char **result) {
  // Allow continuing even with a pending exception

  *result = module->name;

  return 0;
}

int
js_get_module_namespace(js_env_t *env, js_module_t *module, js_value_t **result) {
  // Allow continuing even with a pending exception

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = JS_GetModuleNamespace(env->context, module->definition);

  *result = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_set_module_export(js_env_t *env, js_module_t *module, js_value_t *name, js_value_t *value) {
  if (JS_HasException(env->context)) return js__error(env);

  int err;

  const char *str = JS_ToCString(env->context, name->value);

  int success = JS_SetModuleExport(env->context, module->definition, str, JS_DupValue(env->context, value->value));

  JS_FreeCString(env->context, str);

  if (success < 0) {
    err = js_throw_error(env, NULL, "Could not set module export");
    assert(err == 0);

    return js__error(env);
  }

  return 0;
}

int
js_instantiate_module(js_env_t *env, js_module_t *module, js_module_resolve_cb cb, void *data) {
  if (JS_HasException(env->context)) return js__error(env);

  if (JS_IsNull(module->source)) return 0;

  js_module_resolver_t resolver = {
    .module = module,
    .cb = cb,
    .data = data,
    .next = env->resolvers,
  };

  env->resolvers = &resolver;

  size_t str_len;
  const char *str = JS_ToCStringLen(env->context, &str_len, module->source);

  env->depth++;

  JSValue bytecode = JS_Eval(
    env->context,
    str,
    str_len,
    module->name,
    JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY
  );

  JS_FreeCString(env->context, str);

  if (env->depth == 1) js__on_run_microtasks(env);

  env->depth--;

  if (JS_IsException(bytecode)) {
    if (env->depth == 0) {
      JSValue error = JS_GetException(env->context);

      js__on_uncaught_exception(env->context, error);
    }

    return js__error(env);
  }

  module->bytecode = bytecode;

  module->definition = (JSModuleDef *) JS_VALUE_GET_PTR(bytecode);

  env->resolvers = resolver.next;

  return 0;
}

int
js_run_module(js_env_t *env, js_module_t *module, js_value_t **result) {
  if (JS_HasException(env->context)) return js__error(env);

  int err;

  if (module->meta) {
    JSValue meta = JS_GetImportMeta(env->context, module->definition);

    module->meta(env, module, &(js_value_t) {meta}, module->meta_data);

    JS_FreeValue(env->context, meta);

    if (JS_HasException(env->context)) {
      JSValue error = JS_GetException(env->context);

      js_deferred_t *deferred;
      err = js_create_promise(env, &deferred, result);
      if (err < 0) return err;

      js_reject_deferred(env, deferred, &(js_value_t) {error});

      JS_FreeValue(env->context, error);

      return 0;
    }
  }

  env->depth++;

  JSValue value = JS_EvalFunction(env->context, module->bytecode);

  if (env->depth == 1) js__on_run_microtasks(env);

  env->depth--;

  module->bytecode = JS_NULL;

  if (JS_IsException(value)) {
    JSValue error = JS_GetException(env->context);

    js_deferred_t *deferred;
    err = js_create_promise(env, &deferred, result);
    if (err < 0) return err;

    js_reject_deferred(env, deferred, &(js_value_t) {error});

    JS_FreeValue(env->context, error);

    return 0;
  }

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = value;

  *result = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

static void
js__on_reference_finalize(js_env_t *env, void *data, void *finalize_hint) {
  js_ref_t *reference = (js_ref_t *) data;

  reference->value = JS_NULL;
  reference->finalized = true;
}

static inline void
js__set_weak_reference(js_env_t *env, js_ref_t *reference) {
  if (reference->finalized) return;

  int err;

  js_finalizer_t *finalizer = malloc(sizeof(js_finalizer_t));

  finalizer->data = reference;
  finalizer->finalize_cb = js__on_reference_finalize;
  finalizer->finalize_hint = NULL;

  JSValue external = JS_NewObjectClass(env->context, env->classes.external);

  JS_SetOpaque(external, finalizer);

  JSAtom atom = JS_ValueToAtom(env->context, reference->symbol);

  err = JS_DefinePropertyValue(env->context, reference->value, atom, external, 0);
  assert(err >= 0);

  JS_FreeAtom(env->context, atom);

  JS_FreeValue(env->context, reference->value);
}

static inline void
js__clear_weak_reference(js_env_t *env, js_ref_t *reference) {
  if (reference->finalized) return;

  int err;

  JS_DupValue(env->context, reference->value);

  JSAtom atom = JS_ValueToAtom(env->context, reference->symbol);

  JSValue external = JS_GetProperty(env->context, reference->value, atom);

  js_finalizer_t *finalizer = (js_finalizer_t *) JS_GetOpaque(external, env->classes.external);

  JS_FreeValue(env->context, external);

  finalizer->finalize_cb = NULL;

  err = JS_DeleteProperty(env->context, reference->value, atom, 0);
  assert(err >= 0);

  JS_FreeAtom(env->context, atom);
}

int
js_create_reference(js_env_t *env, js_value_t *value, uint32_t count, js_ref_t **result) {
  // Allow continuing even with a pending exception

  js_ref_t *reference = malloc(sizeof(js_ref_t));

  reference->value = JS_DupValue(env->context, value->value);
  reference->count = count;
  reference->finalized = false;

  if (JS_IsObject(reference->value) || JS_IsFunction(env->context, reference->value)) {
    JSValue global = JS_GetGlobalObject(env->context);
    JSValue constructor = JS_GetPropertyStr(env->context, global, "Symbol");

    JSValue description = JS_NewString(env->context, "__native_reference");

    reference->symbol = JS_Call(env->context, constructor, global, 1, &description);

    JS_FreeValue(env->context, description);
    JS_FreeValue(env->context, constructor);
    JS_FreeValue(env->context, global);

    if (reference->count == 0) js__set_weak_reference(env, reference);
  } else {
    reference->symbol = JS_NULL;
  }

  *result = reference;

  return 0;
}

int
js_delete_reference(js_env_t *env, js_ref_t *reference) {
  // Allow continuing even with a pending exception

  if (reference->count == 0) {
    if (JS_IsObject(reference->value) || JS_IsFunction(env->context, reference->value)) {
      js__clear_weak_reference(env, reference);
    }
  }

  JS_FreeValue(env->context, reference->value);
  JS_FreeValue(env->context, reference->symbol);

  free(reference);

  return 0;
}

int
js_reference_ref(js_env_t *env, js_ref_t *reference, uint32_t *result) {
  // Allow continuing even with a pending exception

  reference->count++;

  if (reference->count == 1) {
    if (JS_IsObject(reference->value) || JS_IsFunction(env->context, reference->value)) {
      js__clear_weak_reference(env, reference);
    }
  }

  if (result) *result = reference->count;

  return 0;
}

int
js_reference_unref(js_env_t *env, js_ref_t *reference, uint32_t *result) {
  // Allow continuing even with a pending exception

  if (reference->count > 0) {
    reference->count--;

    if (reference->count == 0) {
      if (JS_IsObject(reference->value) || JS_IsFunction(env->context, reference->value)) {
        js__set_weak_reference(env, reference);
      }
    }
  }

  if (result) *result = reference->count;

  return 0;
}

int
js_get_reference_value(js_env_t *env, js_ref_t *reference, js_value_t **result) {
  // Allow continuing even with a pending exception

  if (reference->finalized) *result = NULL;
  else {
    js_value_t *wrapper = malloc(sizeof(js_value_t));

    wrapper->value = JS_DupValue(env->context, reference->value);

    *result = wrapper;

    js__attach_to_handle_scope(env, env->scope, wrapper);
  }

  return 0;
}

static void
js__on_constructor_finalize(JSRuntime *runtime, JSValue value) {
  js_env_t *env = (js_env_t *) JS_GetRuntimeOpaque(runtime);

  js_callback_t *callback = (js_callback_t *) JS_GetOpaque(value, env->classes.constructor);

  free(callback);
}

static JSValue
js__on_constructor_call(JSContext *context, JSValueConst new_target, int argc, JSValueConst *argv, int magic, JSValue *data) {
  int err;

  JSValue prototype = JS_GetPropertyStr(context, new_target, "prototype");

  JSValue receiver = JS_NewObjectProto(context, prototype);

  JS_FreeValue(context, prototype);

  js_env_t *env = (js_env_t *) JS_GetContextOpaque(context);

  js_callback_t *callback = (js_callback_t *) JS_GetOpaque(*data, env->classes.constructor);

  js_callback_info_t callback_info = {
    .callback = callback,
    .argc = argc,
    .argv = argv,
    .receiver = receiver,
    .new_target = new_target,
  };

  js_handle_scope_t *scope;
  err = js_open_handle_scope(env, &scope);
  assert(err == 0);

  js_value_t *result = callback->cb(env, &callback_info);

  JSValue value, error;

  if (result == NULL) value = JS_UNDEFINED;
  else value = JS_DupValue(env->context, result->value);

  if (JS_HasException(env->context)) {
    JS_FreeValue(env->context, value);

    value = JS_EXCEPTION;
  }

  err = js_close_handle_scope(env, scope);
  assert(err == 0);

  return receiver;
}

int
js_define_class(js_env_t *env, const char *name, size_t len, js_function_cb constructor, void *data, js_property_descriptor_t const properties[], size_t properties_len, js_value_t **result) {
  if (JS_HasException(env->context)) return js__error(env);

  int err;

  js_callback_t *callback = malloc(sizeof(js_callback_t));

  callback->cb = constructor;
  callback->data = data;

  JSValue external = JS_NewObjectClass(env->context, env->classes.constructor);

  JS_SetOpaque(external, callback);

  JSValue class = JS_NewCFunctionData(env->context, js__on_constructor_call, 0, 0, 1, &external);

  JS_SetConstructorBit(env->context, class, true);

  JSValue prototype = JS_NewObject(env->context);

  JS_SetConstructor(env->context, class, prototype);

  size_t instance_properties_len = 0;
  size_t static_properties_len = 0;

  for (size_t i = 0; i < properties_len; i++) {
    const js_property_descriptor_t *property = &properties[i];

    if ((property->attributes & js_static) == 0) {
      instance_properties_len++;
    } else {
      static_properties_len++;
    }
  }

  if (instance_properties_len) {
    js_property_descriptor_t *instance_properties = malloc(sizeof(js_property_descriptor_t) * instance_properties_len);

    for (size_t i = 0, j = 0; i < properties_len; i++) {
      const js_property_descriptor_t *property = &properties[i];

      if ((property->attributes & js_static) == 0) {
        instance_properties[j++] = *property;
      }
    }

    err = js_define_properties(env, &(js_value_t) {prototype}, instance_properties, instance_properties_len);
    assert(err == 0);

    free(instance_properties);
  }

  if (static_properties_len) {
    js_property_descriptor_t *static_properties = malloc(sizeof(js_property_descriptor_t) * static_properties_len);

    for (size_t i = 0, j = 0; i < properties_len; i++) {
      const js_property_descriptor_t *property = &properties[i];

      if ((property->attributes & js_static) != 0) {
        static_properties[j++] = *property;
      }
    }

    err = js_define_properties(env, &(js_value_t) {class}, static_properties, static_properties_len);
    assert(err == 0);

    free(static_properties);
  }

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = class;

  JS_FreeValue(env->context, external);
  JS_FreeValue(env->context, prototype);

  *result = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_define_properties(js_env_t *env, js_value_t *object, js_property_descriptor_t const properties[], size_t properties_len) {
  if (JS_HasException(env->context)) return js__error(env);

  int err;

  for (size_t i = 0; i < properties_len; i++) {
    const js_property_descriptor_t *property = &properties[i];

    int flags = JS_PROP_HAS_WRITABLE | JS_PROP_HAS_ENUMERABLE | JS_PROP_HAS_CONFIGURABLE;

    if ((property->attributes & js_writable) != 0 || property->getter || property->setter) {
      flags |= JS_PROP_WRITABLE;
    }

    if ((property->attributes & js_enumerable) != 0) {
      flags |= JS_PROP_ENUMERABLE;
    }

    if ((property->attributes & js_configurable) != 0) {
      flags |= JS_PROP_CONFIGURABLE;
    }

    JSValue value = JS_UNDEFINED, getter = JS_UNDEFINED, setter = JS_UNDEFINED;

    if (property->getter || property->setter) {
      if (property->getter) {
        flags |= JS_PROP_HAS_GET;

        js_value_t *fn;
        err = js_create_function(env, "fn", -1, property->getter, property->data, &fn);
        assert(err == 0);

        getter = fn->value;
      }

      if (property->setter) {
        flags |= JS_PROP_HAS_SET;

        js_value_t *fn;
        err = js_create_function(env, "fn", -1, property->setter, property->data, &fn);
        assert(err == 0);

        setter = fn->value;
      }
    } else if (property->method) {
      flags |= JS_PROP_HAS_VALUE;

      js_value_t *fn;
      err = js_create_function(env, "fn", -1, property->method, property->data, &fn);
      assert(err == 0);

      value = fn->value;
    } else {
      flags |= JS_PROP_HAS_VALUE;

      value = property->value->value;
    }

    JSAtom atom = JS_ValueToAtom(env->context, property->name->value);

    err = JS_DefineProperty(env->context, object->value, atom, value, getter, setter, flags);

    JS_FreeAtom(env->context, atom);

    if (err < 0) return js__error(env);
  }

  return 0;
}

int
js_wrap(js_env_t *env, js_value_t *object, void *data, js_finalize_cb finalize_cb, void *finalize_hint, js_ref_t **result) {
  if (JS_HasException(env->context)) return js__error(env);

  int err;

  js_finalizer_t *finalizer = malloc(sizeof(js_finalizer_t));

  finalizer->data = data;
  finalizer->finalize_cb = finalize_cb;
  finalizer->finalize_hint = finalize_hint;

  JSValue external = JS_NewObjectClass(env->context, env->classes.external);

  JS_SetOpaque(external, finalizer);

  JSAtom atom = JS_NewAtom(env->context, "__native_external");

  err = JS_DefinePropertyValue(env->context, object->value, atom, external, 0);

  if (err < 0) {
    JS_FreeValue(env->context, external);

    JS_FreeAtom(env->context, atom);

    return js__error(env);
  }

  JS_FreeAtom(env->context, atom);

  if (result) return js_create_reference(env, object, 0, result);

  return 0;
}

int
js_unwrap(js_env_t *env, js_value_t *object, void **result) {
  if (JS_HasException(env->context)) return js__error(env);

  JSAtom atom = JS_NewAtom(env->context, "__native_external");

  JSValue external = JS_GetProperty(env->context, object->value, atom);

  JS_FreeAtom(env->context, atom);

  js_finalizer_t *finalizer = (js_finalizer_t *) JS_GetOpaque(external, env->classes.external);

  JS_FreeValue(env->context, external);

  *result = finalizer->data;

  return 0;
}

int
js_remove_wrap(js_env_t *env, js_value_t *object, void **result) {
  if (JS_HasException(env->context)) return js__error(env);

  int err;

  JSAtom atom = JS_NewAtom(env->context, "__native_external");

  JSValue external = JS_GetProperty(env->context, object->value, atom);

  js_finalizer_t *finalizer = (js_finalizer_t *) JS_GetOpaque(external, env->classes.external);

  JS_FreeValue(env->context, external);

  finalizer->finalize_cb = NULL;

  if (result) *result = finalizer->data;

  err = JS_DeleteProperty(env->context, object->value, atom, 0);

  if (err < 0) {
    JS_FreeAtom(env->context, atom);

    return js__error(env);
  }

  JS_FreeAtom(env->context, atom);

  return 0;
}

static int
js__on_delegate_get_own_property(JSContext *context, JSPropertyDescriptor *descriptor, JSValueConst object, JSAtom name) {
  js_env_t *env = (js_env_t *) JS_GetContextOpaque(context);

  js_delegate_t *delegate = (js_delegate_t *) JS_GetOpaque(object, env->classes.delegate);

  if (delegate->callbacks.has) {
    JSValue property = JS_AtomToValue(env->context, name);

    bool exists = delegate->callbacks.has(env, &(js_value_t) {property}, delegate->data);

    JS_FreeValue(env->context, property);

    if (JS_HasException(env->context)) return js__error(env);

    if (!exists) return 0;
  }

  if (delegate->callbacks.get) {
    JSValue property = JS_AtomToValue(env->context, name);

    js_value_t *result = delegate->callbacks.get(env, &(js_value_t) {property}, delegate->data);

    JS_FreeValue(env->context, property);

    if (JS_HasException(env->context)) return js__error(env);

    if (result == NULL) return 0;

    if (descriptor) {
      descriptor->flags = JS_PROP_ENUMERABLE;
      descriptor->value = result->value;
      descriptor->getter = JS_UNDEFINED;
      descriptor->setter = JS_UNDEFINED;
    }

    return 1;
  }

  return 0;
}

static int
js__on_delegate_get_own_property_names(JSContext *context, JSPropertyEnum **pproperties, uint32_t *plen, JSValueConst object) {
  int err;

  js_env_t *env = (js_env_t *) JS_GetContextOpaque(context);

  js_delegate_t *delegate = (js_delegate_t *) JS_GetOpaque(object, env->classes.delegate);

  if (delegate->callbacks.own_keys) {
    js_value_t *result = delegate->callbacks.own_keys(env, delegate->data);

    if (JS_HasException(env->context)) return js__error(env);

    uint32_t len;
    err = js_get_array_length(env, result, &len);
    assert(err == 0);

    JSPropertyEnum *properties = js_mallocz(env->context, sizeof(JSPropertyEnum) * len);

    for (uint32_t i = 0; i < len; i++) {
      js_value_t *value;
      err = js_get_element(env, result, i, &value);
      assert(err == 0);

      properties[i].atom = JS_ValueToAtom(env->context, value->value);
    }

    *pproperties = properties;
    *plen = len;

    return 0;
  }

  *pproperties = NULL;
  *plen = 0;

  return 0;
}

static int
js__on_delegate_delete_property(JSContext *context, JSValueConst object, JSAtom name) {
  js_env_t *env = (js_env_t *) JS_GetContextOpaque(context);

  js_delegate_t *delegate = (js_delegate_t *) JS_GetOpaque(object, env->classes.delegate);

  if (delegate->callbacks.delete_property) {
    JSValue property = JS_AtomToValue(env->context, name);

    bool success = delegate->callbacks.delete_property(env, &(js_value_t) {property}, delegate->data);

    JS_FreeValue(env->context, property);

    if (JS_HasException(env->context)) return js__error(env);

    return success;
  }

  return 0;
}

static int
js__on_delegate_set_property(JSContext *context, JSValueConst object, JSAtom name, JSValueConst value, JSValueConst receiver, int flags) {
  js_env_t *env = (js_env_t *) JS_GetContextOpaque(context);

  js_delegate_t *delegate = (js_delegate_t *) JS_GetOpaque(object, env->classes.delegate);

  if (delegate->callbacks.set) {
    JSValue property = JS_AtomToValue(env->context, name);

    bool success = delegate->callbacks.set(env, &(js_value_t) {property}, &(js_value_t) {value}, delegate->data);

    JS_FreeValue(env->context, property);

    if (JS_HasException(env->context)) return js__error(env);

    return success;
  }

  return 0;
}

static void
js__on_delegate_finalize(JSRuntime *runtime, JSValue value) {
  js_env_t *env = (js_env_t *) JS_GetRuntimeOpaque(runtime);

  js_delegate_t *delegate = (js_delegate_t *) JS_GetOpaque(value, env->classes.delegate);

  if (delegate->finalize_cb) {
    delegate->finalize_cb(env, delegate->data, delegate->finalize_hint);
  }

  free(delegate);
}

int
js_create_delegate(js_env_t *env, const js_delegate_callbacks_t *callbacks, void *data, js_finalize_cb finalize_cb, void *finalize_hint, js_value_t **result) {
  // Allow continuing even with a pending exception

  js_delegate_t *delegate = malloc(sizeof(js_delegate_t));

  delegate->data = data;
  delegate->finalize_cb = finalize_cb;
  delegate->finalize_hint = finalize_hint;

  memcpy(&delegate->callbacks, callbacks, sizeof(js_delegate_callbacks_t));

  JSValue object = JS_NewObjectClass(env->context, env->classes.delegate);

  JS_SetOpaque(object, delegate);

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = object;

  *result = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

static void
js__on_finalizer_finalize(JSRuntime *runtime, JSValue value) {
  js_env_t *env = (js_env_t *) JS_GetRuntimeOpaque(runtime);

  js_finalizer_list_t *next = (js_finalizer_list_t *) JS_GetOpaque(value, env->classes.finalizer);
  js_finalizer_list_t *prev;

  while (next) {
    js_finalizer_t *finalizer = &next->finalizer;

    if (finalizer->finalize_cb) {
      finalizer->finalize_cb(env, finalizer->data, finalizer->finalize_hint);
    }

    prev = next;
    next = next->next;

    free(prev);
  }
}

int
js_add_finalizer(js_env_t *env, js_value_t *object, void *data, js_finalize_cb finalize_cb, void *finalize_hint, js_ref_t **result) {
  // Allow continuing even with a pending exception

  int err;

  js_finalizer_list_t *prev = malloc(sizeof(js_finalizer_list_t));

  js_finalizer_t *finalizer = &prev->finalizer;

  finalizer->data = data;
  finalizer->finalize_cb = finalize_cb;
  finalizer->finalize_hint = finalize_hint;

  JSAtom atom = JS_NewAtom(env->context, "__native_finalizer");

  JSValue external;

  if (JS_HasProperty(env->context, object->value, atom) == 1) {
    external = JS_GetProperty(env->context, object->value, atom);
  } else {
    external = JS_NewObjectClass(env->context, env->classes.finalizer);

    JS_SetOpaque(external, NULL);

    err = JS_DefinePropertyValue(env->context, object->value, atom, external, 0);
    assert(err >= 0);

    JS_DupValue(env->context, external);
  }

  JS_FreeAtom(env->context, atom);

  prev->next = (js_finalizer_list_t *) JS_GetOpaque(external, env->classes.finalizer);

  JS_SetOpaque(external, prev);

  JS_FreeValue(env->context, external);

  if (result) return js_create_reference(env, object, 0, result);

  return 0;
}

static void
js__on_type_tag_finalize(JSRuntime *runtime, JSValue value) {
  js_env_t *env = (js_env_t *) JS_GetRuntimeOpaque(runtime);

  js_type_tag_t *tag = (js_type_tag_t *) JS_GetOpaque(value, env->classes.type_tag);

  free(tag);
}

int
js_add_type_tag(js_env_t *env, js_value_t *object, const js_type_tag_t *tag) {
  if (JS_HasException(env->context)) return js__error(env);

  int err;

  JSAtom atom = JS_NewAtom(env->context, "__native_type_tag");

  if (JS_HasProperty(env->context, object->value, atom) == 1) {
    JS_FreeAtom(env->context, atom);

    err = js_throw_errorf(env, NULL, "Object is already type tagged");
    assert(err == 0);

    return js__error(env);
  }

  js_type_tag_t *existing = malloc(sizeof(js_type_tag_t));

  existing->lower = tag->lower;
  existing->upper = tag->upper;

  JSValue external = JS_NewObjectClass(env->context, env->classes.type_tag);

  JS_SetOpaque(external, existing);

  err = JS_DefinePropertyValue(env->context, object->value, atom, external, 0);
  assert(err >= 0);

  JS_FreeAtom(env->context, atom);

  return 0;
}

int
js_check_type_tag(js_env_t *env, js_value_t *object, const js_type_tag_t *tag, bool *result) {
  if (JS_HasException(env->context)) return js__error(env);

  JSAtom atom = JS_NewAtom(env->context, "__native_type_tag");

  *result = false;

  if (JS_HasProperty(env->context, object->value, atom) == 1) {
    JSValue external = JS_GetProperty(env->context, object->value, atom);

    js_type_tag_t *existing = (js_type_tag_t *) JS_GetOpaque(external, env->classes.type_tag);

    JS_FreeValue(env->context, external);

    *result = existing->lower == tag->lower && existing->upper == tag->upper;
  }

  JS_FreeAtom(env->context, atom);

  return 0;
}

int
js_create_int32(js_env_t *env, int32_t value, js_value_t **result) {
  // Allow continuing even with a pending exception

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = JS_NewInt32(env->context, value);

  *result = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_create_uint32(js_env_t *env, uint32_t value, js_value_t **result) {
  // Allow continuing even with a pending exception

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = JS_NewUint32(env->context, value);

  *result = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_create_int64(js_env_t *env, int64_t value, js_value_t **result) {
  // Allow continuing even with a pending exception

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = JS_NewInt64(env->context, value);

  *result = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_create_double(js_env_t *env, double value, js_value_t **result) {
  // Allow continuing even with a pending exception

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = JS_NewFloat64(env->context, value);

  *result = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_create_bigint_int64(js_env_t *env, int64_t value, js_value_t **result) {
  // Allow continuing even with a pending exception

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = JS_NewBigInt64(env->context, value);

  *result = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_create_bigint_uint64(js_env_t *env, uint64_t value, js_value_t **result) {
  // Allow continuing even with a pending exception

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = JS_NewBigUint64(env->context, value);

  *result = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_create_string_utf8(js_env_t *env, const utf8_t *str, size_t len, js_value_t **result) {
  // Allow continuing even with a pending exception

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  if (len == (size_t) -1) {
    wrapper->value = JS_NewString(env->context, (char *) str);
  } else {
    wrapper->value = JS_NewStringLen(env->context, (char *) str, len);
  }

  *result = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_create_string_utf16le(js_env_t *env, const utf16_t *str, size_t len, js_value_t **result) {
  // Allow continuing even with a pending exception

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  if (len == (size_t) -1) len = wcslen((wchar_t *) str);

  size_t utf8_len = utf8_length_from_utf16le(str, len);

  utf8_t *utf8 = malloc(len);

  utf16le_convert_to_utf8(str, len, utf8);

  wrapper->value = JS_NewStringLen(env->context, (char *) utf8, utf8_len);

  free(utf8);

  *result = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_create_string_latin1(js_env_t *env, const latin1_t *str, size_t len, js_value_t **result) {
  // Allow continuing even with a pending exception

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  if (len == (size_t) -1) len = strlen((char *) str);

  size_t utf8_len = utf8_length_from_latin1(str, len);

  utf8_t *utf8 = malloc(len);

  latin1_convert_to_utf8(str, len, utf8);

  wrapper->value = JS_NewStringLen(env->context, (char *) utf8, utf8_len);

  free(utf8);

  *result = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_create_external_string_utf8(js_env_t *env, utf8_t *str, size_t len, js_finalize_cb finalize_cb, void *finalize_hint, js_value_t **result, bool *copied) {
  // Allow continuing even with a pending exception

  int err;
  err = js_create_string_utf8(env, str, len, result);
  assert(err == 0);

  if (copied) *copied = true;

  if (finalize_cb) finalize_cb(env, str, finalize_hint);

  return 0;
}

int
js_create_external_string_utf16le(js_env_t *env, utf16_t *str, size_t len, js_finalize_cb finalize_cb, void *finalize_hint, js_value_t **result, bool *copied) {
  // Allow continuing even with a pending exception

  int err;
  err = js_create_string_utf16le(env, str, len, result);
  assert(err == 0);

  if (copied) *copied = true;

  if (finalize_cb) finalize_cb(env, str, finalize_hint);

  return 0;
}

int
js_create_external_string_latin1(js_env_t *env, latin1_t *str, size_t len, js_finalize_cb finalize_cb, void *finalize_hint, js_value_t **result, bool *copied) {
  // Allow continuing even with a pending exception

  int err;
  err = js_create_string_latin1(env, str, len, result);
  assert(err == 0);

  if (copied) *copied = true;

  if (finalize_cb) finalize_cb(env, str, finalize_hint);

  return 0;
}

int
js_create_property_key_utf8(js_env_t *env, const utf8_t *str, size_t len, js_value_t **result) {
  return js_create_string_utf8(env, str, len, result);
}

int
js_create_property_key_utf16le(js_env_t *env, const utf16_t *str, size_t len, js_value_t **result) {
  return js_create_string_utf16le(env, str, len, result);
}

int
js_create_property_key_latin1(js_env_t *env, const latin1_t *str, size_t len, js_value_t **result) {
  return js_create_string_latin1(env, str, len, result);
}

int
js_create_symbol(js_env_t *env, js_value_t *description, js_value_t **result) {
  // Allow continuing even with a pending exception

  JSValue global = JS_GetGlobalObject(env->context);
  JSValue constructor = JS_GetPropertyStr(env->context, global, "Symbol");

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  JSValue arg = description == NULL ? JS_NULL : description->value;

  wrapper->value = JS_Call(env->context, constructor, global, 1, &arg);

  *result = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  JS_FreeValue(env->context, constructor);
  JS_FreeValue(env->context, global);

  return 0;
}

int
js_create_object(js_env_t *env, js_value_t **result) {
  // Allow continuing even with a pending exception

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = JS_NewObject(env->context);

  *result = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

static void
js__on_function_finalize(JSRuntime *runtime, JSValue value) {
  js_env_t *env = (js_env_t *) JS_GetRuntimeOpaque(runtime);

  js_callback_t *callback = (js_callback_t *) JS_GetOpaque(value, env->classes.function);

  free(callback);
}

static JSValue
js__on_function_call(JSContext *context, JSValueConst receiver, int argc, JSValueConst *argv, int magic, JSValue *data) {
  int err;

  js_env_t *env = (js_env_t *) JS_GetContextOpaque(context);

  js_callback_t *callback = (js_callback_t *) JS_GetOpaque(*data, env->classes.function);

  js_callback_info_t callback_info = {
    .callback = callback,
    .argc = argc,
    .argv = argv,
    .receiver = receiver,
    .new_target = JS_NULL,
  };

  js_handle_scope_t *scope;
  err = js_open_handle_scope(env, &scope);
  assert(err == 0);

  js_value_t *result = callback->cb(env, &callback_info);

  JSValue value;

  if (JS_HasException(env->context)) {
    if (result) JS_FreeValue(env->context, result->value);
    value = JS_EXCEPTION;
  } else {
    if (result) value = JS_DupValue(env->context, result->value);
    else value = JS_UNDEFINED;
  }

  err = js_close_handle_scope(env, scope);
  assert(err == 0);

  return value;
}

int
js_create_function(js_env_t *env, const char *name, size_t len, js_function_cb cb, void *data, js_value_t **result) {
  if (JS_HasException(env->context)) return js__error(env);

  js_callback_t *callback = malloc(sizeof(js_callback_t));

  callback->cb = cb;
  callback->data = data;

  JSValue external = JS_NewObjectClass(env->context, env->classes.function);

  JS_SetOpaque(external, callback);

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = JS_NewCFunctionData(env->context, js__on_function_call, 0, 0, 1, &external);

  JS_FreeValue(env->context, external);

  *result = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_create_function_with_source(js_env_t *env, const char *name, size_t name_len, const char *file, size_t file_len, js_value_t *const args[], size_t args_len, int offset, js_value_t *source, js_value_t **result) {
  if (JS_HasException(env->context)) return js__error(env);

  const char *str;

  size_t buf_len = 0;

  if (name) {
    buf_len += strlen("const ");

    if (name_len == (size_t) -1) buf_len += strlen(name);
    else buf_len += name_len;

    buf_len += strlen(" = ");
  }

  buf_len += strlen("(");

  for (int i = 0; i < args_len; i++) {
    if (i != 0) buf_len += strlen(", ");

    str = JS_ToCString(env->context, args[i]->value);
    buf_len += strlen(str);
    JS_FreeCString(env->context, str);
  }

  buf_len += strlen(") => {\n");

  str = JS_ToCString(env->context, source->value);
  buf_len += strlen(str);
  JS_FreeCString(env->context, str);

  buf_len += strlen("}\n");

  if (name) {
    if (name_len == (size_t) -1) buf_len += strlen(name);
    else buf_len += name_len;

    buf_len += strlen("\n");
  }

  char *buf = malloc(buf_len + 1 /* NULL */);

  buf[0] = '\0';

  if (name) {
    strcat(buf, "const ");

    if (name_len == (size_t) -1) strcat(buf, name);
    else strncat(buf, name, name_len);

    strcat(buf, " = ");
  }

  strcat(buf, "(");

  for (int i = 0; i < args_len; i++) {
    if (i != 0) strcat(buf, ", ");

    str = JS_ToCString(env->context, args[i]->value);
    strcat(buf, str);
    JS_FreeCString(env->context, str);
  }

  strcat(buf, ") => {\n");

  str = JS_ToCString(env->context, source->value);
  strcat(buf, str);
  JS_FreeCString(env->context, str);

  strcat(buf, "}\n");

  if (name) {
    if (name_len == (size_t) -1) strcat(buf, name);
    else strncat(buf, name, name_len);

    strcat(buf, "\n");
  }

  if (file == NULL) file = "";

  JSValue function = JS_Eval(
    env->context,
    buf,
    buf_len,
    file,
    JS_EVAL_TYPE_GLOBAL
  );

  free(buf);

  if (JS_IsException(function)) {
    if (env->depth == 0) {
      JSValue error = JS_GetException(env->context);

      js__on_uncaught_exception(env->context, error);
    }

    return js__error(env);
  }

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = function;

  *result = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_create_typed_function(js_env_t *env, const char *name, size_t len, js_function_cb cb, const js_callback_signature_t *signature, const void *address, void *data, js_value_t **result) {
  return js_create_function(env, name, len, cb, data, result);
}

int
js_create_array(js_env_t *env, js_value_t **result) {
  // Allow continuing even with a pending exception

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = JS_NewArray(env->context);

  *result = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_create_array_with_length(js_env_t *env, size_t len, js_value_t **result) {
  // Allow continuing even with a pending exception

  JSValue global = JS_GetGlobalObject(env->context);
  JSValue constructor = JS_GetPropertyStr(env->context, global, "Array");

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  JSValue arg = JS_NewUint32(env->context, len);

  wrapper->value = JS_CallConstructor(env->context, constructor, 1, &arg);

  *result = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  JS_FreeValue(env->context, arg);
  JS_FreeValue(env->context, constructor);
  JS_FreeValue(env->context, global);

  return 0;
}

static void
js__on_external_finalize(JSRuntime *runtime, JSValue value) {
  js_env_t *env = (js_env_t *) JS_GetRuntimeOpaque(runtime);

  js_finalizer_t *finalizer = (js_finalizer_t *) JS_GetOpaque(value, env->classes.external);

  if (finalizer->finalize_cb) {
    finalizer->finalize_cb(env, finalizer->data, finalizer->finalize_hint);
  }

  free(finalizer);
}

int
js_create_external(js_env_t *env, void *data, js_finalize_cb finalize_cb, void *finalize_hint, js_value_t **result) {
  // Allow continuing even with a pending exception

  js_finalizer_t *finalizer = malloc(sizeof(js_finalizer_t));

  finalizer->data = data;
  finalizer->finalize_cb = finalize_cb;
  finalizer->finalize_hint = finalize_hint;

  JSValue external = JS_NewObjectClass(env->context, env->classes.external);

  JS_SetOpaque(external, finalizer);

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = external;

  *result = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_create_date(js_env_t *env, double time, js_value_t **result) {
  // Allow continuing even with a pending exception

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = JS_NewDate(env->context, time);

  *result = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_create_error(js_env_t *env, js_value_t *code, js_value_t *message, js_value_t **result) {
  // Allow continuing even with a pending exception

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

  js__attach_to_handle_scope(env, env->scope, wrapper);

  JS_FreeValue(env->context, constructor);
  JS_FreeValue(env->context, global);

  return 0;
}

int
js_create_type_error(js_env_t *env, js_value_t *code, js_value_t *message, js_value_t **result) {
  // Allow continuing even with a pending exception

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

  js__attach_to_handle_scope(env, env->scope, wrapper);

  JS_FreeValue(env->context, constructor);
  JS_FreeValue(env->context, global);

  return 0;
}

int
js_create_range_error(js_env_t *env, js_value_t *code, js_value_t *message, js_value_t **result) {
  // Allow continuing even with a pending exception

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

  js__attach_to_handle_scope(env, env->scope, wrapper);

  JS_FreeValue(env->context, constructor);
  JS_FreeValue(env->context, global);

  return 0;
}

int
js_create_syntax_error(js_env_t *env, js_value_t *code, js_value_t *message, js_value_t **result) {
  // Allow continuing even with a pending exception

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

  js__attach_to_handle_scope(env, env->scope, wrapper);

  JS_FreeValue(env->context, constructor);
  JS_FreeValue(env->context, global);

  return 0;
}

int
js_create_promise(js_env_t *env, js_deferred_t **deferred, js_value_t **promise) {
  // Allow continuing even with a pending exception

  *deferred = malloc(sizeof(js_deferred_t));

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  JSValue functions[2];

  wrapper->value = JS_NewPromiseCapability(env->context, functions);

  (*deferred)->resolve = functions[0];
  (*deferred)->reject = functions[1];

  *promise = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

static inline int
js__conclude_deferred(js_env_t *env, js_deferred_t *deferred, js_value_t *resolution, bool resolved) {
  // Allow continuing even with a pending exception

  JSValue global = JS_GetGlobalObject(env->context);

  JSValue result = JS_Call(env->context, resolved ? deferred->resolve : deferred->reject, global, 1, &resolution->value);

  if (env->depth == 0) js__on_run_microtasks(env);

  JS_FreeValue(env->context, global);
  JS_FreeValue(env->context, result);
  JS_FreeValue(env->context, deferred->resolve);
  JS_FreeValue(env->context, deferred->reject);

  free(deferred);

  return 0;
}

int
js_resolve_deferred(js_env_t *env, js_deferred_t *deferred, js_value_t *resolution) {
  return js__conclude_deferred(env, deferred, resolution, true);
}

int
js_reject_deferred(js_env_t *env, js_deferred_t *deferred, js_value_t *resolution) {
  return js__conclude_deferred(env, deferred, resolution, false);
}

int
js_get_promise_state(js_env_t *env, js_value_t *promise, js_promise_state_t *result) {
  // Allow continuing even with a pending exception

  switch (JS_PromiseState(env->context, promise->value)) {
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
js_get_promise_result(js_env_t *env, js_value_t *promise, js_value_t **result) {
  // Allow continuing even with a pending exception

  assert(JS_PromiseState(env->context, promise->value) != JS_PROMISE_PENDING);

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = JS_PromiseResult(env->context, promise->value);

  *result = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

static void
js__on_arraybuffer_finalize(JSRuntime *runtime, void *opaque, void *ptr) {
  free(ptr);
}

int
js_create_arraybuffer(js_env_t *env, size_t len, void **data, js_value_t **result) {
  if (JS_HasException(env->context)) return js__error(env);

  int err;

  if (len > UINT32_MAX) goto err;

  uint8_t *bytes = malloc(len);

  if (bytes == NULL) goto err;

  memset(bytes, 0, len);

  if (data) {
    *data = bytes;
  }

  JSValue arraybuffer = JS_NewArrayBuffer(env->context, bytes, len, js__on_arraybuffer_finalize, NULL, false);

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = arraybuffer;

  *result = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  return 0;

err:
  err = js_throw_range_error(env, NULL, "Array buffer allocation failed");
  assert(err == 0);

  return js__error(env);
}

static void
js__on_backed_arraybuffer_finalize(JSRuntime *runtime, void *opaque, void *ptr) {
  js_arraybuffer_backing_store_t *backing_store = (js_arraybuffer_backing_store_t *) opaque;

  if (--backing_store->references == 0) {
    JS_FreeValueRT(runtime, backing_store->owner);

    free(backing_store);
  }
}

int
js_create_arraybuffer_with_backing_store(js_env_t *env, js_arraybuffer_backing_store_t *backing_store, void **data, size_t *len, js_value_t **result) {
  if (JS_HasException(env->context)) return js__error(env);

  backing_store->references++;

  if (data) {
    *data = backing_store->data;
  }

  if (len) {
    *len = backing_store->len;
  }

  JSValue arraybuffer = JS_NewArrayBuffer(env->context, backing_store->data, backing_store->len, js__on_backed_arraybuffer_finalize, backing_store, false);

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = arraybuffer;

  *result = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

static void
js__on_unsafe_arraybuffer_finalize(JSRuntime *runtime, void *opaque, void *ptr) {
  free(ptr);
}

int
js_create_unsafe_arraybuffer(js_env_t *env, size_t len, void **data, js_value_t **result) {
  if (JS_HasException(env->context)) return js__error(env);

  int err;

  if (len > UINT32_MAX) goto err;

  uint8_t *bytes = malloc(len);

  if (bytes == NULL) goto err;

  if (data) {
    *data = bytes;
  }

  JSValue arraybuffer = JS_NewArrayBuffer(env->context, bytes, len, js__on_unsafe_arraybuffer_finalize, NULL, false);

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = arraybuffer;

  *result = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  return 0;

err:
  err = js_throw_range_error(env, NULL, "Array buffer allocation failed");
  assert(err == 0);

  return js__error(env);
}

static void
js__on_external_arraybuffer_finalize(JSRuntime *runtime, void *opaque, void *ptr) {
  if (ptr == NULL) return;

  js_env_t *env = (js_env_t *) JS_GetRuntimeOpaque(runtime);

  js_finalizer_t *finalizer = (js_finalizer_t *) opaque;

  if (finalizer->finalize_cb) {
    finalizer->finalize_cb(env, finalizer->data, finalizer->finalize_hint);
  }

  free(finalizer);
}

int
js_create_external_arraybuffer(js_env_t *env, void *data, size_t len, js_finalize_cb finalize_cb, void *finalize_hint, js_value_t **result) {
  if (JS_HasException(env->context)) return js__error(env);

  js_finalizer_t *finalizer = malloc(sizeof(js_finalizer_t));

  finalizer->data = data;
  finalizer->finalize_cb = finalize_cb;
  finalizer->finalize_hint = finalize_hint;

  JSValue arraybuffer = JS_NewArrayBuffer(env->context, (uint8_t *) data, len, js__on_external_arraybuffer_finalize, (void *) finalizer, false);

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = arraybuffer;

  *result = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_detach_arraybuffer(js_env_t *env, js_value_t *arraybuffer) {
  // Allow continuing even with a pending exception

  JS_DetachArrayBuffer(env->context, arraybuffer->value);

  return 0;
}

int
js_get_arraybuffer_backing_store(js_env_t *env, js_value_t *arraybuffer, js_arraybuffer_backing_store_t **result) {
  // Allow continuing even with a pending exception

  js_arraybuffer_backing_store_t *backing_store = malloc(sizeof(js_arraybuffer_backing_store_t));

  backing_store->references = 1;

  backing_store->data = JS_GetArrayBuffer(env->context, &backing_store->len, arraybuffer->value);

  backing_store->owner = JS_DupValue(env->context, arraybuffer->value);

  *result = backing_store;

  return 0;
}

int
js_create_sharedarraybuffer(js_env_t *env, size_t len, void **data, js_value_t **result) {
  if (JS_HasException(env->context)) return js__error(env);

  js_arraybuffer_header_t *header = malloc(sizeof(js_arraybuffer_header_t) + len);

  memset(header->data, 0, len);

  header->references = 0;
  header->len = len;

  if (data) {
    *data = header->data;
  }

  JSValue sharedarraybuffer = JS_NewArrayBuffer(env->context, header->data, header->len, NULL, NULL, true);

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = sharedarraybuffer;

  *result = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_create_sharedarraybuffer_with_backing_store(js_env_t *env, js_arraybuffer_backing_store_t *backing_store, void **data, size_t *len, js_value_t **result) {
  if (JS_HasException(env->context)) return js__error(env);

  if (data) {
    *data = backing_store->data;
  }

  if (len) {
    *len = backing_store->len;
  }

  JSValue sharedarraybuffer = JS_NewArrayBuffer(env->context, backing_store->data, backing_store->len, NULL, NULL, true);

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = sharedarraybuffer;

  *result = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_create_unsafe_sharedarraybuffer(js_env_t *env, size_t len, void **data, js_value_t **result) {
  if (JS_HasException(env->context)) return js__error(env);

  js_arraybuffer_header_t *header = malloc(sizeof(js_arraybuffer_header_t) + len);

  header->references = 0;
  header->len = len;

  if (data) {
    *data = header->data;
  }

  JSValue sharedarraybuffer = JS_NewArrayBuffer(env->context, header->data, header->len, NULL, NULL, true);

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = sharedarraybuffer;

  *result = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_get_sharedarraybuffer_backing_store(js_env_t *env, js_value_t *sharedarraybuffer, js_arraybuffer_backing_store_t **result) {
  // Allow continuing even with a pending exception

  js_arraybuffer_backing_store_t *backing_store = malloc(sizeof(js_arraybuffer_backing_store_t));

  backing_store->references = 1;

  backing_store->data = JS_GetArrayBuffer(env->context, &backing_store->len, sharedarraybuffer->value);

  backing_store->owner = JS_NULL;

  *result = backing_store;

  return 0;
}

int
js_release_arraybuffer_backing_store(js_env_t *env, js_arraybuffer_backing_store_t *backing_store) {
  // Allow continuing even with a pending exception

  if (--backing_store->references == 0) {
    JS_FreeValue(env->context, backing_store->owner);

    free(backing_store);
  }

  return 0;
}

int
js_create_typedarray(js_env_t *env, js_typedarray_type_t type, size_t len, js_value_t *arraybuffer, size_t offset, js_value_t **result) {
  if (JS_HasException(env->context)) return js__error(env);

  JSValue global = JS_GetGlobalObject(env->context);
  JSValue constructor;

  switch (type) {
#define V(class, type) \
  case type: \
    constructor = JS_GetPropertyStr(env->context, global, class); \
    break;

    V("Int8Array", js_int8array);
    V("Uint8Array", js_uint8array);
    V("Uint8ClampedArray", js_uint8clampedarray);
    V("Int16Array", js_int16array);
    V("Uint16Array", js_uint16array);
    V("Int32Array", js_int32array);
    V("Uint32Array", js_uint32array);
    V("Float16Array", js_float16array);
    V("Float32Array", js_float32array);
    V("Float64Array", js_float64array);
    V("BigInt64Array", js_bigint64array);
    V("BigUint64Array", js_biguint64array);
#undef V
  }

  JSValue argv[3] = {arraybuffer->value, JS_NewInt64(env->context, offset), JS_NewInt64(env->context, len)};

  JSValue typedarray = JS_CallConstructor(env->context, constructor, 3, argv);

  JS_FreeValue(env->context, constructor);
  JS_FreeValue(env->context, global);

  if (JS_IsException(typedarray)) return js__error(env);

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = typedarray;

  *result = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_create_dataview(js_env_t *env, size_t len, js_value_t *arraybuffer, size_t offset, js_value_t **result) {
  if (JS_HasException(env->context)) return js__error(env);

  JSValue global = JS_GetGlobalObject(env->context);
  JSValue constructor = JS_GetPropertyStr(env->context, global, "DataView");

  JSValue argv[3] = {arraybuffer->value, JS_NewInt64(env->context, offset), JS_NewInt64(env->context, len)};

  JSValue typedarray = JS_CallConstructor(env->context, constructor, 3, argv);

  JS_FreeValue(env->context, constructor);
  JS_FreeValue(env->context, global);

  if (JS_IsException(typedarray)) return js__error(env);

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = typedarray;

  *result = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_coerce_to_boolean(js_env_t *env, js_value_t *value, js_value_t **result) {
  // Allow continuing even with a pending exception

  JSValue boolean = JS_ToBoolean(env->context, value->value);

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = boolean;

  *result = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_coerce_to_number(js_env_t *env, js_value_t *value, js_value_t **result) {
  if (JS_HasException(env->context)) return js__error(env);

  JSValue number = JS_ToNumber(env->context, value->value);

  if (JS_IsException(number)) return js__error(env);

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = number;

  *result = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_coerce_to_string(js_env_t *env, js_value_t *value, js_value_t **result) {
  if (JS_HasException(env->context)) return js__error(env);

  JSValue string = JS_ToString(env->context, value->value);

  if (JS_IsException(string)) return js__error(env);

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = string;

  *result = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_coerce_to_object(js_env_t *env, js_value_t *value, js_value_t **result) {
  if (JS_HasException(env->context)) return js__error(env);

  JSValue object = JS_ToObject(env->context, value->value);

  if (JS_IsException(object)) return js__error(env);

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = object;

  *result = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_typeof(js_env_t *env, js_value_t *value, js_value_type_t *result) {
  // Allow continuing even with a pending exception

  if (JS_IsNumber(value->value)) {
    *result = js_number;
  } else if (JS_IsBigInt(env->context, value->value)) {
    *result = js_bigint;
  } else if (JS_IsString(value->value)) {
    *result = js_string;
  } else if (JS_IsFunction(env->context, value->value)) {
    *result = js_function;
  } else if (JS_IsObject(value->value)) {
    *result = JS_GetOpaque(value->value, env->classes.external) != NULL
                ? js_external
                : js_object;
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
js_instanceof(js_env_t *env, js_value_t *object, js_value_t *constructor, bool *result) {
  if (JS_HasException(env->context)) return js__error(env);

  int success = JS_IsInstanceOf(env->context, object->value, constructor->value);

  if (success < 0) return js__error(env);

  *result = success;

  return 0;
}

int
js_is_undefined(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = JS_IsUndefined(value->value);

  return 0;
}

int
js_is_null(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = JS_IsNull(value->value);

  return 0;
}

int
js_is_boolean(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = JS_IsBool(value->value);

  return 0;
}

int
js_is_number(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = JS_IsNumber(value->value);

  return 0;
}

int
js_is_int32(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  if (JS_IsNumber(value->value)) {
    double number;

    JS_ToFloat64(env->context, &number, value->value);

    double integral;

    *result = modf(number, &integral) == 0.0 && integral >= INT32_MIN && integral <= INT32_MAX;
  } else {
    *result = false;
  }

  return 0;
}

int
js_is_uint32(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  if (JS_IsNumber(value->value)) {
    double number;

    JS_ToFloat64(env->context, &number, value->value);

    double integral;

    *result = modf(number, &integral) == 0.0 && integral >= 0.0 && integral <= UINT32_MAX;
  } else {
    *result = false;
  }

  return 0;
}

int
js_is_string(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = JS_IsString(value->value);

  return 0;
}

int
js_is_symbol(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = JS_IsSymbol(value->value);

  return 0;
}

int
js_is_object(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = JS_IsObject(value->value);

  return 0;
}

int
js_is_function(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = JS_IsFunction(env->context, value->value);

  return 0;
}

int
js_is_async_function(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = false;

  return 0;
}

int
js_is_generator_function(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = false;

  return 0;
}

int
js_is_generator(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = false;

  return 0;
}

int
js_is_arguments(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = false;

  return 0;
}

int
js_is_array(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = JS_IsArray(value->value);

  return 0;
}

int
js_is_external(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = JS_IsObject(value->value) && JS_GetOpaque(value->value, env->classes.external) != NULL;

  return 0;
}

int
js_is_wrapped(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  JSAtom atom = JS_NewAtom(env->context, "__native_external");

  *result = JS_IsObject(value->value) && JS_HasProperty(env->context, value->value, atom) == 1;

  JS_FreeAtom(env->context, atom);

  return 0;
}

int
js_is_delegate(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = JS_IsObject(value->value) && JS_GetOpaque(value->value, env->classes.delegate) != NULL;

  return 0;
}

int
js_is_bigint(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = JS_IsBigInt(env->context, value->value);

  return 0;
}

int
js_is_date(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  JSValue global = JS_GetGlobalObject(env->context);
  JSValue constructor = JS_GetPropertyStr(env->context, global, "Date");

  *result = JS_IsInstanceOf(env->context, value->value, constructor);

  JS_FreeValue(env->context, constructor);
  JS_FreeValue(env->context, global);

  return 0;
}

int
js_is_regexp(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  JSValue global = JS_GetGlobalObject(env->context);
  JSValue constructor = JS_GetPropertyStr(env->context, global, "RegExp");

  *result = JS_IsInstanceOf(env->context, value->value, constructor);

  JS_FreeValue(env->context, constructor);
  JS_FreeValue(env->context, global);

  return 0;
}

int
js_is_error(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = JS_IsError(env->context, value->value);

  return 0;
}

int
js_is_promise(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  JSValue global = JS_GetGlobalObject(env->context);
  JSValue constructor = JS_GetPropertyStr(env->context, global, "Promise");

  *result = JS_IsInstanceOf(env->context, value->value, constructor);

  JS_FreeValue(env->context, constructor);
  JS_FreeValue(env->context, global);

  return 0;
}

int
js_is_proxy(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  JSValue global = JS_GetGlobalObject(env->context);
  JSValue constructor = JS_GetPropertyStr(env->context, global, "Proxy");

  *result = JS_IsInstanceOf(env->context, value->value, constructor);

  JS_FreeValue(env->context, constructor);
  JS_FreeValue(env->context, global);

  return 0;
}

int
js_is_map(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  JSValue global = JS_GetGlobalObject(env->context);
  JSValue constructor = JS_GetPropertyStr(env->context, global, "Map");

  *result = JS_IsInstanceOf(env->context, value->value, constructor);

  JS_FreeValue(env->context, constructor);
  JS_FreeValue(env->context, global);

  return 0;
}

int
js_is_map_iterator(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = false;

  return 0;
}

int
js_is_set(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  JSValue global = JS_GetGlobalObject(env->context);
  JSValue constructor = JS_GetPropertyStr(env->context, global, "Set");

  *result = JS_IsInstanceOf(env->context, value->value, constructor);

  JS_FreeValue(env->context, constructor);
  JS_FreeValue(env->context, global);

  return 0;
}

int
js_is_set_iterator(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = false;

  return 0;
}

int
js_is_weak_map(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  JSValue global = JS_GetGlobalObject(env->context);
  JSValue constructor = JS_GetPropertyStr(env->context, global, "WeakMap");

  *result = JS_IsInstanceOf(env->context, value->value, constructor);

  JS_FreeValue(env->context, constructor);
  JS_FreeValue(env->context, global);

  return 0;
}

int
js_is_weak_set(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  JSValue global = JS_GetGlobalObject(env->context);
  JSValue constructor = JS_GetPropertyStr(env->context, global, "WeakSet");

  *result = JS_IsInstanceOf(env->context, value->value, constructor);

  JS_FreeValue(env->context, constructor);
  JS_FreeValue(env->context, global);

  return 0;
}

int
js_is_weak_ref(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  JSValue global = JS_GetGlobalObject(env->context);
  JSValue constructor = JS_GetPropertyStr(env->context, global, "WeakRef");

  *result = JS_IsInstanceOf(env->context, value->value, constructor);

  JS_FreeValue(env->context, constructor);
  JS_FreeValue(env->context, global);

  return 0;
}

int
js_is_arraybuffer(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  JSValue global = JS_GetGlobalObject(env->context);
  JSValue constructor = JS_GetPropertyStr(env->context, global, "ArrayBuffer");

  *result = JS_IsInstanceOf(env->context, value->value, constructor);

  JS_FreeValue(env->context, constructor);
  JS_FreeValue(env->context, global);

  return 0;
}

int
js_is_detached_arraybuffer(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  size_t len;

  *result = JS_GetArrayBuffer(env->context, &len, value->value) == NULL;

  return 0;
}

int
js_is_sharedarraybuffer(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  JSValue global = JS_GetGlobalObject(env->context);
  JSValue constructor = JS_GetPropertyStr(env->context, global, "SharedArrayBuffer");

  *result = JS_IsInstanceOf(env->context, value->value, constructor);

  JS_FreeValue(env->context, constructor);
  JS_FreeValue(env->context, global);

  return 0;
}

int
js_is_typedarray(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  JSValue global = JS_GetGlobalObject(env->context);

#define V(class) \
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

  V("Int8Array");
  V("Uint8Array");
  V("Uint8ClampedArray");
  V("Int16Array");
  V("Uint16Array");
  V("Int32Array");
  V("Uint32Array");
  V("Float32Array");
  V("Float64Array");
  V("BigInt64Array");
  V("BigUint64Array");

#undef V

  *result = false;

done:
  JS_FreeValue(env->context, global);

  return 0;
}

int
js_is_int8array(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  JSValue global = JS_GetGlobalObject(env->context);
  JSValue constructor = JS_GetPropertyStr(env->context, global, "Int8Array");

  *result = JS_IsInstanceOf(env->context, value->value, constructor);

  JS_FreeValue(env->context, constructor);
  JS_FreeValue(env->context, global);

  return 0;
}

int
js_is_uint8array(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  JSValue global = JS_GetGlobalObject(env->context);
  JSValue constructor = JS_GetPropertyStr(env->context, global, "Uint8Array");

  *result = JS_IsInstanceOf(env->context, value->value, constructor);

  JS_FreeValue(env->context, constructor);
  JS_FreeValue(env->context, global);

  return 0;
}

int
js_is_uint8clampedarray(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  JSValue global = JS_GetGlobalObject(env->context);
  JSValue constructor = JS_GetPropertyStr(env->context, global, "Uint8ClampedArray");

  *result = JS_IsInstanceOf(env->context, value->value, constructor);

  JS_FreeValue(env->context, constructor);
  JS_FreeValue(env->context, global);

  return 0;
}

int
js_is_int16array(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  JSValue global = JS_GetGlobalObject(env->context);
  JSValue constructor = JS_GetPropertyStr(env->context, global, "Int16Array");

  *result = JS_IsInstanceOf(env->context, value->value, constructor);

  JS_FreeValue(env->context, constructor);
  JS_FreeValue(env->context, global);

  return 0;
}

int
js_is_uint16array(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  JSValue global = JS_GetGlobalObject(env->context);
  JSValue constructor = JS_GetPropertyStr(env->context, global, "Uint16Array");

  *result = JS_IsInstanceOf(env->context, value->value, constructor);

  JS_FreeValue(env->context, constructor);
  JS_FreeValue(env->context, global);

  return 0;
}

int
js_is_int32array(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  JSValue global = JS_GetGlobalObject(env->context);
  JSValue constructor = JS_GetPropertyStr(env->context, global, "Int32Array");

  *result = JS_IsInstanceOf(env->context, value->value, constructor);

  JS_FreeValue(env->context, constructor);
  JS_FreeValue(env->context, global);

  return 0;
}

int
js_is_uint32array(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  JSValue global = JS_GetGlobalObject(env->context);
  JSValue constructor = JS_GetPropertyStr(env->context, global, "Uint32Array");

  *result = JS_IsInstanceOf(env->context, value->value, constructor);

  JS_FreeValue(env->context, constructor);
  JS_FreeValue(env->context, global);

  return 0;
}

int
js_is_float16array(js_env_t *env, js_value_t *value, bool *result) {
  *result = false;

  return 0;
}

int
js_is_float32array(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  JSValue global = JS_GetGlobalObject(env->context);
  JSValue constructor = JS_GetPropertyStr(env->context, global, "Float32Array");

  *result = JS_IsInstanceOf(env->context, value->value, constructor);

  JS_FreeValue(env->context, constructor);
  JS_FreeValue(env->context, global);

  return 0;
}

int
js_is_float64array(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  JSValue global = JS_GetGlobalObject(env->context);
  JSValue constructor = JS_GetPropertyStr(env->context, global, "Float64Array");

  *result = JS_IsInstanceOf(env->context, value->value, constructor);

  JS_FreeValue(env->context, constructor);
  JS_FreeValue(env->context, global);

  return 0;
}

int
js_is_bigint64array(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  JSValue global = JS_GetGlobalObject(env->context);
  JSValue constructor = JS_GetPropertyStr(env->context, global, "BigInt64Array");

  *result = JS_IsInstanceOf(env->context, value->value, constructor);

  JS_FreeValue(env->context, constructor);
  JS_FreeValue(env->context, global);

  return 0;
}

int
js_is_biguint64array(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  JSValue global = JS_GetGlobalObject(env->context);
  JSValue constructor = JS_GetPropertyStr(env->context, global, "BigUint64Array");

  *result = JS_IsInstanceOf(env->context, value->value, constructor);

  JS_FreeValue(env->context, constructor);
  JS_FreeValue(env->context, global);

  return 0;
}

int
js_is_dataview(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  JSValue global = JS_GetGlobalObject(env->context);
  JSValue constructor = JS_GetPropertyStr(env->context, global, "DataView");

  *result = JS_IsInstanceOf(env->context, value->value, constructor);

  JS_FreeValue(env->context, constructor);
  JS_FreeValue(env->context, global);

  return 0;
}

int
js_is_module_namespace(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = false;

  return 0;
}

int
js_strict_equals(js_env_t *env, js_value_t *a, js_value_t *b, bool *result) {
  // Allow continuing even with a pending exception

  *result = JS_IsStrictEqual(env->context, a->value, b->value);

  return 0;
}

int
js_get_global(js_env_t *env, js_value_t **result) {
  // Allow continuing even with a pending exception

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = JS_GetGlobalObject(env->context);

  *result = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_get_undefined(js_env_t *env, js_value_t **result) {
  // Allow continuing even with a pending exception

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = JS_UNDEFINED;

  *result = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_get_null(js_env_t *env, js_value_t **result) {
  // Allow continuing even with a pending exception

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = JS_NULL;

  *result = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_get_boolean(js_env_t *env, bool value, js_value_t **result) {
  // Allow continuing even with a pending exception

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = value ? JS_TRUE : JS_FALSE;

  *result = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_get_value_bool(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = JS_ToBool(env->context, value->value);

  return 0;
}

int
js_get_value_int32(js_env_t *env, js_value_t *value, int32_t *result) {
  // Allow continuing even with a pending exception

  JS_ToInt32(env->context, result, value->value);

  return 0;
}

int
js_get_value_uint32(js_env_t *env, js_value_t *value, uint32_t *result) {
  // Allow continuing even with a pending exception

  JS_ToUint32(env->context, result, value->value);

  return 0;
}

int
js_get_value_int64(js_env_t *env, js_value_t *value, int64_t *result) {
  // Allow continuing even with a pending exception

  JS_ToInt64(env->context, result, value->value);

  return 0;
}

int
js_get_value_double(js_env_t *env, js_value_t *value, double *result) {
  // Allow continuing even with a pending exception

  JS_ToFloat64(env->context, result, value->value);

  return 0;
}

int
js_get_value_bigint_int64(js_env_t *env, js_value_t *value, int64_t *result, bool *lossless) {
  // Allow continuing even with a pending exception

  JS_ToBigInt64(env->context, result, value->value);

  if (lossless) *lossless = true;

  return 0;
}

int
js_get_value_bigint_uint64(js_env_t *env, js_value_t *value, uint64_t *result, bool *lossless) {
  // Allow continuing even with a pending exception

  JS_ToBigInt64(env->context, (int64_t *) result, value->value);

  if (lossless) *lossless = true;

  return 0;
}

int
js_get_value_string_utf8(js_env_t *env, js_value_t *value, utf8_t *str, size_t len, size_t *result) {
  // Allow continuing even with a pending exception

  size_t cstr_len;
  const char *cstr = JS_ToCStringLen(env->context, &cstr_len, value->value);

  if (str == NULL) {
    *result = cstr_len;
  } else if (len != 0) {
    size_t written = cstr_len < len ? cstr_len : len;

    memcpy(str, cstr, written);

    if (written < len) str[written] = '\0';

    if (result) *result = written;
  } else if (result) *result = 0;

  JS_FreeCString(env->context, cstr);

  return 0;
}

int
js_get_value_string_utf16le(js_env_t *env, js_value_t *value, utf16_t *str, size_t len, size_t *result) {
  // Allow continuing even with a pending exception

  size_t cstr_len;
  const char *cstr = JS_ToCStringLen(env->context, &cstr_len, value->value);

  size_t utf16_len = utf16_length_from_utf8((utf8_t *) cstr, cstr_len);

  if (str == NULL) {
    *result = utf16_len;
  } else if (len != 0) {
    size_t written = utf16_len < len ? utf16_len : len;

    utf8_convert_to_utf16le((utf8_t *) cstr, cstr_len, str);

    if (written < len) str[written] = L'\0';

    if (result) *result = written;
  } else if (result) *result = 0;

  JS_FreeCString(env->context, cstr);

  return 0;
}

int
js_get_value_string_latin1(js_env_t *env, js_value_t *value, latin1_t *str, size_t len, size_t *result) {
  // Allow continuing even with a pending exception

  size_t cstr_len;
  const char *cstr = JS_ToCStringLen(env->context, &cstr_len, value->value);

  size_t latin1_len = latin1_length_from_utf8((utf8_t *) cstr, cstr_len);

  if (str == NULL) {
    *result = latin1_len;
  } else if (len != 0) {
    size_t written = latin1_len < len ? latin1_len : len;

    utf8_convert_to_latin1((utf8_t *) cstr, cstr_len, str);

    if (written < len) str[written] = '\0';

    if (result) *result = written;
  } else if (result) *result = 0;

  JS_FreeCString(env->context, cstr);

  return 0;
}

int
js_get_value_external(js_env_t *env, js_value_t *value, void **result) {
  // Allow continuing even with a pending exception

  js_finalizer_t *finalizer = (js_finalizer_t *) JS_GetOpaque(value->value, env->classes.external);

  *result = finalizer->data;

  return 0;
}

int
js_get_value_date(js_env_t *env, js_value_t *value, double *result) {
  // Allow continuing even with a pending exception

  JS_ToFloat64(env->context, result, value->value);

  return 0;
}

int
js_get_array_length(js_env_t *env, js_value_t *array, uint32_t *result) {
  // Allow continuing even with a pending exception

  JSValue length = JS_GetPropertyStr(env->context, array->value, "length");

  JS_ToUint32(env->context, result, length);

  JS_FreeValue(env->context, length);

  return 0;
}

int
js_get_array_elements(js_env_t *env, js_value_t *array, js_value_t **elements, size_t len, size_t offset, uint32_t *result) {
  if (JS_HasException(env->context)) return js__error(env);

  int err;

  uint32_t written = 0;

  env->depth++;

  uint32_t m;
  err = js_get_array_length(env, array, &m);
  assert(err == 0);

  for (uint32_t i = 0, n = len, j = offset; i < n && j < m; i++, j++) {
    JSValue value = JS_GetPropertyUint32(env->context, array->value, j);

    if (JS_IsException(value)) {
      if (env->depth == 1) js__on_run_microtasks(env);

      env->depth--;

      if (env->depth == 0) {
        JSValue error = JS_GetException(env->context);

        js__on_uncaught_exception(env->context, error);
      }

      return js__error(env);
    }

    js_value_t *wrapper = malloc(sizeof(js_value_t));

    wrapper->value = value;

    elements[i] = wrapper;

    js__attach_to_handle_scope(env, env->scope, wrapper);

    written++;
  }

  if (env->depth == 1) js__on_run_microtasks(env);

  env->depth--;

  if (result) *result = written;

  return 0;
}

int
js_set_array_elements(js_env_t *env, js_value_t *array, const js_value_t *elements[], size_t len, size_t offset) {
  if (JS_HasException(env->context)) return js__error(env);

  env->depth++;

  for (uint32_t i = 0, n = len, j = offset; i < n; i++, j++) {
    int success = JS_SetPropertyUint32(env->context, array->value, j, JS_DupValue(env->context, elements[i]->value));

    if (env->depth == 1) js__on_run_microtasks(env);

    env->depth--;

    if (success < 0) {
      if (env->depth == 0) {
        JSValue error = JS_GetException(env->context);

        js__on_uncaught_exception(env->context, error);
      }

      return js__error(env);
    }
  }

  if (env->depth == 1) js__on_run_microtasks(env);

  env->depth--;

  return 0;
}

int
js_get_prototype(js_env_t *env, js_value_t *object, js_value_t **result) {
  // Allow continuing even with a pending exception

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = JS_GetPrototype(env->context, object->value);

  *result = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_get_property_names(js_env_t *env, js_value_t *object, js_value_t **result) {
  if (JS_HasException(env->context)) return js__error(env);

  int err;

  JSPropertyEnum *properties;
  uint32_t len;

  env->depth++;

  err = JS_GetOwnPropertyNames(env->context, &properties, &len, object->value, JS_GPN_ENUM_ONLY | JS_GPN_STRING_MASK);

  if (env->depth == 1) js__on_run_microtasks(env);

  env->depth--;

  if (err < 0) {
    if (env->depth == 0) {
      JSValue error = JS_GetException(env->context);

      js__on_uncaught_exception(env->context, error);
    }

    return js__error(env);
  }

  JSValue array = JS_NewArray(env->context);

  for (uint32_t i = 0; i < len; i++) {
    err = JS_SetPropertyUint32(env->context, object->value, i, JS_AtomToValue(env->context, properties[i].atom));
    if (err < 0) goto err;
  }

  if (result == NULL) JS_FreeValue(env->context, array);
  else {
    js_value_t *wrapper = malloc(sizeof(js_value_t));

    wrapper->value = array;

    *result = wrapper;

    js__attach_to_handle_scope(env, env->scope, wrapper);
  }

  return 0;

err:
  for (uint32_t i = 0; i < len; i++) {
    JS_FreeAtom(env->context, properties[i].atom);
  }

  free(properties);

  return js__error(env);
}

int
js_get_property(js_env_t *env, js_value_t *object, js_value_t *key, js_value_t **result) {
  if (JS_HasException(env->context)) return js__error(env);

  JSAtom atom = JS_ValueToAtom(env->context, key->value);

  env->depth++;

  JSValue value = JS_GetProperty(env->context, object->value, atom);

  JS_FreeAtom(env->context, atom);

  if (env->depth == 1) js__on_run_microtasks(env);

  env->depth--;

  if (JS_IsException(value)) {
    if (env->depth == 0) {
      JSValue error = JS_GetException(env->context);

      js__on_uncaught_exception(env->context, error);
    }

    return js__error(env);
  }

  if (result == NULL) JS_FreeValue(env->context, value);
  else {
    js_value_t *wrapper = malloc(sizeof(js_value_t));

    wrapper->value = value;

    *result = wrapper;

    js__attach_to_handle_scope(env, env->scope, wrapper);
  }

  return 0;
}

int
js_has_property(js_env_t *env, js_value_t *object, js_value_t *key, bool *result) {
  if (JS_HasException(env->context)) return js__error(env);

  JSAtom atom = JS_ValueToAtom(env->context, key->value);

  env->depth++;

  int success = JS_HasProperty(env->context, object->value, atom);

  JS_FreeAtom(env->context, atom);

  if (env->depth == 1) js__on_run_microtasks(env);

  env->depth--;

  if (success < 0) {
    if (env->depth == 0) {
      JSValue error = JS_GetException(env->context);

      js__on_uncaught_exception(env->context, error);
    }

    return js__error(env);
  }

  if (result) *result = success == 1;

  return 0;
}

int
js_has_own_property(js_env_t *env, js_value_t *object, js_value_t *key, bool *result) {
  if (JS_HasException(env->context)) return js__error(env);

  JSAtom atom = JS_ValueToAtom(env->context, key->value);

  env->depth++;

  int success = JS_HasProperty(env->context, object->value, atom);

  JS_FreeAtom(env->context, atom);

  if (env->depth == 1) js__on_run_microtasks(env);

  env->depth--;

  if (success < 0) {
    if (env->depth == 0) {
      JSValue error = JS_GetException(env->context);

      js__on_uncaught_exception(env->context, error);
    }

    return js__error(env);
  }

  if (result) *result = success == 1;

  return 0;
}

int
js_set_property(js_env_t *env, js_value_t *object, js_value_t *key, js_value_t *value) {
  if (JS_HasException(env->context)) return js__error(env);

  JSAtom atom = JS_ValueToAtom(env->context, key->value);

  env->depth++;

  int success = JS_SetProperty(env->context, object->value, atom, JS_DupValue(env->context, value->value));

  JS_FreeAtom(env->context, atom);

  if (env->depth == 1) js__on_run_microtasks(env);

  env->depth--;

  if (success < 0) {
    if (env->depth == 0) {
      JSValue error = JS_GetException(env->context);

      js__on_uncaught_exception(env->context, error);
    }

    return js__error(env);
  }

  return 0;
}

int
js_delete_property(js_env_t *env, js_value_t *object, js_value_t *key, bool *result) {
  if (JS_HasException(env->context)) return js__error(env);

  JSAtom atom = JS_ValueToAtom(env->context, key->value);

  env->depth++;

  int success = JS_DeleteProperty(env->context, object->value, atom, 0);

  JS_FreeAtom(env->context, atom);

  if (env->depth == 1) js__on_run_microtasks(env);

  env->depth--;

  if (success < 0) {
    if (env->depth == 0) {
      JSValue error = JS_GetException(env->context);

      js__on_uncaught_exception(env->context, error);
    }

    return js__error(env);
  }

  if (result) *result = success == 1;

  return 0;
}

int
js_get_named_property(js_env_t *env, js_value_t *object, const char *name, js_value_t **result) {
  if (JS_HasException(env->context)) return js__error(env);

  env->depth++;

  JSValue value = JS_GetPropertyStr(env->context, object->value, name);

  if (env->depth == 1) js__on_run_microtasks(env);

  env->depth--;

  if (JS_IsException(value)) {
    if (env->depth == 0) {
      JSValue error = JS_GetException(env->context);

      js__on_uncaught_exception(env->context, error);
    }

    return js__error(env);
  }

  if (result == NULL) JS_FreeValue(env->context, value);
  else {
    js_value_t *wrapper = malloc(sizeof(js_value_t));

    wrapper->value = value;

    *result = wrapper;

    js__attach_to_handle_scope(env, env->scope, wrapper);
  }

  return 0;
}

int
js_has_named_property(js_env_t *env, js_value_t *object, const char *name, bool *result) {
  if (JS_HasException(env->context)) return js__error(env);

  JSAtom atom = JS_NewAtom(env->context, name);

  env->depth++;

  int success = JS_HasProperty(env->context, object->value, atom);

  JS_FreeAtom(env->context, atom);

  if (env->depth == 1) js__on_run_microtasks(env);

  env->depth--;

  if (success < 0) {
    if (env->depth == 0) {
      JSValue error = JS_GetException(env->context);

      js__on_uncaught_exception(env->context, error);
    }

    return js__error(env);
  }

  if (result) *result = success == 1;

  return 0;
}

int
js_set_named_property(js_env_t *env, js_value_t *object, const char *name, js_value_t *value) {
  if (JS_HasException(env->context)) return js__error(env);

  env->depth++;

  int success = JS_SetPropertyStr(env->context, object->value, name, JS_DupValue(env->context, value->value));

  if (env->depth == 1) js__on_run_microtasks(env);

  env->depth--;

  if (success < 0) {
    if (env->depth == 0) {
      JSValue error = JS_GetException(env->context);

      js__on_uncaught_exception(env->context, error);
    }

    return js__error(env);
  }

  return 0;
}

int
js_delete_named_property(js_env_t *env, js_value_t *object, const char *name, bool *result) {
  if (JS_HasException(env->context)) return js__error(env);

  JSAtom atom = JS_NewAtom(env->context, name);

  env->depth++;

  int success = JS_DeleteProperty(env->context, object->value, atom, 0);

  JS_FreeAtom(env->context, atom);

  if (env->depth == 1) js__on_run_microtasks(env);

  env->depth--;

  if (success < 0) {
    if (env->depth == 0) {
      JSValue error = JS_GetException(env->context);

      js__on_uncaught_exception(env->context, error);
    }

    return js__error(env);
  }

  if (result) *result = success == 1;

  return 0;
}

int
js_get_element(js_env_t *env, js_value_t *object, uint32_t index, js_value_t **result) {
  if (JS_HasException(env->context)) return js__error(env);

  env->depth++;

  JSValue value = JS_GetPropertyUint32(env->context, object->value, index);

  if (env->depth == 1) js__on_run_microtasks(env);

  env->depth--;

  if (JS_IsException(value)) {
    if (env->depth == 0) {
      JSValue error = JS_GetException(env->context);

      js__on_uncaught_exception(env->context, error);
    }

    return js__error(env);
  }

  if (result == NULL) JS_FreeValue(env->context, value);
  else {
    js_value_t *wrapper = malloc(sizeof(js_value_t));

    wrapper->value = value;

    *result = wrapper;

    js__attach_to_handle_scope(env, env->scope, wrapper);
  }

  return 0;
}

int
js_has_element(js_env_t *env, js_value_t *object, uint32_t index, bool *result) {
  if (JS_HasException(env->context)) return js__error(env);

  JSAtom atom = JS_NewAtomUInt32(env->context, index);

  env->depth++;

  int success = JS_HasProperty(env->context, object->value, atom);

  JS_FreeAtom(env->context, atom);

  if (env->depth == 1) js__on_run_microtasks(env);

  env->depth--;

  if (success < 0) {
    if (env->depth == 0) {
      JSValue error = JS_GetException(env->context);

      js__on_uncaught_exception(env->context, error);
    }

    return js__error(env);
  }

  if (result) *result = success == 1;

  return 0;
}

int
js_set_element(js_env_t *env, js_value_t *object, uint32_t index, js_value_t *value) {
  if (JS_HasException(env->context)) return js__error(env);

  env->depth++;

  int success = JS_SetPropertyUint32(env->context, object->value, index, JS_DupValue(env->context, value->value));

  if (env->depth == 1) js__on_run_microtasks(env);

  env->depth--;

  if (success < 0) {
    if (env->depth == 0) {
      JSValue error = JS_GetException(env->context);

      js__on_uncaught_exception(env->context, error);
    }

    return js__error(env);
  }

  return 0;
}

int
js_delete_element(js_env_t *env, js_value_t *object, uint32_t index, bool *result) {
  if (JS_HasException(env->context)) return js__error(env);

  JSAtom atom = JS_NewAtomUInt32(env->context, index);

  env->depth++;

  int success = JS_DeleteProperty(env->context, object->value, atom, 0);

  JS_FreeAtom(env->context, atom);

  if (env->depth == 1) js__on_run_microtasks(env);

  env->depth--;

  if (success < 0) {
    if (env->depth == 0) {
      JSValue error = JS_GetException(env->context);

      js__on_uncaught_exception(env->context, error);
    }

    return js__error(env);
  }

  if (result) *result = success == 1;

  return 0;
}

int
js_get_string_view(js_env_t *env, js_value_t *string, js_string_encoding_t *encoding, const void **str, size_t *len, js_string_view_t **result) {
  // Allow continuing even with a pending exception

  js_string_view_t *view = malloc(sizeof(js_string_view_t));

  view->value = JS_ToCStringLen(env->context, len, string->value);

  if (encoding) *encoding = js_utf8;

  if (str) *str = view->value;

  *result = view;

  return 0;
}

int
js_release_string_view(js_env_t *env, js_string_view_t *view) {
  // Allow continuing even with a pending exception

  JS_FreeCString(env->context, view->value);

  free(view);

  return 0;
}

int
js_get_typedarray_view(js_env_t *env, js_value_t *typedarray, js_typedarray_type_t *type, void **data, size_t *len, js_typedarray_view_t **result) {
  // Allow continuing even with a pending exception

  js_get_typedarray_info(env, typedarray, type, data, len, NULL, NULL);

  *result = NULL;

  return 0;
}

int
js_release_typedarray_view(js_env_t *env, js_typedarray_view_t *view) {
  // Allow continuing even with a pending exception

  return 0;
}

int
js_get_dataview_view(js_env_t *env, js_value_t *dataview, void **data, size_t *len, js_dataview_view_t **result) {
  // Allow continuing even with a pending exception

  js_get_dataview_info(env, dataview, data, len, NULL, NULL);

  *result = NULL;

  return 0;
}

int
js_release_dataview_view(js_env_t *env, js_dataview_view_t *view) {
  // Allow continuing even with a pending exception

  return 0;
}

int
js_get_callback_info(js_env_t *env, const js_callback_info_t *info, size_t *argc, js_value_t *argv[], js_value_t **receiver, void **data) {
  // Allow continuing even with a pending exception

  if (argv) {
    size_t i = 0, n = info->argc < *argc ? info->argc : *argc;

    for (; i < n; i++) {
      js_value_t *wrapper = malloc(sizeof(js_value_t));

      wrapper->value = JS_DupValue(env->context, info->argv[i]);

      argv[i] = wrapper;

      js__attach_to_handle_scope(env, env->scope, wrapper);
    }

    n = *argc;

    if (i < n) {
      js_value_t *wrapper = malloc(sizeof(js_value_t));

      wrapper->value = JS_UNDEFINED;

      js__attach_to_handle_scope(env, env->scope, wrapper);

      for (; i < n; i++) {
        argv[i] = wrapper;
      }
    }
  }

  if (argc) {
    *argc = info->argc;
  }

  if (receiver) {
    js_value_t *wrapper = malloc(sizeof(js_value_t));

    wrapper->value = JS_DupValue(env->context, info->receiver);

    *receiver = wrapper;

    js__attach_to_handle_scope(env, env->scope, wrapper);
  }

  if (data) {
    *data = info->callback->data;
  }

  return 0;
}

int
js_get_typed_callback_info(const js_typed_callback_info_t *info, js_env_t **env, void **data) {
  // Allow continuing even with a pending exception

  return 0;
}

int
js_get_new_target(js_env_t *env, const js_callback_info_t *info, js_value_t **result) {
  // Allow continuing even with a pending exception

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = JS_DupValue(env->context, info->new_target);

  *result = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_get_arraybuffer_info(js_env_t *env, js_value_t *arraybuffer, void **pdata, size_t *plen) {
  // Allow continuing even with a pending exception

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
js_get_sharedarraybuffer_info(js_env_t *env, js_value_t *sharedarraybuffer, void **pdata, size_t *plen) {
  // Allow continuing even with a pending exception

  size_t len;

  uint8_t *data = JS_GetArrayBuffer(env->context, &len, sharedarraybuffer->value);

  if (pdata) {
    *pdata = data;
  }

  if (plen) {
    *plen = len;
  }

  return 0;
}

int
js_get_typedarray_info(js_env_t *env, js_value_t *typedarray, js_typedarray_type_t *ptype, void **pdata, size_t *plen, js_value_t **parraybuffer, size_t *poffset) {
  // Allow continuing even with a pending exception

  size_t offset, byte_len, bytes_per_element;

  JSValue arraybuffer = JS_GetTypedArrayBuffer(env->context, typedarray->value, &offset, &byte_len, &bytes_per_element);

  if (ptype) {
    JSValue global = JS_GetGlobalObject(env->context);

#define V(class, type) \
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

    V("Int8Array", js_int8array);
    V("Uint8Array", js_uint8array);
    V("Uint8ClampedArray", js_uint8clampedarray);
    V("Int16Array", js_int16array);
    V("Uint16Array", js_uint16array);
    V("Int32Array", js_int32array);
    V("Uint32Array", js_uint32array);
    V("Float32Array", js_float32array);
    V("Float64Array", js_float64array);
    V("BigInt64Array", js_bigint64array);
    V("BigUint64Array", js_biguint64array);
#undef V

  done:
    JS_FreeValue(env->context, global);
  }

  if (pdata) {
    size_t size;

    *pdata = JS_GetArrayBuffer(env->context, &size, arraybuffer) + offset;
  }

  if (plen) {
    *plen = byte_len / bytes_per_element;
  }

  if (parraybuffer) {
    js_value_t *wrapper = malloc(sizeof(js_value_t));

    wrapper->value = JS_DupValue(env->context, arraybuffer);

    *parraybuffer = wrapper;

    js__attach_to_handle_scope(env, env->scope, wrapper);
  }

  if (poffset) {
    *poffset = offset;
  }

  JS_FreeValue(env->context, arraybuffer);

  return 0;
}

int
js_get_dataview_info(js_env_t *env, js_value_t *dataview, void **pdata, size_t *plen, js_value_t **parraybuffer, size_t *poffset) {
  // Allow continuing even with a pending exception

  size_t offset;

  JSValue arraybuffer;

  if (pdata || poffset) {
    JSValue value = JS_GetPropertyStr(env->context, dataview->value, "byteOffset");

    JS_ToInt64(env->context, (int64_t *) &offset, value);

    JS_FreeValue(env->context, value);
  }

  if (pdata || parraybuffer) {
    arraybuffer = JS_GetPropertyStr(env->context, dataview->value, "buffer");
  }

  if (pdata) {
    size_t size;

    *pdata = JS_GetArrayBuffer(env->context, &size, arraybuffer) + offset;
  }

  if (plen) {
    JSValue value = JS_GetPropertyStr(env->context, dataview->value, "byteLength");

    JS_ToInt64(env->context, (int64_t *) plen, value);

    JS_FreeValue(env->context, value);
  }

  if (parraybuffer) {
    js_value_t *wrapper = malloc(sizeof(js_value_t));

    wrapper->value = JS_DupValue(env->context, arraybuffer);

    *parraybuffer = wrapper;

    js__attach_to_handle_scope(env, env->scope, wrapper);
  }

  if (poffset) {
    *poffset = offset;
  }

  if (pdata || parraybuffer) {
    JS_FreeValue(env->context, arraybuffer);
  }

  return 0;
}

int
js_call_function(js_env_t *env, js_value_t *recv, js_value_t *function, size_t argc, js_value_t *const argv[], js_value_t **result) {
  if (JS_HasException(env->context)) return js__error(env);

  JSValue *args = malloc(argc * sizeof(JSValue));

  for (size_t i = 0; i < argc; i++) {
    args[i] = argv[i]->value;
  }

  env->depth++;

  JSValue value = JS_Call(env->context, function->value, recv->value, argc, args);

  free(args);

  if (env->depth == 1) js__on_run_microtasks(env);

  env->depth--;

  if (JS_IsException(value)) {
    if (env->depth == 0) {
      JSValue error = JS_GetException(env->context);

      js__on_uncaught_exception(env->context, error);
    }

    return js__error(env);
  }

  if (result == NULL) JS_FreeValue(env->context, value);
  else {
    js_value_t *wrapper = malloc(sizeof(js_value_t));

    wrapper->value = value;

    *result = wrapper;

    js__attach_to_handle_scope(env, env->scope, wrapper);
  }

  return 0;
}

int
js_call_function_with_checkpoint(js_env_t *env, js_value_t *receiver, js_value_t *function, size_t argc, js_value_t *const argv[], js_value_t **result) {
  if (JS_HasException(env->context)) return js__error(env);

  JSValue *args = malloc(argc * sizeof(JSValue));

  for (size_t i = 0; i < argc; i++) {
    args[i] = argv[i]->value;
  }

  env->depth++;

  JSValue value = JS_Call(env->context, function->value, receiver->value, argc, args);

  free(args);

  js__on_run_microtasks(env);

  env->depth--;

  if (JS_IsException(value)) {
    JSValue error = JS_GetException(env->context);

    js__on_uncaught_exception(env->context, error);

    return js__error(env);
  }

  if (result == NULL) JS_FreeValue(env->context, value);
  else {
    js_value_t *wrapper = malloc(sizeof(js_value_t));

    wrapper->value = value;

    *result = wrapper;

    js__attach_to_handle_scope(env, env->scope, wrapper);
  }

  return 0;
}

int
js_new_instance(js_env_t *env, js_value_t *constructor, size_t argc, js_value_t *const argv[], js_value_t **result) {
  if (JS_HasException(env->context)) return js__error(env);

  JSValue *args = malloc(argc * sizeof(JSValue));

  for (size_t i = 0; i < argc; i++) {
    args[i] = argv[i]->value;
  }

  env->depth++;

  JSValue value = JS_CallConstructor(env->context, constructor->value, argc, args);

  free(args);

  env->depth--;

  if (JS_IsException(value)) {
    if (env->depth == 0) {
      JSValue error = JS_GetException(env->context);

      js__on_uncaught_exception(env->context, error);
    }

    return js__error(env);
  }

  if (result == NULL) JS_FreeValue(env->context, value);
  else {
    js_value_t *wrapper = malloc(sizeof(js_value_t));

    wrapper->value = value;

    *result = wrapper;

    js__attach_to_handle_scope(env, env->scope, wrapper);
  }

  return 0;
}

int
js_create_threadsafe_function(js_env_t *env, js_value_t *function, size_t queue_limit, size_t initial_thread_count, js_finalize_cb finalize_cb, void *finalize_hint, void *context, js_threadsafe_function_cb cb, js_threadsafe_function_t **result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_get_threadsafe_function_context(js_threadsafe_function_t *function, void **result) {
  return -1;
}

int
js_call_threadsafe_function(js_threadsafe_function_t *function, void *data, js_threadsafe_function_call_mode_t mode) {
  return -1;
}

int
js_acquire_threadsafe_function(js_threadsafe_function_t *function) {
  return -1;
}

int
js_release_threadsafe_function(js_threadsafe_function_t *function, js_threadsafe_function_release_mode_t mode) {
  return -1;
}

int
js_ref_threadsafe_function(js_env_t *env, js_threadsafe_function_t *function) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_unref_threadsafe_function(js_env_t *env, js_threadsafe_function_t *function) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_add_teardown_callback(js_env_t *env, js_teardown_cb callback, void *data) {
  if (JS_HasException(env->context)) return js__error(env);

  js_teardown_task_t *task = malloc(sizeof(js_teardown_task_t));

  task->type = js_immediate_teardown;
  task->immediate.cb = callback;
  task->data = data;

  intrusive_list_prepend(&env->teardown_queue.tasks, &task->list);

  return 0;
}

int
js_remove_teardown_callback(js_env_t *env, js_teardown_cb callback, void *data) {
  if (JS_HasException(env->context)) return js__error(env);

  if (env->destroying) return 0;

  intrusive_list_for_each(next, &env->teardown_queue.tasks) {
    js_teardown_task_t *task = intrusive_entry(next, js_teardown_task_t, list);

    if (task->type == js_immediate_teardown && task->immediate.cb == callback && task->data == data) {
      intrusive_list_remove(&env->teardown_queue.tasks, &task->list);

      free(task);

      return 0;
    }
  }

  return 0;
}

int
js_add_deferred_teardown_callback(js_env_t *env, js_deferred_teardown_cb callback, void *data, js_deferred_teardown_t **result) {
  if (JS_HasException(env->context)) return js__error(env);

  js_teardown_task_t *task = malloc(sizeof(js_teardown_task_t));

  task->type = js_deferred_teardown;
  task->deferred.cb = callback;
  task->deferred.handle.env = env;
  task->data = data;

  intrusive_list_prepend(&env->teardown_queue.tasks, &task->list);

  env->refs++;

  if (result) *result = &task->deferred.handle;

  return 0;
}

int
js_finish_deferred_teardown_callback(js_deferred_teardown_t *handle) {
  // Allow continuing even with a pending exception

  int err;

  js_env_t *env = handle->env;

  intrusive_list_for_each(next, &env->teardown_queue.tasks) {
    js_teardown_task_t *task = intrusive_entry(next, js_teardown_task_t, list);

    if (task->type == js_deferred_teardown && &task->deferred.handle == handle) {
      intrusive_list_remove(&env->teardown_queue.tasks, &task->list);

      if (--env->refs == 0 && env->destroying) {
        err = uv_async_send(&env->teardown);
        assert(err == 0);
      }

      free(task);

      return 0;
    }
  }

  return -1;
}

int
js_throw(js_env_t *env, js_value_t *error) {
  if (JS_HasException(env->context)) return js__error(env);

  JS_Throw(env->context, JS_DupValue(env->context, error->value));

  return 0;
}

int
js_vformat(char **result, size_t *size, const char *message, va_list args) {
  va_list args_copy;
  va_copy(args_copy, args);

  int res = vsnprintf(NULL, 0, message, args_copy);

  va_end(args_copy);

  if (res < 0) return res;

  *size = res + 1 /* NULL */;
  *result = malloc(*size);

  va_copy(args_copy, args);

  vsnprintf(*result, *size, message, args_copy);

  va_end(args_copy);

  return 0;
}

int
js_throw_error(js_env_t *env, const char *code, const char *message) {
  if (JS_HasException(env->context)) return js__error(env);

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
js_throw_verrorf(js_env_t *env, const char *code, const char *message, va_list args) {
  if (JS_HasException(env->context)) return js__error(env);

  size_t len;
  char *formatted;
  js_vformat(&formatted, &len, message, args);

  int err = js_throw_error(env, code, formatted);

  free(formatted);

  return err;
}

int
js_throw_errorf(js_env_t *env, const char *code, const char *message, ...) {
  if (JS_HasException(env->context)) return js__error(env);

  va_list args;
  va_start(args, message);

  int err = js_throw_verrorf(env, code, message, args);

  va_end(args);

  return err;
}

int
js_throw_type_error(js_env_t *env, const char *code, const char *message) {
  if (JS_HasException(env->context)) return js__error(env);

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
js_throw_type_verrorf(js_env_t *env, const char *code, const char *message, va_list args) {
  if (JS_HasException(env->context)) return js__error(env);

  size_t len;
  char *formatted;
  js_vformat(&formatted, &len, message, args);

  int err = js_throw_type_error(env, code, formatted);

  free(formatted);

  return err;
}

int
js_throw_type_errorf(js_env_t *env, const char *code, const char *message, ...) {
  if (JS_HasException(env->context)) return js__error(env);

  va_list args;
  va_start(args, message);

  int err = js_throw_type_verrorf(env, code, message, args);

  va_end(args);

  return err;
}

int
js_throw_range_error(js_env_t *env, const char *code, const char *message) {
  if (JS_HasException(env->context)) return js__error(env);

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
js_throw_range_verrorf(js_env_t *env, const char *code, const char *message, va_list args) {
  if (JS_HasException(env->context)) return js__error(env);

  size_t len;
  char *formatted;
  js_vformat(&formatted, &len, message, args);

  int err = js_throw_range_error(env, code, formatted);

  free(formatted);

  return err;
}

int
js_throw_range_errorf(js_env_t *env, const char *code, const char *message, ...) {
  if (JS_HasException(env->context)) return js__error(env);

  va_list args;
  va_start(args, message);

  int err = js_throw_range_verrorf(env, code, message, args);

  va_end(args);

  return err;
}

int
js_throw_syntax_error(js_env_t *env, const char *code, const char *message) {
  if (JS_HasException(env->context)) return js__error(env);

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
js_throw_syntax_verrorf(js_env_t *env, const char *code, const char *message, va_list args) {
  if (JS_HasException(env->context)) return js__error(env);

  size_t len;
  char *formatted;
  js_vformat(&formatted, &len, message, args);

  int err = js_throw_syntax_error(env, code, formatted);

  free(formatted);

  return err;
}

int
js_throw_syntax_errorf(js_env_t *env, const char *code, const char *message, ...) {
  if (JS_HasException(env->context)) return js__error(env);

  va_list args;
  va_start(args, message);

  int err = js_throw_syntax_verrorf(env, code, message, args);

  va_end(args);

  return err;
}

int
js_is_exception_pending(js_env_t *env, bool *result) {
  // Allow continuing even with a pending exception

  *result = JS_HasException(env->context);

  return 0;
}

int
js_get_and_clear_last_exception(js_env_t *env, js_value_t **result) {
  // Allow continuing even with a pending exception

  JSValue error = JS_GetException(env->context);

  if (JS_IsUninitialized(error)) return js_get_undefined(env, result);

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  wrapper->value = error;

  *result = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_fatal_exception(js_env_t *env, js_value_t *error) {
  // Allow continuing even with a pending exception

  js__on_uncaught_exception(env->context, JS_DupValue(env->context, error->value));

  return 0;
}

int
js_terminate_execution(js_env_t *env) {
  // Allow continuing even with a pending exception

  JS_ThrowInternalError(env->context, "terminated");

  JSValue error = JS_GetException(env->context);

  JS_SetUncatchableError(env->context, error);

  JS_Throw(env->context, error);

  return 0;
}

int
js_adjust_external_memory(js_env_t *env, int64_t change_in_bytes, int64_t *result) {
  // Allow continuing even with a pending exception

  env->external_memory += change_in_bytes;

  if (result) *result = env->external_memory;

  return 0;
}

int
js_request_garbage_collection(js_env_t *env) {
  // Allow continuing even with a pending exception

  if (env->platform->options.expose_garbage_collection) {
    JS_RunGC(env->runtime);
  }

  return 0;
}

int
js_get_heap_statistics(js_env_t *env, js_heap_statistics_t *result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_create_inspector(js_env_t *env, js_inspector_t **result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_destroy_inspector(js_env_t *env, js_inspector_t *inspector) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_on_inspector_response(js_env_t *env, js_inspector_t *inspector, js_inspector_message_cb cb, void *data) {
  return 0;
}

int
js_on_inspector_paused(js_env_t *env, js_inspector_t *inspector, js_inspector_paused_cb cb, void *data) {
  return 0;
}

int
js_connect_inspector(js_env_t *env, js_inspector_t *inspector) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_send_inspector_request(js_env_t *env, js_inspector_t *inspector, js_value_t *message) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}
