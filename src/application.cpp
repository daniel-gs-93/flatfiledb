#include "flatfiledb/application.h"

#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

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

std::vector<domain::TableSchema> QueryService::DescribeCatalog() const {
  const domain::ICatalog& catalog = engine_->Catalog();
  std::vector<domain::TableSchema> schemas;
  for (const std::string& name : catalog.TableNames()) {
    std::optional<domain::TableSchema> schema = catalog.FindTable(name);
    if (schema) {
      schemas.push_back(std::move(*schema));
    }
  }
  return schemas;
}

}  // namespace flatfiledb::application
