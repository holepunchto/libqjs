# libqjs

ABI compatible replacement for https://github.com/holepunchto/libjs built on QuickJS. Useful for embedded or otherwise restricted systems where the requirements of V8 can become problematic.

## Differences

Being built on QuickJS, the library has some differences from its V8 counterpart.

- **Strict memory management:** When using the `js_handle_scope_t` APIs, values are immediately freed when closing a scope. In V8, however, garbage collection happens at regular intervals and so values may be used for a short time even after the associated handle scope has been closed. In QuickJS, this will result in a use-after-free.

## License

ISC
