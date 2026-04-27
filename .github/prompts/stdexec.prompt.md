# stdexec — C++26 std::execution (P2300) Reference Implementation

> Use this skill when writing asynchronous C++ code using stdexec (NVIDIA/stdexec), the reference implementation of P2300 `std::execution`. Covers sender/receiver model, algorithms, schedulers, coroutines, and advanced patterns.

---

## 1. Core Mental Model

stdexec is a **lazy, composable async execution framework**. The pipeline is:

```
Build (lazy) → Compose (| operator) → Connect (sender+receiver→opstate) → Start → Complete
```

**Nothing executes until `start()` is called on an operation state.** Senders are pure descriptions of work.

---

## 2. Three Completion Signals

Every sender completes with exactly ONE of:

| Signal | CPO | Meaning |
|--------|-----|---------|
| Value | `set_value(receiver, args...)` | Success with results |
| Error | `set_error(receiver, error)` | Failure |
| Stopped | `set_stopped(receiver)` | Cancellation |

---

## 3. Core Types & Concepts

```cpp
#include <stdexec/execution.hpp>    // Master include
#include <exec/static_thread_pool.hpp>  // Thread pool scheduler
#include <exec/async_scope.hpp>     // Structured concurrency
#include <exec/task.hpp>            // Coroutine task
```

| Concept | Purpose |
|---------|---------|
| `stdexec::sender` | Describes async work (lazy) |
| `stdexec::receiver` | Accepts completion signals |
| `stdexec::scheduler` | Creates senders for scheduling |
| `stdexec::operation_state` | Connected sender+receiver, ready to `start()` |

---

## 4. Algorithm Cheat Sheet

### 4.1 Factory Senders (Create Work)

```cpp
stdexec::just(42, "hello")       // Immediate values → set_value(rcvr, 42, "hello")
stdexec::just_error(err)         // Immediate error
stdexec::just_stopped()          // Immediate cancellation
stdexec::schedule(scheduler)     // Schedule on executor → set_value(rcvr)
stdexec::read_env(query)         // Read from receiver's environment
```

### 4.2 Adaptors (Transform/Compose)

```cpp
// Value transformation (sync function)
sender | stdexec::then([](int x) { return x * 2; })

// Error handling
sender | stdexec::upon_error([](auto err) { return fallback; })
sender | stdexec::upon_stopped([] { return default_val; })

// Dynamic composition (function returns a NEW sender)
sender | stdexec::let_value([](int x) { return stdexec::just(x + 1); })
sender | stdexec::let_error([](auto e) { return recovery_sender; })
sender | stdexec::let_stopped([] { return fallback_sender; })

// Scheduler transitions
stdexec::starts_on(sched, sender)          // Start sender on sched
stdexec::continues_on(sender, sched)       // Continue after sender on sched
stdexec::on(sched, sender)                 // Run on sched, return to caller's scheduler
sender | stdexec::on(sched, closure)       // Run closure part on sched

// Bulk parallel
sender | stdexec::bulk(count, [](std::size_t idx, auto&... results) { ... })
```

### 4.3 Multi-Sender Combinators

```cpp
// Wait for ALL (tuple of results)
stdexec::when_all(sender1, sender2, sender3)

// With variant handling
stdexec::when_all_with_variant(sender1, sender2)
```

### 4.4 Consumers (Execute & Wait)

```cpp
// Blocking wait — returns std::optional<std::tuple<Args...>>
auto [result] = stdexec::sync_wait(sender).value();

// Fire-and-forget
stdexec::start_detached(sender);
```

---

## 5. The `|` Pipe Operator

All adaptors support pipe composition:

```cpp
auto pipeline =
    stdexec::schedule(sched)
  | stdexec::then([] { return fetch_data(); })
  | stdexec::let_value([](Data d) {
      return stdexec::just(process(d));
    })
  | stdexec::then([](Result r) { save(r); });

auto [val] = stdexec::sync_wait(std::move(pipeline)).value();
```

---

## 6. Completion Signatures

Senders declare their completions statically:

