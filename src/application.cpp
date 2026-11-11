#include "flatfiledb/application.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "flatfiledb/domain/binder.h"

namespace flatfiledb::application {

QueryService::QueryService(std::unique_ptr<domain::IEngine> engine,
                           std::unique_ptr<domain::ICsvSource> csv_source)
    : engine_(std::move(engine)), csv_source_(std::move(csv_source)) {}

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

domain::ExecutionResult QueryService::ImportCsv(
    const domain::ImportRequest& request) {
  if (!csv_source_) {
    return domain::ExecutionError{domain::ErrorKind::kUnsupported,
                                  "import is not available"};
  }
  auto read = csv_source_->Read(request.file_path);
  if (auto* reason = std::get_if<std::string>(&read)) {
    return domain::ExecutionError{domain::ErrorKind::kIo, std::move(*reason)};
  }
  auto inferred =
      domain::InferImport(request, std::get<std::vector<domain::CsvLine>>(read),
                          engine_->Catalog());
  if (auto* error = std::get_if<domain::ExecutionError>(&inferred)) {
    return std::move(*error);
  }
  domain::ImportPlan& plan = std::get<domain::ImportPlan>(inferred);

  const std::string table = plan.schema.name;
  const std::size_t row_count = plan.rows.size();
  domain::PlanNode node;
  node.node =
      domain::ImportTableNode{std::move(plan.schema), std::move(plan.rows)};
  domain::ExecutionResult result = engine_->Execute(node);
  if (!std::holds_alternative<domain::CommandComplete>(result)) {
    return result;
  }

  std::string tag =
      "IMPORT " + table + " (imported " + std::to_string(row_count) + " rows";
  if (plan.skipped_count > 0) {
    tag += ", skipped " + std::to_string(plan.skipped_count);
  }
  tag += ")";
  return domain::CommandComplete{std::move(tag), std::move(plan.notes)};
}

}  // namespace flatfiledb::application
