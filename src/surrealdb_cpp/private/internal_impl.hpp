#ifndef SURREALDB_INTERNAL_IMPL_HPP
#define SURREALDB_INTERNAL_IMPL_HPP

#include <surrealdb.h>

namespace surrealdb::internal {
    inline const char* get_version_string(sr_string_t ver) {
        return ver ? ver : "unknown";
    }
}

#endif // SURREALDB_INTERNAL_IMPL_HPP