```cpp
using my_sigs = stdexec::completion_signatures<
    stdexec::set_value_t(int, double),     // Can succeed with (int, double)
    stdexec::set_error_t(std::exception_ptr),  // Can fail
    stdexec::set_stopped_t()               // Can be cancelled
>;
```

Query signatures from a sender:

```cpp
using sigs = stdexec::completion_signatures_of_t<MySender, MyEnv>;
```

---

## 7. Schedulers & Thread Pools

```cpp
// Static thread pool (work-stealing, per-thread queues)
exec::static_thread_pool pool{8};
auto sched = pool.get_scheduler();

// Run loop (single-threaded, manual drive)
stdexec::run_loop loop;
auto sched = loop.get_scheduler();
// ... start senders ...
loop.run();  // Blocks, drives the loop

// Inline scheduler (execute immediately in caller's context)
stdexec::inline_scheduler sched;
```

### Parallel execution pattern:

```cpp
exec::static_thread_pool pool{4};
auto sched = pool.get_scheduler();

auto work = stdexec::when_all(
    stdexec::starts_on(sched, stdexec::just(0) | stdexec::then(compute)),
    stdexec::starts_on(sched, stdexec::just(1) | stdexec::then(compute)),
    stdexec::starts_on(sched, stdexec::just(2) | stdexec::then(compute))
);

auto [a, b, c] = stdexec::sync_wait(std::move(work)).value();
```

---

## 8. Coroutines (`exec::task`)

```cpp
#include <exec/task.hpp>

exec::task<int> async_compute(exec::static_thread_pool::scheduler sched) {
    co_await stdexec::schedule(sched);  // Transfer to pool
    int x = co_await stdexec::just(42); // co_await any sender
    co_return x * 2;
}

// Launch:
auto [result] = stdexec::sync_wait(async_compute(sched)).value();
```

**Rules:**
- `co_await` any sender to get its value result
- `co_return` to complete the task sender with a value
- Stop tokens propagate automatically through coroutine chains
- Scheduler affinity: sticky (stays on scheduler) vs transferring

---

## 9. Structured Concurrency (`exec::async_scope`)

```cpp
#include <exec/async_scope.hpp>

exec::async_scope scope;

// Nest senders into scope (increments active count)
scope.spawn(stdexec::starts_on(sched, work1));
scope.spawn(stdexec::starts_on(sched, work2));

// Wait for all spawned work to complete
stdexec::sync_wait(
    stdexec::starts_on(sched, scope.on_empty())
);

// Request cancellation of all nested work
scope.request_stop();
```

---

## 10. Environment & Queries

Environments carry contextual information through the sender chain:

```cpp
// Standard queries
stdexec::get_scheduler(env)             // Current scheduler
stdexec::get_stop_token(env)            // Cancellation token
stdexec::get_delegation_scheduler(env)  // Scheduler for delegation

// Custom environment
auto env = stdexec::prop(stdexec::get_scheduler, my_sched);

// Write environment into sender chain
sender | stdexec::write_env(env)
```

---

## 11. Custom Sender (Advanced)

### Minimal custom sender:

```cpp
struct my_sender {
    using sender_concept = stdexec::sender_tag;
    using completion_signatures = stdexec::completion_signatures<
        stdexec::set_value_t(int),
        stdexec::set_error_t(std::exception_ptr)
    >;

    template <stdexec::receiver Receiver>
    struct op_state {
        Receiver rcvr_;
        
        friend void tag_invoke(stdexec::start_t, op_state& self) noexcept {
            try {
                stdexec::set_value(std::move(self.rcvr_), 42);
            } catch (...) {
                stdexec::set_error(std::move(self.rcvr_), std::current_exception());
            }
        }
    };

    template <stdexec::receiver Receiver>
    friend auto tag_invoke(stdexec::connect_t, my_sender, Receiver rcvr) {
        return op_state<Receiver>{std::move(rcvr)};
    }
};
```

### Modern style (member functions, preferred):

```cpp
struct my_sender {
    using sender_concept = stdexec::sender_tag;
    using completion_signatures = stdexec::completion_signatures<
        stdexec::set_value_t(int)
    >;

    template <class Receiver>
    auto connect(Receiver rcvr) const {
        return my_op_state<Receiver>{std::move(rcvr)};
    }
};
```

