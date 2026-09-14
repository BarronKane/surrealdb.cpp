#pragma once

/// @file surrealdb.hpp
/// Umbrella header.
///
/// With `SURREALDB_USE_MODULES` defined and not compiling the library itself,
/// this defers to `import surrealdb;`. The module is a re-export of exactly
/// these headers, so the two spellings are interchangeable.

#include "config.hpp"

#if defined(SURREALDB_USE_MODULES) && !defined(SURREALDB_COMPILING_LIBRARY)
import surrealdb;
#else
#  include "error.hpp"
#  include "detail/owned.hpp"
#  include "detail/invoke.hpp"
#  include "value.hpp"
#  include "array.hpp"
#  include "object.hpp"
#  include "format.hpp"
#  include "geometry.hpp"
#  include "make.hpp"
#  include "options.hpp"
#  include "poll.hpp"
#  include "rpc.hpp"
#  include "results.hpp"
#  include "stream.hpp"
#  include "connection.hpp"
#  include "dsl/query.hpp"
#endif
