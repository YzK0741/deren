# vessel: project containers and allocation policy

Import `deren.utility` or `deren.utility.shared`, then use
`deren::utility::vessel::vector<T>`, `string`, `map`, and related aliases.
They are standard containers using `memory::allocator<T>`, with ordinary STL
semantics and no inheritance wrapper.

```cpp
import deren.utility;
namespace vessel = deren::utility::vessel;
vessel::vector<int> values{1, 2, 3};
vessel::unordered_map<vessel::string, int> counts;
auto label = vessel::format("items={}", values.size());
```

Available aliases cover array, vector/deque/list, basic_string and five character
string types, map/multimap, set/multiset, the four unordered containers, and
queue/stack/priority_queue. Adapters default to vessel storage; callers
supplying another underlying container choose its allocation policy.
Allocator-backed istringstream/ostringstream/stringstream also own vessel strings.
`to_string` supports integers. Nested owning strings and containers should
also use vessel: an outer allocator cannot replace an element's allocator.

## Allocation failure

`memory::try_allocate(bytes, alignment)` is noexcept and returns
`expected<void*, allocation_error>`. Errors are invalid_alignment,
size_overflow, and out_of_memory. Alignment must be a nonzero power of two;
the byte limit is PTRDIFF_MAX. Successful zero-byte requests still return
non-null storage. Release it through `memory::deallocate`, which accepts
null pointers and supports release on another thread.

`memory::allocate` and the STL adapter terminate on failure. They never
return null to STL. A fixed stack buffer writes the reason, byte count,
and alignment directly to OS stderr, followed by abort. No log queue,
GPU wait, recovery callback, or retry loop participates. Recursive failure
reporting on the same thread exits immediately. An unavailable stderr
does not prevent termination, but diagnostics cannot then be guaranteed.

For recoverable OOM, use try_allocate and handle its expected result.
Standard containers do not provide allocation failure as a return value,
and vessel does not invent that contract. Element construction, user
hash/comparator callbacks, bounds checks, and length checks retain STL
semantics. Container methods are not universally noexcept.

## DLL and migration boundaries

Only shared_utility implements the mimalloc allocation/free entry points.
Templates do not include mimalloc headers. Exported vessel interfaces need
a PUBLIC shared_utility link dependency and rebuilt consumers. No PMR
default-resource registration or startup hook is needed. Global operator
new is unchanged.

Owned containers in engine, utility, application, configuration, loaders,
backends, RHI/GUI contracts, and tests use vessel. Borrowed APIs retain
span/string_view. Third-party libraries and vstd standard-library exports
keep their existing types. RHI ABI53 and GUI ABI5 require rebuilt matching
plugins; factories reject mismatched versions before returning typed objects.
Vectors with different allocators use range copies; string boundaries use
views or explicitly construct the target type.
STL filesystem/stream/format internals, function, shared_ptr control blocks,
ordinary new, and third-party hidden allocations are outside this policy.

Test builds include fault injection for raw allocation, real container
growth, and concurrent fatal diagnostics. VR_BUILD_TESTS=OFF removes these
hooks at compile time.