---

## 12. Internal Architecture (For Library Hackers)

### 12.1 `__sexpr` — Sender Expression Template

All built-in senders are `__sexpr<DescriptorFn>`, a tagged tuple:

```
__sexpr = (Tag, Data, Child1, Child2, ...)
```

- **Tag**: Algorithm identifier (`then_t`, `let_value_t`, `when_all_t`, ...)
- **Data**: Algorithm-specific state (the callback function, scheduler, etc.)
- **Children**: Predecessor senders in the composition chain

### 12.2 Per-Algorithm Customization

Each tag specializes `__sexpr_impl<Tag>` with:

```cpp
template <>
struct __sexpr_impl<then_t> : __sexpr_defaults {
    static auto __get_state(Sender&&, Receiver&&);    // Extract per-op state
    static auto __get_env(Index, State const&);       // Per-child environment
    static auto __get_attrs(Data const&, Child const&...);  // Sender attributes
    static auto __complete(Index, State&, SetTag, Args...);  // Completion handler
    static auto __start(State&, ChildOps&...);        // Initiate execution
};
```

### 12.3 Domain System

Domains control algorithm dispatch — enabling custom implementations:

```cpp
struct my_domain {
    // Intercept and transform senders before connection
    template <class OpTag, class Sender, class Env>
    auto transform_sender(OpTag, Sender&& sndr, Env const& env) const;
    
    // Custom environment transformation
    template <class OpTag, class Env>
    auto transform_env(OpTag, Env const& env) const;
};
```

**Lookup order**: completing domain → starting domain → `default_domain`

### 12.4 Two-Phase Sender Transformation

When `connect(sender, receiver)` is called:
1. **Completing domain** transforms the sender (recursive until fixpoint)
2. **Starting domain** transforms the sender (recursive until fixpoint)
3. Final sender is connected to receiver → operation state

---

## 13. Build Integration

### CMake (FetchContent or subdirectory):

```cmake
add_subdirectory(third_party/stdexec)
target_link_libraries(my_target PRIVATE STDEXEC::stdexec)
```

### Requirements:
- **C++20** minimum (`-std=c++20`)
- **GCC 12+**: needs `-fcoroutines`
- **Clang 16+**: coroutines built-in
- **MSVC 14.43+**: needs `/Zc:__cplusplus /Zc:preprocessor`
- Links `Threads::Threads` automatically

---

## 14. Common Patterns

### Error recovery with fallback:

```cpp
auto robust = sender
    | stdexec::let_error([](auto) {
        return stdexec::just(default_value);
      });
```

### Timeout (when_any):

```cpp
auto with_timeout = exec::when_any(
    actual_work,
    timer_sender(5s) | stdexec::then([] { throw timeout_error{}; })
);
```

### Sequential async operations:

```cpp
auto pipeline = stdexec::just()
    | stdexec::let_value([] { return async_step1(); })
    | stdexec::let_value([](auto result1) { return async_step2(result1); })
    | stdexec::let_value([](auto result2) { return async_step3(result2); });
```

### Fan-out / fan-in:

```cpp
auto fan_out = stdexec::when_all(
    stdexec::starts_on(sched, task_a),
    stdexec::starts_on(sched, task_b),
    stdexec::starts_on(sched, task_c)
) | stdexec::then([](auto a, auto b, auto c) {
    return merge(a, b, c);
});
```

### `then` vs `let_value`:
- **`then`**: synchronous transformation `(T) → U`
- **`let_value`**: async transformation `(T) → sender_of<U>` (returns a new sender)

---

## 15. Key Differences from std::async / std::future

| Feature | std::async/future | stdexec |
|---------|-------------------|---------|
| Evaluation | Eager | **Lazy** |
| Composition | None (`.then()` not standard) | **Rich algorithm library** |
| Cancellation | None | **Built-in stop tokens** |
| Error model | Exceptions only | **Three-channel (value/error/stopped)** |
| Allocation | Heap per future | **Zero-alloc possible** |
| Customization | None | **Domains, environments, CPOs** |
| Coroutines | No integration | **Native co_await support** |
