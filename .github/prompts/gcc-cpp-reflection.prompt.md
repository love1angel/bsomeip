# C++26 Reflection (P2996) — GCC 16 Implementation Reference

> Use this skill when writing C++26 reflection code using GCC's `-freflection` flag. Covers the `^^` operator, splicing `[: :]`, `std::meta::info`, 170+ metafunctions, `define_aggregate`, annotations, and practical patterns.

---

## 1. Enabling Reflection

```bash
g++ -std=c++26 -freflection example.cpp
```

**Requirements:** GCC 16+ (trunk). The `-freflection` flag only works with `-std=c++26` or `-std=gnu++26`.

**Primary header:**
```cpp
#include <meta>
```

---

## 2. The Reflection Operator: `^^`

`^^` creates a compile-time value of type `std::meta::info` representing the reflected entity.

```cpp
constexpr auto r1 = ^^int;            // Reflect a type
constexpr auto r2 = ^^std::vector;    // Reflect a template
constexpr auto r3 = ^^my_func;       // Reflect a function
constexpr auto r4 = ^^S::member;     // Reflect a class member
constexpr auto r5 = ^^my_var;        // Reflect a variable
constexpr auto r6 = ^^MyNS;          // Reflect a namespace
constexpr auto r7 = ^^MyConcept;     // Reflect a concept
constexpr auto r8 = ^^::;            // Reflect global namespace
```

**Parsing rules:**
- `^^int()` reflects the type `int()` (function type), not a call
- `^^X < xv` is ambiguous if `X` is a template — use `(^^X) < xv`
- `^^int && true` — use `(^^int) && true`

**Cannot reflect:** pack indices, lambda captures, local parameters in requires-expressions, using-declarations.

---

## 3. Splice Expressions: `[: :]`

Splicing converts a `std::meta::info` back into program entities:

```cpp
// Type splice
constexpr auto t = ^^int;
typename [:t:] x = 42;                // x is int

// Expression splice
constexpr auto v = ^^some_var;
[:v:] = 10;                           // Access some_var

// Namespace splice (in template context)
template <auto R> int fn() {
    namespace Alias = [:R:];
    return Alias::value;
}

// Template splice
constexpr auto tmpl = ^^std::vector;
template [:tmpl:]<int> vec;            // std::vector<int>

// Declarator splice
constexpr int c = [:^^S:]::a;         // Access static member of S

// Template class splice
constexpr int d = template [:^^TCls:]<int>::b;
```

---

## 4. `std::meta::info` Type

```cpp
using info = decltype(^^int);   // std::meta::info

constexpr info null_reflection; // Default: null reflection
```

- Pointer-sized scalar type
- Unique per entity: `^^int == ^^int` (always true)
- Only `==` and `!=` — no ordering (`<`, `>` not supported)
- Constexpr only — all operations at compile time

---

## 5. Metafunction API

### 5.1 Identity & Naming

```cpp
using namespace std::meta;

has_identifier(info)               // → bool
identifier_of(info)                // → string_view (source name)
u8identifier_of(info)              // → u8string_view
display_string_of(info)            // → string_view (human-readable)
u8display_string_of(info)          // → u8string_view
symbol_of(info)                    // → string_view (mangled)
u8symbol_of(info)                  // → u8string_view
source_location_of(info)           // → std::source_location
operator_of(info)                  // → operators enum
```

### 5.2 Type & Entity Queries

```cpp
type_of(info)                      // → info (type of the entity)
parent_of(info)                    // → info (enclosing namespace/class)
dealias(info)                      // → info (strip aliases)
object_of(info)                    // → info (object for static members)
constant_of(info)                  // → info (constant value)
variable_of(info)                  // → info (variable for parameters)
return_type_of(info)               // → info (function return type)
template_of(info)                  // → info (template of specialization)
template_arguments_of(info)        // → vector<info>
has_template_arguments(info)       // → bool
has_parent(info)                   // → bool
has_default_argument(info)         // → bool
has_default_member_initializer(info) // → bool
```

### 5.3 Entity Classification (bool)

