#include "flatfiledb/application.h"

#include <utility>

#include "flatfiledb/domain/binder.h"

namespace flatfiledb::application {

QueryService::QueryService(std::unique_ptr<domain::IEngine> engine)
    : engine_(std::move(engine)) {}

domain::ExecutionResult QueryService::Execute(
    const domain::Statement& statement) {
  domain::BindResult bound = domain::Bind(statement, engine_->Catalog());
  if (auto* error = std::get_if<domain::ExecutionError>(&bound)) {
    return std::move(*error);
  }
  return engine_->Execute(*std::get<domain::PlanPtr>(bound));
}

}  // namespace flatfiledb::application
