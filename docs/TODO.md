# Status

The current version is 0.2.1.0, and `qmcorecmd` of qmsetup uses the library. Before major version 1, any release may change the headers incompatibly. From major version 1 onward, the headers remain source compatible within a major version. The soname includes the minor version. A minor version may therefore change the symbols that the binary exports.

## Known gaps

- `vlarray::get_allocator()` returns a `const` reference to the stored allocator. A standard container returns its allocator by value, and no property of an allocator requires returning a reference.
- `support/commandline.h` contains more than a thousand lines of inline code, which every translation unit that includes the header compiles.
- `DynamicRegistry::remove_listener()` waits for every notification in progress, not only for the notifications that reach the listener being removed. This behavior is safe but conservative. If other threads register continuously, the count may never be observed at zero, and no timeout exists. A caller can therefore wait far longer than the callbacks that it must outlive. A per-listener count or a generation number would bound the wait.
- In a Debug build with MSVC, the leak check of the debug runtime, which Boost.Test enables by default, reports three blocks for each `DynamicRegistry` type that `test_dynamicregistry` uses: the registry object, the container proxy of its `std::map` and the head node of the map. `shared_instance()` never destroys a registry by design. These blocks are therefore expected and do not indicate a leak.
- `processMemoryUsage()` in `src/system.cpp` has no caller. It is `[[maybe_unused]] static`, is declared in no header, and requires `<Psapi.h>` and `<mach/mach.h>`. It should be either deleted or promoted to `system::` with documentation and a test, because no test can cover it in its current form.

## Wanted

- Mutually exclusive option groups for `cli`, so that `--json` and `--xml` can exclude each other. The SysCmdLine implementation of this feature interacts with its option priority levels. The semantics should therefore be designed rather than copied.
- On **Windows**, `communicate()` starts one worker thread per open pipe. With a single pipe, no interleaving is required, and the thread exists only to make a timeout interruptible. CPython omits the thread in that case (`Lib/subprocess.py:1199`, at most one pipe and no timeout). The POSIX implementation requires no change, because it is one `poll()` loop without threads, as in CPython. The change is probably not worthwhile. A thread costs tens of microseconds compared with the milliseconds of `CreateProcessW`, and the change adds a second code path to the function of this library with the most subtle deadlock reasoning. A measurement must precede any implementation.

## Unverified

- On Windows, `console::width()` is tested against the console to which the test suite is attached, and the test is skipped if no console exists. The test does not verify that the function reads the visible window rather than the scrollback buffer. Windows Terminal gives the buffer the same width as the window, and a measurement showed that an implementation reading `dwSize.X` also passes. Such an implementation fails only on a console whose buffer a user has widened, which is the case that the current code handles.
