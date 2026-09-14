module;

// The module is a re-export of the same headers, so `import surrealdb;` and
// `#include <surrealdb/surrealdb.hpp>` are interchangeable. This define stops
// the umbrella header from turning round and importing us.
#define SURREALDB_COMPILING_LIBRARY
#include <surrealdb/surrealdb.hpp>

export module surrealdb;

export namespace surrealdb {

// What this build supports.
//
// The `SURREALDB_HAS_*` macros cannot be exported -- macros do not cross a
// module boundary -- so these constants are how a module consumer asks.
using ::surrealdb::cpp_standard;
using ::surrealdb::version;
using ::surrealdb::version_major;
using ::surrealdb::version_minor;
using ::surrealdb::version_patch;
using ::surrealdb::version_string;
using ::surrealdb::has_concepts;
using ::surrealdb::has_cxx17;
using ::surrealdb::has_cxx20;
using ::surrealdb::has_cxx23;
using ::surrealdb::has_ranges;
using ::surrealdb::has_span;
using ::surrealdb::has_std_expected;
using ::surrealdb::has_deleted_reasons;
using ::surrealdb::has_exceptions;
using ::surrealdb::has_format;

// Errors and results
using ::surrealdb::error;
using ::surrealdb::error_code;
using ::surrealdb::is_ok;
using ::surrealdb::ok;
using ::surrealdb::result;
using ::surrealdb::to_string;

// Bounded waits
using ::surrealdb::poll;
using ::surrealdb::poll_state;

// Ownership
using ::surrealdb::owned_array;
using ::surrealdb::owned_arr_results;
using ::surrealdb::owned_byte_array;
using ::surrealdb::owned_connection;
using ::surrealdb::owned_object;
using ::surrealdb::owned_rpc;
using ::surrealdb::owned_rpc_stream;
using ::surrealdb::owned_stream;
using ::surrealdb::owned_string;
using ::surrealdb::owned_strings;
using ::surrealdb::owned_uuids;
using ::surrealdb::owned_value;
using ::surrealdb::owned_values;

// Geometry and record ids
using ::surrealdb::coord;
using ::surrealdb::coord_span;
using ::surrealdb::geometry;
using ::surrealdb::geometry_kind;
using ::surrealdb::geometry_ref;
using ::surrealdb::geometry_span;
using ::surrealdb::id_kind;
using ::surrealdb::linestring_ref;
using ::surrealdb::linestring_span;
using ::surrealdb::point_ref;
using ::surrealdb::point_span;
using ::surrealdb::polygon_ref;
using ::surrealdb::polygon_span;
using ::surrealdb::thing;
using ::surrealdb::thing_ref;

// Values
using ::surrealdb::array_view;
using ::surrealdb::bytes_ref;
using ::surrealdb::datetime_ref;
using ::surrealdb::decimal_ref;
using ::surrealdb::duration_ref;
using ::surrealdb::file_ref;
using ::surrealdb::none_t;
using ::surrealdb::null_t;
using ::surrealdb::object_view;
using ::surrealdb::regex_ref;
using ::surrealdb::table_ref;
using ::surrealdb::uuid_ref;
using ::surrealdb::value;
using ::surrealdb::value_kind;

// Builders
using ::surrealdb::array_builder;
using ::surrealdb::array_from;
using ::surrealdb::keys_of;
using ::surrealdb::object_builder;
using ::surrealdb::object_keys;
using ::surrealdb::view;

// Comparison and debugging
using ::surrealdb::debug_print;
using ::surrealdb::operator==;
using ::surrealdb::operator!=;

// Live queries
using ::surrealdb::action;
using ::surrealdb::notification;
using ::surrealdb::stream;

// Query results
using ::surrealdb::query_results;
using ::surrealdb::statement_result;

// RPC and sessions
using ::surrealdb::rpc;
using ::surrealdb::rpc_stream;
using ::surrealdb::session_id;
using ::surrealdb::session_list;

// Connection
using ::surrealdb::access;
using ::surrealdb::capabilities;
using ::surrealdb::connection;
using ::surrealdb::credentials;
using ::surrealdb::options;
using ::surrealdb::scope;
using ::surrealdb::target_mode;
using ::surrealdb::target_set;
using ::surrealdb::toggle;
using ::surrealdb::transaction;

}

// Value construction. Exported as a whole namespace rather than name by name:
// it is a flat set of free functions that will grow, and a missing `using`
// here is invisible until someone tries to `import surrealdb;` and cannot
// build a polygon.
export namespace surrealdb::make {
using ::surrealdb::make::array;
using ::surrealdb::make::boolean;
using ::surrealdb::make::collection;
using ::surrealdb::make::bytes;
using ::surrealdb::make::coord;
using ::surrealdb::make::datetime;
using ::surrealdb::make::decimal;
using ::surrealdb::make::duration;
using ::surrealdb::make::file;
using ::surrealdb::make::floating;
using ::surrealdb::make::integer;
using ::surrealdb::make::linestring;
using ::surrealdb::make::multilinestring;
using ::surrealdb::make::multipoint;
using ::surrealdb::make::multipolygon;
using ::surrealdb::make::none;
using ::surrealdb::make::null;
using ::surrealdb::make::object;
using ::surrealdb::make::point;
using ::surrealdb::make::polygon;
using ::surrealdb::make::regex;
using ::surrealdb::make::set;
using ::surrealdb::make::string;
using ::surrealdb::make::table;
using ::surrealdb::make::thing;
using ::surrealdb::make::uuid;

// Ranges
using ::surrealdb::make::bound;
using ::surrealdb::make::excluded;
using ::surrealdb::make::included;
using ::surrealdb::make::range;
using ::surrealdb::make::unbounded;
}

export namespace surrealdb::dsl {
using ::surrealdb::dsl::arena;
using ::surrealdb::dsl::built_query;
using ::surrealdb::dsl::comparison;
using ::surrealdb::dsl::f;
using ::surrealdb::dsl::field_ref;
using ::surrealdb::dsl::group_guard;
using ::surrealdb::dsl::junction;
using ::surrealdb::dsl::query;
using ::surrealdb::dsl::raw;
using ::surrealdb::dsl::raw_expr;
using ::surrealdb::dsl::select;
using ::surrealdb::dsl::select_guard;
using ::surrealdb::dsl::select_query;
using ::surrealdb::dsl::select_writer;
using ::surrealdb::dsl::where_guard;

// The expression-template operators.
//
// Easy to forget, and forgetting them is not a subtle failure: `f("age") > 30`
// simply stops compiling for module consumers while it keeps working for
// header ones. The modules example exercises exactly that line so the build
// catches it.
using ::surrealdb::dsl::operator==;
using ::surrealdb::dsl::operator!=;
using ::surrealdb::dsl::operator<;
using ::surrealdb::dsl::operator<=;
using ::surrealdb::dsl::operator>;
using ::surrealdb::dsl::operator>=;
using ::surrealdb::dsl::operator&&;
using ::surrealdb::dsl::operator||;
}
