#include "test_support.h"

#include <unistd.h>

#include <atomic>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "strata/csv_codec.h"
#include "strata/csv_file_source.h"
#include "strata/domain/binder.h"
#include "strata/sql_parser.h"
#include "test_harness.h"

namespace strata::testing {

namespace {

std::atomic<int> next_directory_number{0};

void RestorePermissions(const std::filesystem::path& root) {
  std::error_code error;
  std::filesystem::permissions(root, std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::add, error);
  if (!std::filesystem::is_directory(root, error)) {
    return;
  }
  for (const auto& entry : std::filesystem::directory_iterator(root, error)) {
    RestorePermissions(entry.path());
  }
}

domain::ColumnDef Column(const std::string& name, domain::DataType type) {
  return domain::ColumnDef{name, type};
}

}  // namespace

TempDir::TempDir() {
  std::string name = "strata_test_" + std::to_string(::getpid()) + "_" +
                     std::to_string(next_directory_number.fetch_add(1));
  std::error_code error;
  path_ = std::filesystem::temp_directory_path(error) / name;
  if (error) {
    ReportFailure(__FILE__, __LINE__, "no temp directory: " + error.message());
    return;
  }
  std::filesystem::remove_all(path_, error);
  std::filesystem::create_directories(path_, error);
  if (error) {
    ReportFailure(__FILE__, __LINE__,
                  "cannot create " + path_.string() + ": " + error.message());
  }
}

TempDir::~TempDir() {
  RestorePermissions(path_);
  std::error_code error;
  std::filesystem::remove_all(path_, error);
}

void WriteFile(const std::filesystem::path& path, const std::string& content) {
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file << content;
  file.flush();
  if (error || !file.good()) {
    ReportFailure(__FILE__, __LINE__, "cannot write " + path.string());
  }
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    ReportFailure(__FILE__, __LINE__, "cannot read " + path.string());
    return "";
  }
  return std::string(std::istreambuf_iterator<char>(file),
                     std::istreambuf_iterator<char>());
}

void WriteCatalogFixture(const std::filesystem::path& dir) {
  WriteFile(dir / "catalog.csv",
            infrastructure::SerializeCatalog(
                {UsersSchema(), PostsSchema(), EngagementsSchema()}));
}

void WriteFixtureA(const std::filesystem::path& dir) {
  WriteCatalogFixture(dir);
  WriteFile(dir / "users.csv",
            "id,username,location\n"
            "1,alice,Paris\n"
            "2,carol,Paris\n"
            "3,bob,Rome\n"
            "4,dave,Oslo\n");
  WriteFile(dir / "posts.csv",
            "id,content,username,views\n"
            "10,hello world,alice,5\n"
            "11,second post,alice,0\n"
            "12,bobs post,bob,7\n");
  WriteFile(dir / "engagements.csv",
            "id,postid,username,type,comment,timestamp\n"
            "100,10,bob,like,None,1000\n"
            "101,10,carol,comment,nice,1001\n"
            "102,11,bob,comment,ok,1002\n"
            "103,12,alice,like,None,1003\n"
            "104,12,carol,like,None,1004\n");
}

void WriteFixtureB(const std::filesystem::path& dir) {
  WriteCatalogFixture(dir);
  WriteFile(dir / "users.csv",
            "id,username,location\n"
            "1,alice,Paris\n"
            "x,bad,Nowhere\n"
            "5,eve\n"
            "1,alicia,Rome\n"
            "2,bob,Rome\n");
  WriteFile(dir / "posts.csv",
            "id,content,username,views\n"
            "10,hello,alice,5\n"
            "11,bad views,bob,abc\n");
  WriteFile(dir / "engagements.csv",
            "id,postid,username,type,comment,timestamp\n"
            "100,10,bob,like,None,1\n"
            "101,99,bob,like,None,2\n"
            "102,10,zed,like,None,3\n"
            "103,10,alicia,comment,hi,4\n");
}

void WriteFixtureC1(const std::filesystem::path& dir) {
  WriteCatalogFixture(dir);
  WriteFile(dir / "users.csv",
            "id,username,location\n"
            "1,a,b\n"
            "2,b,c\n");
  WriteFile(dir / "posts.csv",
            "id,content,username,views\n"
            "10,x,a,0\n"
            "11,y,b,0\n");
  WriteFile(dir / "engagements.csv",
            "id,postid,username,type,comment,timestamp\n");
}

