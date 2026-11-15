#ifndef STRATA_DOMAIN_BINDER_H_
#define STRATA_DOMAIN_BINDER_H_

#include <variant>

#include "strata/domain/ast.h"
#include "strata/domain/engine.h"
#include "strata/domain/plan.h"
#include "strata/domain/value.h"

namespace strata::domain {

using BindResult = std::variant<PlanPtr, ExecutionError>;

// Resolves names and types in statement against catalog and builds the plan.
// Pure: reads the catalog, touches nothing else. Errors follow the rules in
// docs/DesignDocument.md section 2.4: kBind for name and type errors,
// kUnsupported for an UPDATE of a primary-key column, kRuntime for a constant
// INSERT value or DEFAULT whose evaluation overflows. DDL statements bind to
// CreateTableNode, AddColumnNode and DropTableNode; their checks are in
// section 2.4 as well.
BindResult Bind(const Statement& statement, const ICatalog& catalog);

}  // namespace strata::domain

#endif  // STRATA_DOMAIN_BINDER_H_
