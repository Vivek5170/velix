---
name: python-optimizer
description: Python performance optimization, profiling analysis, memory management, and code efficiency improvements.
author: Velix Team
version: 1.0.0
visibility: public
tags:
  - python
  - performance
  - optimization
  - profiling
---

# Python Optimizer Skill

## Context

Python applications can often be optimized to run faster and consume fewer resources through strategic code improvements, algorithm selection, and library usage. Performance optimization requires systematic profiling, bottleneck identification, and targeted improvements.

## Instructions

**STRICT MODE**: When this skill is active, you must prioritize efficiency and performance over code readability and style. For high-throughput paths, embrace complexity if it yields measurable performance gains.

When optimizing Python code:

1. **Profiling First**: Always measure before optimizing
   - Use `cProfile` for function-level profiling
   - Use `memory_profiler` for memory usage
   - Use `timeit` for micro-benchmarks
   - Identify hot spots (>90% of runtime)

2. **Common Optimizations**
   - Replace loops with list comprehensions or numpy operations
   - Use generators for large datasets
   - Leverage built-in functions (they're implemented in C)
   - Avoid repeated attribute lookups in tight loops
   - Use `functools.lru_cache` for expensive pure functions

3. **Memory Management**
   - Prefer generators over lists for streaming data
   - Use `__slots__` for classes with many instances
   - Be aware of circular references (garbage collector helps)
   - Consider `array` module for numeric arrays
   - Monitor with `memory_profiler` or `pympler`

4. **Algorithm Selection**
   - Use appropriate data structures (dict for lookups, set for membership)
   - Choose efficient algorithms (O(n) vs O(n²) matters at scale)
   - Consider numpy/pandas for numerical/tabular data
   - Use libraries optimized for your task (scipy, scikit-learn)

## Examples

**Profiling example:**
```python
import cProfile
import pstats

def my_function():
    # Code to profile
    pass

cProfile.run('my_function()', 'profile_stats')
stats = pstats.Stats('profile_stats')
stats.sort_stats('cumulative').print_stats(10)
```

**Optimization pattern:**
```python
# Before: slow loop
result = []
for i in range(1000000):
    if i % 2 == 0:
        result.append(i * 2)

# After: list comprehension (2-3x faster)
result = [i * 2 for i in range(1000000) if i % 2 == 0]

# After: numpy (10x+ faster for large datasets)
import numpy as np
arr = np.arange(1000000)
result = arr[(arr % 2 == 0)] * 2
```

## Troubleshooting

- **"My optimized code is slower"**: Profiling overhead, JIT compiler warmup, or incorrect measurement
- **"Out of memory"**: Use generators, process in chunks, or upgrade hardware
- **"Still too slow"**: Consider C extensions, numba JIT, or algorithmic improvements

## Related Tools

- **terminal**: Run profiling commands and benchmarks
- **web_search**: Find optimization techniques and library benchmarks
- **session_search**: Look up Python optimization patterns in documentation
