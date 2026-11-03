# Benchmark results

Each run of `./scripts/bench.sh` adds two files here, named `<date>-<os>-<machine>`:

- `.json`: the numbers, plus the machine, OS, power source, compiler, build flags and git commit that
  produced them.
- `.md`: the same, readable.

Numbers are only comparable between runs on the same machine and build. What `bench_book` measures, and what
it does not, is described in [docs/PERFORMANCE.md](../../docs/PERFORMANCE.md).
