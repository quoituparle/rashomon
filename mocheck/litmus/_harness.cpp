// Tool-owned. Each litmus test is a complete program (threads, assertions, main);
// tools/litmus.py stages it as mocheck_impl.hpp exactly like an implementation under
// test, so it goes through the same rewrites and compiler flags as a real verify.
#include "mocheck_impl.hpp"
