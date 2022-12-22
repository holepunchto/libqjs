#include <map>
#include <queue>
#include <stack>
#include <vector>

#include <js.h>
#include <quickjs.h>
#include <uv.h>

typedef struct js_task_s js_task_t;
typedef struct js_callback_s js_callback_t;
typedef struct js_source_text_module_s js_source_text_module_t;
typedef struct js_synthetic_module_s js_synthetic_module_t;
typedef struct js_module_resolver_s js_module_resolver_t;
typedef struct js_module_evaluator_s js_module_evaluator_t;

typedef enum {
  js_source_text_module,
  js_synthetic_module
} js_module_type_t;

struct js_task_s {
  js_env_t *env;
  js_task_cb cb;
  void *data;

  js_task_s(js_env_t *env, js_task_cb cb, void *data)
      : env(env),
        cb(cb),
        data(data) {}

  inline void
  run () {
    cb(env, data);
  }
};

struct js_platform_s {
  js_platform_options_t options;
  uv_loop_t *loop;

  js_platform_s(js_platform_options_t options, uv_loop_t *loop)
      : options(options),
        loop(loop) {}
};

struct js_env_s {
  uv_loop_t *loop;
  uv_prepare_t prepare;
  uv_check_t check;
  js_platform_t *platform;
  js_handle_scope_t *scope;
  JSRuntime *runtime;
  JSContext *context;
  std::stack<js_module_resolver_t *> resolvers;
  std::map<uintptr_t, js_module_evaluator_t *> evaluators;

  js_env_s(uv_loop_t *loop, js_platform_t *platform, JSRuntime *runtime, JSContext *context)
      : loop(loop),
        prepare(),
        check(),
        platform(platform),
        scope(),
        runtime(runtime),
        context(context),
        resolvers() {
    JS_SetRuntimeOpaque(runtime, this);
    JS_SetContextOpaque(context, this);

    uv_prepare_init(loop, &prepare);
    uv_prepare_start(&prepare, on_prepare);
    prepare.data = this;

    uv_check_init(loop, &check);
    uv_check_start(&check, on_check);
    check.data = this;

    // The check handle should not on its own keep the loop alive; it's simply
    // used for running any outstanding tasks that might cause additional work
    // to be queued.
    uv_unref(reinterpret_cast<uv_handle_t *>(&check));

    js_open_handle_scope(this, &scope);
  }

  ~js_env_s() {
    js_close_handle_scope(this, scope);
  }

private:
  inline void
  run_microtasks () {
    JSContext *context;

    for (;;) {
      int err = JS_ExecutePendingJob(runtime, &context);
      if (err <= 0) break;
    }
  }

  inline void
  check_liveness () {
    if (true /* macrotask queue empty */) {
      uv_prepare_stop(&prepare);
    } else {
      uv_prepare_start(&prepare, on_prepare);
    }
  }

  static void
  on_prepare (uv_prepare_t *handle) {
    auto env = reinterpret_cast<js_env_t *>(handle->data);

    env->check_liveness();
  }

  static void
  on_check (uv_check_t *handle) {
    auto env = reinterpret_cast<js_env_t *>(handle->data);

    env->run_microtasks();

    if (uv_loop_alive(env->loop)) return;

    env->check_liveness();
  }
};

struct js_value_s {
  JSContext *context;
  JSValue value;

  js_value_s(JSContext *context, JSValue value)
      : context(context),
        value(value) {}

  js_value_s(const js_value_s &that)
      : context(that.context),
        value(that.value) {
    JS_DupValue(context, value);
  }

  ~js_value_s() {
    JS_FreeValue(context, value);
  }

  js_value_s &
  operator=(const js_value_s &that) {
    context = that.context;
    value = that.value;

    JS_DupValue(context, value);

    return *this;
  }
};

struct js_handle_scope_s {
  js_handle_scope_t *parent;
  std::queue<js_value_t *> values;

  js_handle_scope_s(js_handle_scope_t *parent)
      : parent(parent),
        values() {}

  inline void
  push (js_value_t *value) {
    values.push(value);
  }

  inline void
  close () {
    while (!values.empty()) {
      auto value = values.front();
      values.pop();

      delete value;
    }
  }
};

struct js_escapable_handle_scope_s : public js_handle_scope_s {
  bool escaped;