```cpp
// What is it?
is_type(info)                 is_variable(info)
is_function(info)             is_namespace(info)
is_concept(info)              is_value(info)
is_object(info)               is_type_alias(info)
is_namespace_alias(info)      is_base(info)
is_structured_binding(info)   is_annotation(info)

// Member kind
is_class_member(info)         is_namespace_member(info)
is_nonstatic_data_member(info) is_static_member(info)
is_bit_field(info)            is_enumerator(info)

// Function kind
is_constructor(info)          is_destructor(info)
is_default_constructor(info)  is_copy_constructor(info)
is_move_constructor(info)     is_copy_assignment(info)
is_move_assignment(info)      is_special_member_function(info)
is_conversion_function(info)  is_operator_function(info)
is_literal_operator(info)     is_vararg_function(info)
is_function_parameter(info)   is_explicit_object_parameter(info)

// Template kind
is_template(info)             is_function_template(info)
is_class_template(info)       is_variable_template(info)
is_alias_template(info)       is_constructor_template(info)
is_conversion_function_template(info)
is_operator_function_template(info)
is_literal_operator_template(info)
```

### 5.4 Access & Qualifiers (bool)

```cpp
is_public(info)               is_protected(info)
is_private(info)              is_accessible(info, access_context)
is_virtual(info)              is_pure_virtual(info)
is_override(info)             is_final(info)
is_deleted(info)              is_defaulted(info)
is_user_provided(info)        is_user_declared(info)
is_explicit(info)             is_noexcept(info)
is_const(info)                is_volatile(info)
is_mutable_member(info)
is_lvalue_reference_qualified(info)
is_rvalue_reference_qualified(info)

// Storage & linkage
has_static_storage_duration(info)
has_thread_storage_duration(info)
has_automatic_storage_duration(info)
has_internal_linkage(info)
has_module_linkage(info)
has_external_linkage(info)
has_c_language_linkage(info)
has_linkage(info)
```

### 5.5 Type Classification (bool, operate on type reflections)

```cpp
// Primary type categories
is_void_type(info)            is_null_pointer_type(info)
is_integral_type(info)        is_floating_point_type(info)
is_array_type(info)           is_pointer_type(info)
is_lvalue_reference_type(info) is_rvalue_reference_type(info)
is_member_object_pointer_type(info)
is_member_function_pointer_type(info)
is_enum_type(info)            is_union_type(info)
is_class_type(info)           is_function_type(info)
is_reflection_type(info)      is_reference_type(info)

// Composite type categories
is_arithmetic_type(info)      is_fundamental_type(info)
is_object_type(info)          is_scalar_type(info)
is_compound_type(info)        is_member_pointer_type(info)

// Type properties
is_const_type(info)           is_volatile_type(info)
is_trivially_copyable_type(info) is_standard_layout_type(info)
is_empty_type(info)           is_polymorphic_type(info)
is_abstract_type(info)        is_final_type(info)
is_aggregate_type(info)       is_structural_type(info)
is_signed_type(info)          is_unsigned_type(info)
is_bounded_array_type(info)   is_unbounded_array_type(info)
is_scoped_enum_type(info)     is_complete_type(info)
is_enumerable_type(info)      is_implicit_lifetime_type(info)

// Constructibility/assignability/destructibility
is_constructible_type(info, ...)
is_default_constructible_type(info)
is_copy_constructible_type(info)
is_move_constructible_type(info)
is_assignable_type(info, ...)
is_copy_assignable_type(info)
is_move_assignable_type(info)
is_destructible_type(info)
// + trivially_* and nothrow_* variants for all above

// Relationships
is_same_type(info, info)
is_base_of_type(info, info)
is_virtual_base_of_type(info, info)
is_convertible_type(info, info)
is_nothrow_convertible_type(info, info)
is_layout_compatible_type(info, info)
is_pointer_interconvertible_base_of_type(info, info)
is_invocable_type(info, ...)
is_invocable_r_type(info, info, ...)
// + nothrow_* variants

has_virtual_destructor(info)
has_unique_object_representations(info)
reference_constructs_from_temporary(info, info)
reference_converts_from_temporary(info, info)
```

### 5.6 Type Transformations (return info)

```cpp
remove_const(info)            add_const(info)
remove_volatile(info)         add_volatile(info)
remove_cv(info)               add_cv(info)
remove_reference(info)        add_lvalue_reference(info)
add_rvalue_reference(info)    remove_cvref(info)
remove_pointer(info)          add_pointer(info)
remove_extent(info)           remove_all_extents(info)
make_signed(info)             make_unsigned(info)
decay(info)                   underlying_type(info)
common_type(info...)          common_reference(info...)
invoke_result(info, info...)
unwrap_reference(info)        unwrap_ref_decay(info)
```

