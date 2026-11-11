#ifndef FLATFILEDB_APPLICATION_H_
#define FLATFILEDB_APPLICATION_H_

#include <memory>

#include "flatfiledb/domain/ast.h"
#include "flatfiledb/domain/engine.h"
#include "flatfiledb/domain/value.h"

namespace flatfiledb::application {

// Executes one statement: binds it against the engine's catalog, then hands
// the plan to the engine. Owns the engine. Imports only the domain.
class QueryService {
 public:
  explicit QueryService(std::unique_ptr<domain::IEngine> engine);

  // Borrows statement for the duration of the call.
  domain::ExecutionResult Execute(const domain::Statement& statement);

 private:
  std::unique_ptr<domain::IEngine> engine_;
};

}  // namespace flatfiledb::application

#endif  // FLATFILEDB_APPLICATION_H_