  js_escapable_handle_scope_s(js_handle_scope_t *parent)
      : js_handle_scope_t(parent),
        escaped(false) {}
};

struct js_module_s {
  js_module_type_t type;
  JSContext *context;
  void *data;

  js_module_s(js_module_type_t type, JSContext *context, void *data)
      : type(type),
        context(context),
        data(data) {}
};

struct js_source_text_module_s : js_module_t {
  JSValue bytecode;

  js_source_text_module_s(JSContext *context, void *data, JSValue bytecode = JS_NULL)
      : js_module_t(js_source_text_module, context, data),
        bytecode(bytecode) {}
};

struct js_synthetic_module_s : js_module_t {
  JSModuleDef *definition;

  js_synthetic_module_s(JSContext *context, void *data, JSModuleDef *definition)
      : js_module_t(js_synthetic_module, context, data),
        definition(definition) {}
};

struct js_module_resolver_s {
  js_source_text_module_t *module;
  js_module_cb cb;

  js_module_resolver_s(js_source_text_module_t *module, js_module_cb cb)
      : module(module),
        cb(cb) {}
};

struct js_module_evaluator_s {
  js_synthetic_module_t *module;
  js_synethic_module_cb cb;

  js_module_evaluator_s(js_synthetic_module_t *module, js_synethic_module_cb cb)
      : module(module),
        cb(cb) {}
};

struct js_ref_s {
  JSContext *context;
  JSValue value;
  uint32_t count;

  js_ref_s(JSContext *context, JSValue value, uint32_t count)
      : context(context),
        value(value),
        count(count) {}

  js_ref_s(const js_ref_s &) = delete;

  js_value_s &
  operator=(const js_value_s &that) = delete;
};

struct js_callback_s {
  js_env_t *env;
  js_function_cb cb;
  void *data;

  js_callback_s(js_env_t *env, js_function_cb cb, void *data)
      : env(env),
        cb(cb),
        data(data) {}
};

struct js_callback_info_s {
  std::vector<js_value_t *> args;
  js_value_t *self;
  void *data;

  js_callback_info_s(std::vector<js_value_t *> args, js_value_t *self, void *data)
      : args(args),
        self(self),
        data(data) {}
};

extern "C" int
js_create_platform (uv_loop_t *loop, const js_platform_options_t *options, js_platform_t **result) {
  auto platform = new js_platform_t(options ? *options : js_platform_options_t(), loop);

  *result = platform;

  return 0;
}

extern "C" int
js_destroy_platform (js_platform_t *platform) {
  delete platform;

  return 0;
}

extern "C" int
js_get_platform_loop (js_platform_t *platform, uv_loop_t **result) {
  *result = platform->loop;

  return 0;
}

static JSModuleDef *
on_resolve_module (JSContext *context, const char *name, void *opaque) {
  auto env = reinterpret_cast<js_env_t *>(JS_GetContextOpaque(context));

  auto resolver = env->resolvers.top();

  js_handle_scope_t *scope;
  js_open_handle_scope(env, &scope);

  auto specifier = new js_value_t(context, JS_NewString(context, name));

  env->scope->push(specifier);

  auto assertions = new js_value_t(context, JS_NULL);

  env->scope->push(assertions);

  auto referrer = resolver->module;

  auto module = resolver->cb(
    env,
    specifier,
    assertions,
    referrer,
    referrer->data
  );

  js_close_handle_scope(env, scope);

  switch (module->type) {
  case js_source_text_module: {
    auto bytecode = reinterpret_cast<js_source_text_module_t *>(module)->bytecode;

    return reinterpret_cast<JSModuleDef *>(JS_VALUE_GET_PTR(bytecode));
  }

  case js_synthetic_module: {
    auto definition = reinterpret_cast<js_synthetic_module_t *>(module)->definition;

    return definition;
  }
  }
}

extern "C" int
js_create_env (uv_loop_t *loop, js_platform_t *platform, js_env_t **result) {
  auto runtime = JS_NewRuntime();
  auto context = JS_NewContextRaw(runtime);

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

  JS_SetModuleLoaderFunc(runtime, nullptr, on_resolve_module, nullptr);

  auto env = new js_env_t(loop, platform, runtime, context);

  *result = env;

  return 0;
}

extern "C" int
js_destroy_env (js_env_t *env) {
  auto runtime = env->runtime;
  auto context = env->context;

  delete env;

  JS_FreeContext(context);
  JS_FreeRuntime(runtime);

  return 0;
}

