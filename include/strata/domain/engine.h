#ifndef STRATA_DOMAIN_ENGINE_H_
#define STRATA_DOMAIN_ENGINE_H_

#include <optional>
#include <string>
#include <vector>

#include "strata/domain/plan.h"
#include "strata/domain/value.h"

namespace strata::domain {

// Read access to table schemas. Implemented by each engine.
class ICatalog {
 public:
  virtual ~ICatalog() = default;

  // The name is matched case-insensitively. Returns a copy of the schema.
  virtual std::optional<TableSchema> FindTable(
      const std::string& name) const = 0;

  // Table names in registration order.
  virtual std::vector<std::string> TableNames() const = 0;
};

// Port owned by the domain: executes a bound plan and persists its effects.
class IEngine {
 public:
  virtual ~IEngine() = default;

  // Valid for the lifetime of the engine.
  virtual const ICatalog& Catalog() const = 0;

  // Borrows plan for the duration of the call. A plan the engine cannot run
  // yields ExecutionError{kUnsupported}; INSERT, UPDATE and DELETE yield
  // AffectedRows, and DDL and import yield CommandComplete.
  virtual ExecutionResult Execute(const PlanNode& plan) = 0;
};

}  // namespace strata::domain

#endif  // STRATA_DOMAIN_ENGINE_H_
