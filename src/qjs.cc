#include <queue>

#include <js.h>
#include <quickjs.h>
#include <uv.h>

struct js_platform_s {
  uv_loop_t *loop;
  uv_prepare_t prepare;
  uv_check_t check;

  js_platform_s(uv_loop_t *loop)
      : loop(loop),
        prepare(),
        check() {
    uv_prepare_init(loop, &prepare);
    uv_prepare_start(&prepare, on_prepare);

    uv_check_init(loop, &check);
    uv_check_start(&check, on_check);

    // The check handle should not on its own keep the loop alive; it's simply
    // used for running any outstanding tasks that might cause additional work
    // to be queued.
    uv_unref(reinterpret_cast<uv_handle_t *>(&check));

    prepare.data = this;
    check.data = this;
  }

private:
  static void
  on_prepare (uv_prepare_t *handle) {
    auto platform = reinterpret_cast<js_platform_t *>(handle->data);
  }

  static void
  on_check (uv_check_t *handle) {
    auto platform = reinterpret_cast<js_platform_t *>(handle->data);

    if (uv_loop_alive(platform->loop)) return;
  }
};

struct js_env_s {
  uv_loop_t *loop;
  uv_prepare_t prepare;
  uv_check_t check;
  js_platform_t *platform;
  js_handle_scope_t *scope;
  JSRuntime *runtime;
  JSContext *context;

  js_env_s(uv_loop_t *loop, js_platform_t *platform, JSRuntime *runtime, JSContext *context)
      : loop(loop),
        prepare(),
        check(),
        platform(platform),
        scope(),
        runtime(runtime),
        context(context) {
    js_open_handle_scope(this, &scope);

    uv_prepare_init(loop, &prepare);
    uv_prepare_start(&prepare, on_prepare);

    uv_check_init(loop, &check);
    uv_check_start(&check, on_check);

    // The check handle should not on its own keep the loop alive; it's simply
    // used for running any outstanding tasks that might cause additional work
    // to be queued.
    uv_unref(reinterpret_cast<uv_handle_t *>(&check));

    prepare.data = this;
    check.data = this;
  }

  ~js_env_s() {
    js_close_handle_scope(this, scope);
  }

private:
  static void
  on_prepare (uv_prepare_t *handle) {
    auto env = reinterpret_cast<js_env_t *>(handle->data);
  }

  static void
  on_check (uv_check_t *handle) {
    auto env = reinterpret_cast<js_env_t *>(handle->data);

    if (uv_loop_alive(env->loop)) return;
  }
};

struct js_handle_scope_s {
  js_handle_scope_t *parent;
  std::queue<const js_value_t *> values;

  js_handle_scope_s(js_handle_scope_t *parent)
      : parent(parent),
        values() {}

  void
  push (const js_value_t *value) {
    values.push(value);
  }
};

struct js_value_s {
  JSContext *context;
  JSValue value;

  js_value_s(js_handle_scope_t *scope, JSContext *context, JSValue &&value)
      : context(context),
        value(std::move(value)) {
    scope->push(this);
  }

  ~js_value_s() {
    JS_FreeValue(context, value);
  }
};

struct js_ref_s {
};

int
js_create_platform (uv_loop_t *loop, js_platform_t **result) {
  auto platform = new js_platform_t(loop);

  *result = platform;

  return 0;
}

int
js_destroy_platform (js_platform_t *platform) {
  delete platform;

  return 0;
}

int
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

  auto env = new js_env_t(loop, platform, runtime, context);

  *result = env;

  return 0;
}

int
js_destroy_env (js_env_t *env) {
  JS_FreeContext(env->context);
  JS_FreeRuntime(env->runtime);

  delete env;

  return 0;
}

int
js_open_handle_scope (js_env_t *env, js_handle_scope_t **result) {
  auto scope = new js_handle_scope_t(env->scope);

  *result = env->scope = scope;

  return 0;
}

int
js_close_handle_scope (js_env_t *env, js_handle_scope_t *scope) {
  if (env->scope != scope) return -1;

  while (!scope->values.empty()) {
    auto value = scope->values.front();

    scope->values.pop();

    delete value;
  }

  env->scope = scope->parent;

  delete scope;

  return 0;
}

int
js_run_script (js_env_t *env, js_value_t *source, js_value_t **result) {
  const char *str = JS_ToCString(env->context, source->value);

  JSValue value = JS_Eval(env->context, str, -1, "<eval>", JS_EVAL_TYPE_GLOBAL);

  *result = new js_value_t(env->scope, env->context, std::move(value));

  return 0;
}

int
js_create_string_utf8 (js_env_t *env, const char *str, size_t len, js_value_t **result) {
  JSValue value;

  if (len == (size_t) -1) {
    value = JS_NewString(env->context, str);
  } else {
    value = JS_NewStringLen(env->context, str, len);
  }

  *result = new js_value_t(env->scope, env->context, std::move(value));

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