extern "C" int
js_get_env_loop (js_env_t *env, uv_loop_t **result) {
  *result = env->loop;

  return 0;
}

extern "C" int
js_open_handle_scope (js_env_t *env, js_handle_scope_t **result) {
  auto scope = new js_handle_scope_t(env->scope);

  env->scope = scope;

  *result = scope;

  return 0;
}

extern "C" int
js_close_handle_scope (js_env_t *env, js_handle_scope_t *scope) {
  if (env->scope != scope) return -1;

  env->scope->close();

  env->scope = scope->parent;

  delete scope;

  return 0;
}

extern "C" int
js_open_escapable_handle_scope (js_env_t *env, js_escapable_handle_scope_t **result) {
  auto scope = new js_escapable_handle_scope_t(env->scope);

  env->scope = scope;

  *result = scope;

  return 0;
}

extern "C" int
js_close_escapable_handle_scope (js_env_t *env, js_escapable_handle_scope_t *scope) {
  if (env->scope != scope) return -1;

  env->scope->close();

  env->scope = scope->parent;

  delete scope;

  return 0;
}

extern "C" int
js_escape_handle (js_env_t *env, js_escapable_handle_scope_t *scope, js_value_t *escapee, js_value_t **result) {
  if (env->scope != scope || scope->escaped) return -1;

  scope->escaped = true;

  *result = new js_value_t(*escapee);

  scope->parent->push(*result);

  return 0;
}

extern "C" int
js_run_script (js_env_t *env, js_value_t *source, js_value_t **result) {
  size_t str_len;
  const char *str = JS_ToCStringLen(env->context, &str_len, source->value);

  JSValue value = JS_Eval(env->context, str, str_len, "<anonymous>", JS_EVAL_TYPE_GLOBAL);

  JS_FreeCString(env->context, str);

  *result = new js_value_t(env->context, value);

  env->scope->push(*result);

  return 0;
}

extern "C" int
js_create_module (js_env_t *env, const char *name, size_t len, js_value_t *source, js_module_cb cb, void *data, js_module_t **result) {
  auto module = new js_source_text_module_t(env->context, data);

  auto resolver = js_module_resolver_t(module, cb);

  env->resolvers.push(&resolver);

  size_t str_len;
  const char *str = JS_ToCStringLen(env->context, &str_len, source->value);

  module->bytecode = JS_Eval(
    env->context,
    str,
    str_len,
    name,
    JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY
  );

  JS_FreeCString(env->context, str);

  env->resolvers.pop();

  *result = module;

  return 0;
}

static int
on_evaluate_module (JSContext *context, JSModuleDef *definition) {
  auto env = reinterpret_cast<js_env_t *>(JS_GetContextOpaque(context));

  auto evaluator = env->evaluators.at(reinterpret_cast<uintptr_t>(definition));

  auto module = evaluator->module;

  evaluator->cb(env, module, module->data);

  return 0;
}

extern "C" int
js_create_synthetic_module (js_env_t *env, const char *name, size_t len, const js_value_t *export_names[], size_t names_len, js_synethic_module_cb cb, void *data, js_module_t **result) {
  auto definition = JS_NewCModule(env->context, name, on_evaluate_module);

  auto module = new js_synthetic_module_t(env->context, data, definition);

  for (size_t i = 0; i < names_len; i++) {
    auto export_name = export_names[i];

    const char *str = JS_ToCString(env->context, export_name->value);

    JS_AddModuleExport(env->context, definition, str);

    JS_FreeCString(env->context, str);
  }

  auto evaluator = new js_module_evaluator_t(module, cb);

  env->evaluators.emplace(reinterpret_cast<uintptr_t>(definition), evaluator);

  *result = module;

  return 0;
}

extern "C" int
js_delete_module (js_env_t *env, js_module_t *module) {
  delete module;

  return 0;
}

extern "C" int
js_set_module_export (js_env_t *env, js_module_t *module, js_value_t *name, js_value_t *value) {
  if (module->type != js_synthetic_module) return -1;

  auto definition = reinterpret_cast<js_synthetic_module_t *>(module)->definition;

  const char *str = JS_ToCString(env->context, name->value);

  JS_DupValue(env->context, value->value);

  JS_SetModuleExport(env->context, definition, str, value->value);

  JS_FreeCString(env->context, str);

  return 0;
}

