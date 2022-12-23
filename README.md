# libqjs

ABI compatible replacement for https://github.com/holepunchto/libjs built on QuickJS. Useful for embedded or otherwise restricted systems where the requirements of V8 can become problematic.

## Differences

Being built on QuickJS, the library has several differences from its V8 counterpart.

- **Strict memory management:** When using the `js_handle_scope_t` APIs, values are immediately freed when closing a scope. In V8, however, garbage collection happens at regular intervals and so values may be used for a short time even after the associated handle scope has been closed. In QuickJS, this will result in a use-after-free.

- **No weak references:** When using the `js_ref_t` APIs, the referenced JavaScript value is immediately freed when the reference count reaches 0. Attempts to get the value will result in `NULL` being returned even if other references to the JavaScript value exist and attempts to increase the reference count will fail.

## License

ISC
