## Benchmarking

DDNet is available in the [Phoronix Test Suite](https://openbenchmarking.org/test/pts/ddnet). If you have PTS installed you can easily benchmark DDNet on your own system like this:

```sh
phoronix-test-suite benchmark ddnet
```

## Rendering traces

Use the client console to record frame counters, component CPU zones, and native Vulkan/WebGPU GPU timestamps in memory and save them as JSON. Supported native backends additionally split GPU work into world and interface zones:

```text
render_trace_start 10 trace.json
```

The trace stops after the requested number of seconds. Use `render_trace_stop` to stop it early. Generate an interactive HTML report with:

```sh
python scripts/render_trace_report.py trace.json
```

Tracing is disabled by default. OpenGL traces contain the CPU zones and counters but no native GPU timestamp. WebGPU keeps total frame timing when timestamp queries inside render passes are unavailable, but omits the world/interface split.

The `debug` overlay shows the latest frontend render time, native GPU total and zones, commands, draw calls, triangles, memory, and frame-mailbox totals. These statistics and GPU queries are also disabled when neither `debug`, a rendering trace, nor `benchmark_quit` is active.