extern "C" int
js_run_module (js_env_t *env, js_module_t *module, js_value_t **result) {
  if (module->type != js_source_text_module) return -1;

  auto bytecode = reinterpret_cast<js_source_text_module_t *>(module)->bytecode;

  *result = new js_value_t(env->context, JS_EvalFunction(env->context, bytecode));

  return 0;
}

extern "C" int
js_create_reference (js_env_t *env, js_value_t *value, uint32_t count, js_ref_t **result) {
  auto reference = new js_ref_t(value->context, value->value, count);

  if (reference->count > 0) JS_DupValue(reference->context, reference->value);

  *result = reference;

  return 0;
}

extern "C" int
js_delete_reference (js_env_t *env, js_ref_t *reference) {
  if (reference->count > 0) JS_FreeValue(reference->context, reference->value);

  delete reference;

  return 0;
}

extern "C" int
js_reference_ref (js_env_t *env, js_ref_t *reference, uint32_t *result) {
  if (reference->count == 0) return -1;

  reference->count++;

  if (result != nullptr) {
    *result = reference->count;
  }

  return 0;
}

extern "C" int
js_reference_unref (js_env_t *env, js_ref_t *reference, uint32_t *result) {
  if (reference->count == 0) return -1;

  reference->count--;

  if (reference->count == 0) JS_FreeValue(reference->context, reference->value);

  if (result != nullptr) {
    *result = reference->count;
  }

  return 0;
}

extern "C" int
js_get_reference_value (js_env_t *env, js_ref_t *reference, js_value_t **result) {
  if (reference->count == 0) {
    *result = nullptr;
  } else {
    JS_DupValue(reference->context, reference->value);

    *result = new js_value_t(reference->context, reference->value);

    env->scope->push(*result);
  }

  return 0;
}

extern "C" int
js_create_int32 (js_env_t *env, int32_t value, js_value_t **result) {
  *result = new js_value_t(env->context, JS_NewInt32(env->context, value));

  env->scope->push(*result);

  return 0;
}

extern "C" int
js_create_uint32 (js_env_t *env, uint32_t value, js_value_t **result) {
  *result = new js_value_t(env->context, JS_NewUint32(env->context, value));

  env->scope->push(*result);

  return 0;
}

extern "C" int
js_create_string_utf8 (js_env_t *env, const char *str, size_t len, js_value_t **result) {
  JSValue value;

  if (len == (size_t) -1) {
    value = JS_NewString(env->context, str);
  } else {
    value = JS_NewStringLen(env->context, str, len);
  }

  *result = new js_value_t(env->context, value);

  env->scope->push(*result);

  return 0;
}

extern "C" int
js_create_object (js_env_t *env, js_value_t **result) {
  *result = new js_value_t(env->context, JS_NewObject(env->context));

  env->scope->push(*result);

  return 0;
}

static JSClassID js_function_data_class_id;

static uv_once_t js_function_init = UV_ONCE_INIT;

static void
on_function_init () {
  JS_NewClassID(&js_function_data_class_id);
}

static JSValue
on_function_call (JSContext *context, JSValueConst self, int argc, JSValueConst *argv, int magic, JSValue *data) {
  auto callback = reinterpret_cast<js_callback_t *>(JS_GetOpaque(*data, js_function_data_class_id));

  auto env = callback->env;

  js_handle_scope_t *scope;
  js_open_handle_scope(env, &scope);

  auto args = std::vector<js_value_t *>();
  args.reserve(argc);

  for (int i = 0; i < argc; i++) {
    JS_DupValue(context, argv[i]);

    auto arg = new js_value_t(env->context, argv[i]);

    env->scope->push(arg);

    args.push_back(arg);
  }

  JS_DupValue(context, self);

  auto receiver = new js_value_t(env->context, self);

  env->scope->push(receiver);

  auto callback_info = js_callback_info_t(args, receiver, callback->data);

  auto result = callback->cb(env, &callback_info);

  JSValue value;

  if (result == nullptr) value = JS_UNDEFINED;
  else {
    value = result->value;

    JS_DupValue(context, value);
  }

  js_close_handle_scope(env, scope);

  return value;
}

