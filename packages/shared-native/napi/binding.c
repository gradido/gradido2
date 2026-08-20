/*
 * The Node-API layer. Keep it thin: it should convert values and call into
 * src/, so the same code can also be linked into a test binary or a CLI.
 */

#include <node_api.h>

#include "shared_native.h"

static napi_value Add(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok || argc < 2) {
    napi_throw_type_error(env, NULL, "add(a, b) expects two numbers");
    return NULL;
  }

  int64_t a = 0;
  int64_t b = 0;
  napi_get_value_int64(env, argv[0], &a);
  napi_get_value_int64(env, argv[1], &b);

  napi_value result;
  napi_create_int64(env, shared_native_add(a, b), &result);
  return result;
}

NAPI_MODULE_INIT(/* env, exports */) {
  napi_property_descriptor properties[] = {
      {"add", NULL, Add, NULL, NULL, NULL, napi_default, NULL},
  };
  if (napi_define_properties(env, exports, 1, properties) != napi_ok) {
    napi_throw_error(env, NULL, "failed to define module properties");
    return NULL;
  }
  return exports;
}