### 5.7 Collection Queries (return vector<info>)

```cpp
members_of(info)                           // All members
members_of(info, access_context)           // Filtered by access
bases_of(info)                             // Base classes
bases_of(info, access_context)
static_data_members_of(info)
static_data_members_of(info, access_context)
nonstatic_data_members_of(info)
nonstatic_data_members_of(info, access_context)
subobjects_of(info)                        // Bases + nonstatic data members
subobjects_of(info, access_context)
enumerators_of(info)                       // Enum values
parameters_of(info)                        // Function parameters
template_arguments_of(info)                // Template args

// Access checking
has_inaccessible_nonstatic_data_members(info, access_context)
has_inaccessible_bases(info, access_context)
has_inaccessible_subobjects(info, access_context)
```

### 5.8 Size, Alignment & Offset

```cpp
size_of(info)           // → size_t
alignment_of(info)      // → size_t
bit_size_of(info)       // → size_t
offset_of(info)         // → member_offset (bit offset)
rank(info)              // → size_t (array dimensions)
extent(info, unsigned)  // → size_t (array extent at dimension)
tuple_size(info)        // → size_t
tuple_element(size_t, info) // → info
variant_size(info)      // → size_t
variant_alternative(size_t, info) // → info
```

### 5.9 Ordering

```cpp
type_order(info, info)  // → std::strong_ordering
```

---

## 6. Extract & Reflect

```cpp
// Extract compile-time value from reflection
extract<int>(^^my_constexpr_var)          // → int value
extract<bool>(reflect_constant(true))     // → true

// Create reflection from value
reflect_constant(42)                      // → info for constant 42
reflect_constant(nullptr)                 // → info for nullptr
reflect_object(^^my_var)                  // → info for object
reflect_function(^^my_func)              // → info for function
reflect_constant_string("hello")          // → info for string
reflect_constant_array(arr)               // → info for array
```

---

## 7. Template Substitution

```cpp
// Check if substitution is valid
can_substitute(^^std::vector, {^^int})    // → true

// Perform substitution
substitute(^^std::vector, {^^int})        // → ^^std::vector<int>

// With reflect_constant for non-type params
substitute(^^std::array, {^^int, reflect_constant(5)})  // → ^^std::array<int, 5>
```

---

## 8. Code Generation: `define_aggregate`

Generate struct/union definitions at compile time:

```cpp
struct S;  // Forward declaration

consteval {
    define_aggregate(^^S, {
        data_member_spec(^^int, {.name = "x"}),
        data_member_spec(^^double, {.name = "y", .alignment = 16}),
        data_member_spec(^^bool, {.name = "flag", .no_unique_address = true}),
        data_member_spec(^^unsigned, {.bit_width = 3}),  // Anonymous bitfield
        data_member_spec(^^int, {.name = "bits", .bit_width = 7}),
    });
}

// Now S is complete: S s = {.x = 1, .y = 2.0, .flag = true, .bits = 42};
```

**`data_member_spec` options:**
```cpp
data_member_spec(type_info, {
    .name = "member_name",         // string or u8string (optional = anonymous)
    .alignment = alignof(T) * 2,   // Optional custom alignment
    .bit_width = 5,                // Optional bitfield width (0 = zero-width)
    .no_unique_address = true,     // Optional [[no_unique_address]]
});
```

**Works with:**
- Forward-declared structs and unions
- Template specializations (`^^S<42>`)
- Aliases (use `dealias()` first)

---

## 9. Annotations (P3394)

Attach compile-time values to declarations:

```cpp
struct [[=1]] MyClass {          // Annotate class with int 1
    [[=42]] int x;               // Annotate member
    [[="tag"]] void method();    // Annotate with string
};

[[=3.14]] double pi = 3.14159;   // Annotate variable
[[=MyAnnotation{.a=1}]] int y;   // Annotate with struct

// Query annotations
consteval {
    auto anns = annotations_of(^^MyClass);       // vector<info>
    auto typed = annotations_of_with_type(^^MyClass, ^^int);  // Filter by type
}
```

**Valid targets:** classes, enums, variables, functions, parameters, data members, structured bindings (via `[[=expr]]`).

---

## 10. Context Queries