extern "C" int
js_create_function (js_env_t *env, const char *name, size_t len, js_function_cb cb, void *data, js_value_t **result) {
  uv_once(&js_function_init, on_function_init);

  auto callback = new js_callback_t(env, cb, data);

  auto external = js_value_t(env->context, JS_NewObjectClass(env->context, js_function_data_class_id));

  JS_SetOpaque(external.value, callback);

  *result = new js_value_t(env->context, JS_NewCFunctionData(env->context, on_function_call, 0, 0, 1, &external.value));

  env->scope->push(*result);

  return 0;
}

extern "C" int
js_get_callback_info (js_env_t *env, const js_callback_info_t *info, size_t *argc, js_value_t *argv[], js_value_t **self, void **data) {
  if (argc) *argc = info->args.size();
  if (argv) *argv = *info->args.data();
  if (self) *self = info->self;
  if (data) *data = info->data;

  return 0;
}

extern "C" int
js_get_global (js_env_t *env, js_value_t **result) {
  *result = new js_value_t(env->context, JS_GetGlobalObject(env->context));

  env->scope->push(*result);

  return 0;
}

extern "C" int
js_get_null (js_env_t *env, js_value_t **result) {
  *result = new js_value_t(env->context, JS_NULL);

  env->scope->push(*result);

  return 0;
}

extern "C" int
js_get_undefined (js_env_t *env, js_value_t **result) {
  *result = new js_value_t(env->context, JS_UNDEFINED);

  env->scope->push(*result);

  return 0;
}

extern "C" int
js_get_boolean (js_env_t *env, bool value, js_value_t **result) {
  *result = new js_value_t(env->context, value ? JS_TRUE : JS_FALSE);

  env->scope->push(*result);

  return 0;
}

extern "C" int
js_get_value_int32 (js_env_t *env, js_value_t *value, int32_t *result) {
  JS_ToInt32(env->context, result, value->value);

  return 0;
}

extern "C" int
js_get_value_uint32 (js_env_t *env, js_value_t *value, uint32_t *result) {
  JS_ToUint32(env->context, result, value->value);

  return 0;
}

extern "C" int
js_get_value_string_utf8 (js_env_t *env, js_value_t *value, char *str, size_t len, size_t *result) {
  size_t cstr_len;
  const char *cstr = JS_ToCStringLen(env->context, &cstr_len, value->value);

  len = std::clamp(cstr_len, static_cast<size_t>(0), len - 1);

  if (str == nullptr) {
    *result = cstr_len;
  } else if (len != 0) {
    strncpy(str, cstr, len);

    str[len] = '\0';

    if (result != nullptr) {
      *result = len;
    }
  } else if (result != nullptr) {
    *result = 0;
  }

  JS_FreeCString(env->context, cstr);

  return 0;
}

extern "C" int
js_get_named_property (js_env_t *env, js_value_t *object, const char *name, js_value_t **result) {
  *result = new js_value_t(env->context, JS_GetPropertyStr(env->context, object->value, name));

  env->scope->push(*result);

  return 0;
}

extern "C" int
js_set_named_property (js_env_t *env, js_value_t *object, const char *name, js_value_t *value) {
  JS_DupValue(env->context, value->value);

  JS_SetPropertyStr(env->context, object->value, name, value->value);

  return 0;
}

static JSClassID js_job_data_class_id;

static uv_once_t js_job_init = UV_ONCE_INIT;

static void
on_job_init () {
  JS_NewClassID(&js_job_data_class_id);
}

static JSValue
on_job (JSContext *context, int argc, JSValueConst *argv) {
  auto env = reinterpret_cast<js_env_t *>(JS_GetContextOpaque(context));

  auto external = *argv;

  auto task = reinterpret_cast<js_task_t *>(JS_GetOpaque(external, js_job_data_class_id));

  task->run();

  return JS_NULL;
}

extern "C" int
js_queue_microtask (js_env_t *env, js_task_cb cb, void *data) {
  uv_once(&js_job_init, on_job_init);

  auto task = new js_task_t(env, cb, data);

  auto external = js_value_t(env->context, JS_NewObjectClass(env->context, js_job_data_class_id));

  JS_SetOpaque(external.value, task);

  JS_EnqueueJob(env->context, on_job, 1, &external.value);

  return 0;
}

extern "C" int
js_request_garbage_collection (js_env_t *env) {
  if (!env->platform->options.expose_garbage_collection) return -1;

  JS_RunGC(env->runtime);

  return 0;
}