void WriteFixtureC2(const std::filesystem::path& dir) {
  WriteCatalogFixture(dir);
  WriteFile(dir / "users.csv",
            "id,username,location\n"
            "1,a,b\n"
            "2,b,a\n");
  WriteFile(dir / "posts.csv",
            "id,content,username,views\n"
            "10,x,a,0\n"
            "11,y,b,0\n");
  WriteFile(dir / "engagements.csv",
            "id,postid,username,type,comment,timestamp\n");
}

void WriteFixtureD(const std::filesystem::path& dir) {
  WriteFile(dir / "catalog.csv",
            infrastructure::SerializeCatalog(
                {domain::TableSchema{"notes",
                                     {Column("id", domain::DataType::kInt),
                                      Column("body", domain::DataType::kText)},
                                     0,
                                     {}}}));
  WriteFile(dir / "notes.csv",
            "id,body\n"
            "1,\"a,b\"\n"
            "2,\"say \"\"hi\"\"\"\n"
            "3,plain\n"
            "4,\"\"\n"
            "5,\"unterminated\n"
            "6,\"ok\"x\n");
}

std::unique_ptr<infrastructure::CsvRepository> MakeCsvRepository(
    const std::filesystem::path& dir) {
  return std::make_unique<infrastructure::CsvRepository>(dir.string());
}

domain::TableSchema UsersSchema() {
  return domain::TableSchema{"users",
                             {Column("id", domain::DataType::kInt),
                              Column("username", domain::DataType::kText),
                              Column("location", domain::DataType::kText)},
                             0,
                             {},
                             {1}};
}

domain::TableSchema PostsSchema() {
  return domain::TableSchema{"posts",
                             {Column("id", domain::DataType::kInt),
                              Column("content", domain::DataType::kText),
                              Column("username", domain::DataType::kText),
                              Column("views", domain::DataType::kInt)},
                             0,
                             {domain::ForeignKey{2, "users", 1}}};
}

domain::TableSchema EngagementsSchema() {
  return domain::TableSchema{
      "engagements",
      {Column("id", domain::DataType::kInt),
       Column("postId", domain::DataType::kInt),
       Column("username", domain::DataType::kText),
       Column("type", domain::DataType::kText),
       Column("comment", domain::DataType::kText),
       Column("timestamp", domain::DataType::kInt)},
      0,
      {domain::ForeignKey{1, "posts", 0}, domain::ForeignKey{2, "users", 1}}};
}

domain::TableSchema MeasuresSchema() {
  return domain::TableSchema{"measures",
                             {Column("id", domain::DataType::kInt),
                              Column("score", domain::DataType::kDouble)},
                             0,
                             {}};
}

domain::TableSchema DraftsSchema() {
  domain::ColumnDef owner = Column("owner", domain::DataType::kText);
  owner.default_value = domain::Value(std::string("none"));
  domain::ColumnDef score = Column("score", domain::DataType::kDouble);
  score.default_value = domain::Value(1.0);
  return domain::TableSchema{
      "drafts",
      {Column("id", domain::DataType::kInt),
       Column("body", domain::DataType::kText), owner, score},
      0,
      {}};
}

domain::TableSchema JobsSchema() {
  return domain::TableSchema{"jobs",
                             {Column("id", domain::DataType::kInt),
                              Column("body", domain::DataType::kText)},
                             0,
                             {},
                             {},
                             std::optional<std::size_t>(0)};
}

std::optional<domain::TableSchema> FakeCatalog::FindTable(
    const std::string& name) const {
  std::string lowered = domain::ToLower(name);
  for (const auto& table_name : TableNames()) {
    if (table_name != lowered) {
      continue;
    }
    if (lowered == "users") {
      return UsersSchema();
    }
    if (lowered == "posts") {
      return PostsSchema();
    }
    if (lowered == "engagements") {
      return EngagementsSchema();
    }
    if (lowered == "drafts") {
      return DraftsSchema();
    }
    if (lowered == "jobs") {
      return JobsSchema();
    }
    return MeasuresSchema();
  }
  return std::nullopt;
}

std::vector<std::string> FakeCatalog::TableNames() const {
  return {"users", "posts", "engagements", "measures", "drafts", "jobs"};
}

DecoratorRepository::DecoratorRepository(
    std::unique_ptr<infrastructure::IFlatFileRepository> inner,
    std::shared_ptr<RepositoryLog> log)
    : inner_(std::move(inner)), log_(std::move(log)) {}