```cpp
current_function()      // → info of current function
current_class()         // → info of current class
current_namespace()     // → info of current namespace

// Access context for member visibility
auto ctx = access_context::current();
members_of(^^MyClass, ctx);  // Only accessible members
```

---

## 11. Practical Patterns

### 11.1 Enum-to-String

```cpp
template <typename E>
consteval std::string_view enum_to_string(E value) {
    for (auto e : std::meta::enumerators_of(^^E))
        if ([:e:] == value)
            return std::meta::identifier_of(e);
    return "<unknown>";
}

enum Color { Red, Green, Blue };
static_assert(enum_to_string(Color::Red) == "Red");
```

### 11.2 Iterate Struct Members

```cpp
template <typename T>
consteval void print_fields() {
    for (auto m : std::meta::nonstatic_data_members_of(^^T)) {
        // identifier_of(m) → field name
        // type_of(m) → field type
        // offset_of(m) → bit offset
        // is_public(m), is_private(m) etc.
    }
}
```

### 11.3 Generic Serialization

```cpp
template <typename T>
void to_json(const T& obj, auto& out) {
    out << "{";
    bool first = true;
    [:expand(std::meta::nonstatic_data_members_of(^^T)):] >> [&]<auto M> {
        if (!first) out << ",";
        first = false;
        out << "\"" << std::meta::identifier_of(M) << "\":";
        to_json(obj.[:M:], out);
    };
    out << "}";
}
```

### 11.4 Struct-of-Arrays Transform

```cpp
template <typename T>
struct SoA;

template <typename T>
struct SoA {
    consteval {
        for (auto m : std::meta::nonstatic_data_members_of(^^T)) {
            auto arr_type = substitute(^^std::vector, {type_of(m)});
            // Generate: std::vector<FieldType> field_name;
        }
    }
};
```

### 11.5 Filter Members with Ranges

```cpp
#include <meta>
#include <ranges>

consteval auto public_data_members(std::meta::info cls) {
    return std::meta::nonstatic_data_members_of(cls)
         | std::views::filter(std::meta::is_public)
         | std::ranges::to<std::vector>();
}
```

---

## 12. `consteval { }` Blocks

Top-level and class-level `consteval` blocks run at compile time:

```cpp
// Top-level
consteval {
    auto members = members_of(^^MyType);
    // ... compile-time logic
}

// In class
struct S {
    int x;
    consteval {
        // Can access current_class() → ^^S
        // Can call define_aggregate for incomplete types
    }
};
```

---

## 13. Error Handling

Metafunctions throw `std::meta::exception` on invalid input:

```cpp
consteval bool safe_check(std::meta::info r) {
    try {
        substitute(r, {});
    } catch (std::meta::exception&) {
        return false;
    }
    return true;
}
```

---

## 14. Bazel Build Integration

```python
cc_binary(
    name = "my_reflect_app",
    srcs = ["main.cpp"],
    copts = ["-std=c++26", "-freflection"],
    # Requires GCC 16+ toolchain
)
```

---

## 15. Key Limitations (GCC 16 trunk)

- **Constexpr only** — all reflection operations must be at compile time
- **No runtime reflection** — `std::meta::info` has no runtime representation
- **No `^^` on:** pack indices, non-type template params, lambda captures
- **Anonymous unions** handled specially in `members_of`
- **`define_aggregate`** requires forward-declared (incomplete) types
- **Ordering:** `<`, `>`, `<=`, `>=` not available for `info`; use `type_order()` for types
- **Header:** `#include <meta>` (not `<experimental/meta>`)

---

## 16. Complete Metafunction Return Types

| Returns | Functions |
|---------|-----------|
| `bool` | All `is_*`, `has_*`, `can_substitute` |
| `info` | `type_of`, `parent_of`, `dealias`, `template_of`, `substitute`, all type transforms |
| `vector<info>` | `members_of`, `bases_of`, `*_data_members_of`, `enumerators_of`, `parameters_of`, `template_arguments_of`, `annotations_of` |
| `string_view` | `identifier_of`, `display_string_of`, `symbol_of` |
| `u8string_view` | `u8identifier_of`, `u8display_string_of`, `u8symbol_of` |
| `size_t` | `size_of`, `alignment_of`, `bit_size_of`, `rank`, `extent`, `tuple_size`, `variant_size` |
| `member_offset` | `offset_of` |
| `source_location` | `source_location_of` |
| `strong_ordering` | `type_order` |
| `access_context` | `access_context::current()` |
