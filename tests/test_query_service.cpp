#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "test_support.h"

namespace {

namespace domain = strata::domain;
namespace testing = strata::testing;

struct FakeEngineLog {
  int execute_calls = 0;
  bool is_root_project = false;
  bool is_create_table = false;
};

// An engine that records what it was asked to run and returns a canned result.
class FakeEngine : public domain::IEngine {
 public:
  FakeEngine(std::shared_ptr<FakeEngineLog> log,
             domain::ExecutionResult canned_result)
      : log_(std::move(log)), canned_result_(std::move(canned_result)) {}

  const domain::ICatalog& Catalog() const override { return catalog_; }

  domain::ExecutionResult Execute(const domain::PlanNode& plan) override {
    ++log_->execute_calls;
    log_->is_root_project =
        std::holds_alternative<domain::ProjectNode>(plan.node);
    log_->is_create_table =
        std::holds_alternative<domain::CreateTableNode>(plan.node);
    return canned_result_;
  }

 private:
  std::shared_ptr<FakeEngineLog> log_;
  domain::ExecutionResult canned_result_;
  testing::FakeCatalog catalog_;
};

domain::ResultSet OneRowResult() {
  domain::ResultSet result_set;
  result_set.columns.push_back(domain::ColumnDef{"id", domain::DataType::kInt});
  result_set.rows.push_back(domain::Row{domain::Value(int64_t{1})});
  return result_set;
}

}  // namespace

TEST(test_query_service_binds_then_executes_on_engine) {
  auto log = std::make_shared<FakeEngineLog>();
  strata::application::QueryService service(std::make_unique<FakeEngine>(
      log, domain::ExecutionResult(OneRowResult())));

  domain::Statement statement = testing::MustParse("SELECT id FROM users");
  domain::ExecutionResult result = service.Execute(statement);

  CHECK_EQ(log->execute_calls, 1);
  CHECK(log->is_root_project);
  const domain::ResultSet* result_set = testing::AsResultSet(result);
  CHECK(result_set != nullptr);
  if (result_set == nullptr) {
    return;
  }
  CHECK_EQ(result_set->columns.size(), std::size_t{1});
  std::vector<std::string> row_texts = testing::RowsAsText(*result_set);
  CHECK_EQ(row_texts.size(), std::size_t{1});
  if (row_texts.empty()) {
    return;
  }
  CHECK_EQ(row_texts[0], std::string("1"));
}

TEST(test_query_service_bind_error_does_not_reach_engine) {
  auto log = std::make_shared<FakeEngineLog>();
  strata::application::QueryService service(std::make_unique<FakeEngine>(
      log, domain::ExecutionResult(OneRowResult())));

  domain::Statement statement = testing::MustParse("SELECT nosuch FROM users");
  domain::ExecutionResult result = service.Execute(statement);

  const domain::ExecutionError* error = testing::AsError(result);
  CHECK(error != nullptr);
  if (error == nullptr) {
    return;
  }
  CHECK_EQ(static_cast<int>(error->kind),
           static_cast<int>(domain::ErrorKind::kBind));
  CHECK_EQ(log->execute_calls, 0);
}

TEST(test_query_service_propagates_engine_errors) {
  const domain::ErrorKind kinds[] = {
      domain::ErrorKind::kBind, domain::ErrorKind::kUnsupported,
      domain::ErrorKind::kConstraintViolation, domain::ErrorKind::kRuntime,
      domain::ErrorKind::kIo};
  for (domain::ErrorKind kind : kinds) {
    auto log = std::make_shared<FakeEngineLog>();
    domain::ExecutionError canned{kind, "canned message"};
    strata::application::QueryService service(
        std::make_unique<FakeEngine>(log, domain::ExecutionResult(canned)));

    domain::Statement statement = testing::MustParse("SELECT id FROM users");
    domain::ExecutionResult result = service.Execute(statement);

    const domain::ExecutionError* error = testing::AsError(result);
    CHECK(error != nullptr);
    if (error == nullptr) {
      continue;
    }
    CHECK_EQ(static_cast<int>(error->kind), static_cast<int>(kind));
    CHECK_EQ(error->message, std::string("canned message"));
  }
}

TEST(test_query_service_returns_command_complete_from_engine) {
  auto log = std::make_shared<FakeEngineLog>();
  strata::application::QueryService service(std::make_unique<FakeEngine>(
      log, domain::ExecutionResult(domain::CommandComplete{"CREATE TABLE t"})));

  domain::Statement statement =
      testing::MustParse("CREATE TABLE t (id INT PRIMARY KEY)");
  domain::ExecutionResult result = service.Execute(statement);

  const domain::CommandComplete* complete = testing::AsCommandComplete(result);
  CHECK(complete != nullptr);
  if (complete != nullptr) {
    CHECK_EQ(complete->tag, std::string("CREATE TABLE t"));
  }
  CHECK_EQ(log->execute_calls, 1);
  CHECK(log->is_create_table);
}

TEST(test_query_service_describe_catalog_returns_schemas_in_order) {
  auto log = std::make_shared<FakeEngineLog>();
  strata::application::QueryService service(std::make_unique<FakeEngine>(
      log, domain::ExecutionResult(domain::AffectedRows{0})));
  testing::FakeCatalog catalog;

  std::vector<domain::TableSchema> schemas = service.DescribeCatalog();

  std::vector<std::string> names;
  for (const domain::TableSchema& schema : schemas) {
    names.push_back(schema.name);
  }
  CHECK_EQ(names, std::vector<std::string>({"users", "posts", "engagements",
                                            "measures", "drafts", "jobs"}));
  CHECK_EQ(schemas.size(), catalog.TableNames().size());
  for (std::size_t i = 0; i < schemas.size() && i < catalog.TableNames().size();
       ++i) {
    std::optional<domain::TableSchema> expected =
        catalog.FindTable(catalog.TableNames()[i]);
    CHECK(expected.has_value());
    if (!expected) {
      continue;
    }
    CHECK_EQ(schemas[i].columns.size(), expected->columns.size());
    CHECK_EQ(schemas[i].primary_key_index, expected->primary_key_index);
    CHECK_EQ(schemas[i].foreign_keys.size(), expected->foreign_keys.size());
    CHECK_EQ(schemas[i].unique_indexes, expected->unique_indexes);
    CHECK(schemas[i].auto_increment_index == expected->auto_increment_index);
  }
  CHECK_EQ(log->execute_calls, 0);
}
