# vsomeip → bsomeip Migration Guide

## Quick Start

Replace your include:
```cpp
// Before:
#include <vsomeip/vsomeip.hpp>

// After:
#include <bsomeip/compat/vsomeip.hpp>
```

The compat shim puts everything in the `vsomeip` namespace with the same API
surface. Most code compiles unchanged.

## API Mapping

### Runtime & Application

| vsomeip | bsomeip (compat) | bsomeip (native) |
|---------|-------------------|-------------------|
| `vsomeip::runtime::get()` | Same | Not needed |
| `runtime->create_application("name")` | Same | `bsomeip::api::application app{config}` |
| `app->init()` | Returns `true` (no-op) | No init step |
| `app->start()` | Fires state handler | Use `io::uring_context` |
| `app->stop()` | Fires state handler | Destroy context |

### Messages

| vsomeip | bsomeip (compat) | bsomeip (native) |
|---------|-------------------|-------------------|
| `shared_ptr<message>` | Same | `bsomeip::api::message` (value type) |
| `create_request()` | Same | `message::create_request(svc, mth, cli, ses)` |
| `create_response(req)` | Same | `message::create_response(req.header())` |
| `msg->get_payload()` | Returns `shared_ptr<payload>` | `msg.payload()` → `span<byte>` |
| `msg->set_payload(pl)` | Same | Direct memcpy into payload span |
| `app->send(msg)` | Same | `app.route(msg.data)` |

### Service Registration

| vsomeip | bsomeip (compat) | bsomeip (native) |
|---------|-------------------|-------------------|
| `offer_service(s,i,maj,min)` | Same | Same (strong types) |
| `request_service(s,i)` | Same | Same (strong types) |
| `register_message_handler(s,i,m,h)` | Same | `register_message_handler(svc_id, mth_id, handler)` |

### Typed API (bsomeip only)

bsomeip provides typed APIs that have no vsomeip equivalent:

```cpp
// Instead of manual serialize/deserialize:
// vsomeip:
//   msg->get_payload()->get_data()  // raw bytes
//   memcpy(...)

// bsomeip native — typed RPC:
auto resp = sync_wait(
    proxy.async_call<AddRequest, AddResponse>(method_id{0x0421}, req)
).value();
// resp is a typed struct, no manual parsing

// bsomeip native — attribute pattern:
comm::attribute<VehicleSpeed, MyImpl> speed{skel, getter, setter, event};
speed.set(VehicleSpeed{.value = 120});  // auto-notifies subscribers

// bsomeip native — broadcast:
comm::broadcast<GpsPosition, MyImpl> gps{skel, event_id};
gps.fire(GpsPosition{.lat = 48.1, .lon = 11.5});
```

## Migration Steps

### Phase 1: Drop-in replacement
1. Replace `#include <vsomeip/vsomeip.hpp>` with `#include <bsomeip/compat/vsomeip.hpp>`
2. Build. Fix any compile errors (typically minor type differences).
3. Run tests. The compat layer handles the translation.

### Phase 2: Use strong types
Replace raw `uint16_t` IDs with bsomeip strong types:
```cpp
// Before:
app->offer_service(0x1234, 0x0001);

// After:
using namespace bsomeip::wire;
app.offer_service(service_id{0x1234}, instance_id{0x0001});
```

### Phase 3: Use typed codec
Define request/response as aggregates and use the reflection-based codec:
```cpp
struct AddRequest {
    std::uint32_t a;
    std::uint32_t b;
};
struct AddResponse {
    std::uint32_t sum;
};

// Automatic serialization — no manual byte packing
auto result = sync_wait(
    proxy.async_call<AddRequest, AddResponse>(method, AddRequest{3, 4})
).value();
assert(result.sum == 7);
```

### Phase 4: Use communication patterns
Replace manual event handling with `attribute`, `broadcast`, `rpc`:
```cpp
// Server
comm::attribute<Temperature, MyService> temp{skeleton, get_id, set_id, evt_id};
temp.set(Temperature{.celsius = 22.5});  // notifies subscribers on change

// Client
comm::attribute_proxy<Temperature> temp{proxy, get_id, set_id, evt_id};
auto val = sync_wait(temp.get()).value();
```

### Phase 5: Use sender/receiver
Replace callback-based flow with P2300 senders:
```cpp
// Before (vsomeip):
app->register_message_handler(svc, inst, mth,
    [](const shared_ptr<message>& msg) {
        // manual deserialize, process, serialize, send
    });

// After (bsomeip native):
skeleton.serve<Request, Response>(method_id{mth},
    [](MyImpl& impl, const Request& req) -> Response {
        return Response{.result = impl.compute(req)};
    });
```

### Phase 6: Remove compat header
Once all code uses native bsomeip APIs, remove the compat include.

## Key Differences

| Feature | vsomeip | bsomeip |
|---------|---------|---------|
| Memory model | `shared_ptr` everywhere | Value types, zero-copy views |
| Serialization | Manual byte packing | Compile-time reflection codec |
| Async model | Callbacks + threads | P2300 senders (stdexec) |
| I/O backend | boost::asio | io_uring (Linux) |
| E2E protection | AUTOSAR profiles | Same, as sender adaptors |
| Config | JSON files | Compile-time `app_config` struct |
| Dispatch | `mutex` + `unordered_map` | Lock-free `flat_map` + `inplace_handler` |
| Containers | STL defaults | `flat_map`, strong types |

## Limitations of the Compat Layer

- **No plugin system**: vsomeip's plugin architecture is not emulated.
- **No JSON config**: bsomeip uses compile-time configuration.
- **Instance ID in dispatch**: bsomeip dispatches by `(service, method)` only;
  the instance parameter in `register_message_handler` is ignored.
- **Event subscription**: vsomeip subscribes by `(service, instance, eventgroup, event)`;
  bsomeip uses `(service, eventgroup)`.
- **`start()` does not block**: vsomeip's `start()` runs the event loop.
  bsomeip uses explicit `io::uring_context::run()`.
- **Shared pointers**: The compat layer wraps bsomeip value types in `shared_ptr`
  to match vsomeip's API. This adds overhead. Native bsomeip avoids this entirely.