infrastructure::LoadOutcome DecoratorRepository::Load(bool is_parallel) {
  return inner_->Load(is_parallel);
}

infrastructure::InspectOutcome DecoratorRepository::InspectTable(
    const domain::TableSchema& schema) {
  return inner_->InspectTable(schema);
}

std::optional<std::string> DecoratorRepository::Commit(
    const infrastructure::CommitSet& set) {
  ++log_->write_calls;
  std::vector<std::string> names;
  for (const auto& write : set.writes) {
    names.push_back(write.schema.name);
  }
  log_->written_table_names.push_back(names);
  if (log_->write_failure) {
    return log_->write_failure;
  }
  return inner_->Commit(set);
}

std::optional<std::string> DecoratorRepository::AppendRows(
    const domain::TableSchema& schema, const std::vector<domain::Row>& rows) {
  ++log_->append_calls;
  if (log_->append_failure) {
    return log_->append_failure;
  }
  return inner_->AppendRows(schema, rows);
}

domain::Statement MustParse(const std::string& sql) {
  adapter::ParseResult parsed = adapter::ParseStatement(sql);
  if (auto* statement = std::get_if<domain::Statement>(&parsed)) {
    return std::move(*statement);
  }
  const auto& error = std::get<adapter::ParseError>(parsed);
  ReportFailure(__FILE__, __LINE__,
                "parse failed for [" + sql + "] at column " +
                    std::to_string(error.column) + ": " + error.description);
  return domain::SelectStatement{};
}

domain::PlanPtr MustBind(const std::string& sql,
                         const domain::ICatalog& catalog) {
  domain::Statement statement = MustParse(sql);
  domain::BindResult bound = domain::Bind(statement, catalog);
  if (auto* plan = std::get_if<domain::PlanPtr>(&bound)) {
    return std::move(*plan);
  }
  ReportFailure(__FILE__, __LINE__,
                "bind failed for [" + sql +
                    "]: " + std::get<domain::ExecutionError>(bound).message);
  return nullptr;
}

domain::ExecutionResult ExecuteSql(application::QueryService& service,
                                   const std::string& sql) {
  adapter::ParseResult parsed = adapter::ParseStatement(sql);
  if (auto* error = std::get_if<adapter::ParseError>(&parsed)) {
    return domain::ExecutionError{
        domain::ErrorKind::kBind,
        "parse error: " + std::to_string(error->column) + ": " +
            error->description};
  }
  return service.Execute(std::get<domain::Statement>(parsed));
}

std::unique_ptr<application::QueryService> MakeLoadedService(
    const std::filesystem::path& dir, std::shared_ptr<RepositoryLog> log) {
  std::unique_ptr<infrastructure::IFlatFileRepository> repository =
      MakeCsvRepository(dir);
  if (log) {
    repository =
        std::make_unique<DecoratorRepository>(std::move(repository), log);
  }
  auto engine =
      std::make_unique<infrastructure::FlatFileEngine>(std::move(repository));
  infrastructure::LoadResult loaded = engine->Load(false);
  if (!loaded.is_success) {
    ReportFailure(
        __FILE__, __LINE__,
        "engine failed to load " + dir.string() + ": " + loaded.error);
    return nullptr;
  }
  return std::make_unique<application::QueryService>(
      std::move(engine), std::make_unique<infrastructure::FileCsvSource>());
}

const domain::ResultSet* AsResultSet(const domain::ExecutionResult& result) {
  return std::get_if<domain::ResultSet>(&result);
}

const domain::AffectedRows* AsAffectedRows(
    const domain::ExecutionResult& result) {
  return std::get_if<domain::AffectedRows>(&result);
}

const domain::CommandComplete* AsCommandComplete(
    const domain::ExecutionResult& result) {
  return std::get_if<domain::CommandComplete>(&result);
}

const domain::ExecutionError* AsError(const domain::ExecutionResult& result) {
  return std::get_if<domain::ExecutionError>(&result);
}

std::vector<std::string> RowsAsText(const domain::ResultSet& result_set) {
  std::vector<std::string> lines;
  for (const auto& row : result_set.rows) {
    std::string line;
    for (std::size_t i = 0; i < row.size(); ++i) {
      if (i > 0) {
        line += '\t';
      }
      line += domain::FormatValue(row[i]);
    }
    lines.push_back(line);
  }
  return lines;
}

}  // namespace strata::testing
