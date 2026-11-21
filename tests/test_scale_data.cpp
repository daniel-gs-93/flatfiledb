#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <variant>
#include <vector>

#include "strata/domain/value.h"
#include "test_harness.h"
#include "test_scale_data_support.h"
#include "test_support.h"

// Unit tests of the volume-test data of test_scale_data_support.cpp: its shape,
// its determinism, the values it plants and the format of the CSV files.

namespace {

using strata::domain::Row;
using strata::testing::GenerateScaleTables;
using strata::testing::kScaleRejectedLineCount;
using strata::testing::kScaleRowCount;
using strata::testing::ScaleTable;
using strata::testing::ScaleTables;

std::int64_t NumberAt(const Row& row, std::size_t column) {
  return std::get<std::int64_t>(row[column]);
}

const std::string& TextAt(const Row& row, std::size_t column) {
  return std::get<std::string>(row[column]);
}

std::vector<std::string> NamesOfColumns(const ScaleTable& table) {
  std::vector<std::string> names;
  for (const auto& column : table.schema.columns) {
    names.push_back(column.name);
  }
  return names;
}

// The lines of a CSV text whose every line ends in "\r\n".
std::vector<std::string> LinesOf(const std::string& text) {
  std::vector<std::string> lines;
  std::size_t begin = 0;
  while (begin < text.size()) {
    const std::size_t end = text.find("\r\n", begin);
    if (end == std::string::npos) {
      lines.push_back(text.substr(begin));
      break;
    }
    lines.push_back(text.substr(begin, end - begin));
    begin = end + 2;
  }
  return lines;
}

std::size_t FieldCount(const std::string& line) {
  return 1 + static_cast<std::size_t>(std::ranges::count(line, ','));
}

// FNV-1a over the bytes of the three CSV files: it changes when any byte of
// the generated data changes.
std::uint64_t Fingerprint(const std::string& text, std::uint64_t seed) {
  std::uint64_t hash = seed;
  for (const char byte : text) {
    hash ^= static_cast<unsigned char>(byte);
    hash *= 0x100000001B3ull;
  }
  return hash;
}

}  // namespace

TEST(test_scale_data_tables_have_the_documented_shape) {
  const std::vector<ScaleTable>& tables = ScaleTables();
  CHECK_EQ(tables.size(), std::size_t{3});
  if (tables.size() != 3) {
    return;
  }
  CHECK_EQ(tables[0].schema.name, std::string("depots"));
  CHECK_EQ(tables[1].schema.name, std::string("parcels"));
  CHECK_EQ(tables[2].schema.name, std::string("scans"));
  CHECK_EQ(NamesOfColumns(tables[0]),
           std::vector<std::string>({"id", "code", "town"}));
  CHECK_EQ(NamesOfColumns(tables[1]),
           std::vector<std::string>({"id", "label", "depot_code", "weight_g"}));
  CHECK_EQ(NamesOfColumns(tables[2]),
           std::vector<std::string>({"id", "parcel_id", "depot_code", "status",
                                     "remark", "scanned_at"}));
  for (const ScaleTable& table : tables) {
    CHECK_EQ(table.rows.size(), kScaleRowCount);
    for (std::size_t i = 0; i < table.rows.size(); ++i) {
      CHECK_EQ(table.rows[i].size(), table.schema.columns.size());
      CHECK_EQ(NumberAt(table.rows[i], 0), static_cast<std::int64_t>(i + 1));
    }
  }
}

TEST(test_scale_data_generation_is_deterministic) {
  const std::vector<ScaleTable> first = GenerateScaleTables();
  const std::vector<ScaleTable> second = GenerateScaleTables();
  CHECK_EQ(first.size(), second.size());
  CHECK_EQ(first.size(), ScaleTables().size());
  for (std::size_t i = 0; i < first.size() && i < second.size(); ++i) {
    CHECK_EQ(first[i].schema.name, second[i].schema.name);
    CHECK(first[i].rows == second[i].rows);
    CHECK(first[i].rows == ScaleTables()[i].rows);
  }
}

TEST(test_scale_data_keys_and_references_are_valid) {
  const std::vector<ScaleTable>& tables = ScaleTables();
  if (tables.size() != 3) {
    return;
  }
  std::unordered_set<std::string> codes;
  std::unordered_set<std::string> towns;
  for (const Row& depot : tables[0].rows) {
    codes.insert(TextAt(depot, 1));
    towns.insert(TextAt(depot, 2));
  }
  CHECK_EQ(codes.size(), kScaleRowCount);  // code is unique
  CHECK(towns.size() > 3000);              // many towns, with some repeated
  CHECK(towns.size() < kScaleRowCount);
  const std::string& depot_one = TextAt(tables[0].rows[0], 1);

  for (const Row& parcel : tables[1].rows) {
    CHECK(codes.contains(TextAt(parcel, 2)));
    CHECK(TextAt(parcel, 2) != depot_one);  // parcels never use depot 1
    CHECK(NumberAt(parcel, 3) >= 100 && NumberAt(parcel, 3) < 20100);
  }
  for (const Row& scan : tables[2].rows) {
    CHECK(NumberAt(scan, 1) >= 1 &&
          NumberAt(scan, 1) <= static_cast<std::int64_t>(kScaleRowCount));
    CHECK(codes.contains(TextAt(scan, 2)));
    CHECK(NumberAt(scan, 5) >= 1735689600 && NumberAt(scan, 5) <= 1767225599);
  }
}

TEST(test_scale_data_plants_the_probe_values) {
  const std::vector<ScaleTable>& tables = ScaleTables();
  if (tables.size() != 3) {
    return;
  }
  const std::string probe_town = strata::testing::kScaleProbeTown;
  std::size_t probe_town_count = 0;
  for (const Row& depot : tables[0].rows) {
    if (TextAt(depot, 2) == probe_town) {
      ++probe_town_count;
      CHECK_EQ(NumberAt(depot, 0), std::int64_t{1});
    }
  }
  CHECK_EQ(probe_town_count, std::size_t{1});

  // The scans of depot 1 are exactly the six planted ones.
  const std::string& depot_one = TextAt(tables[0].rows[0], 1);
  std::vector<std::string> statuses;
  for (const Row& scan : tables[2].rows) {
    if (TextAt(scan, 2) == depot_one) {
      CHECK(NumberAt(scan, 0) <= 6);
      statuses.push_back(TextAt(scan, 3));
    }
  }
  std::ranges::sort(statuses);
  CHECK_EQ(statuses,
           std::vector<std::string>(
               {"", "delayed", "delayed", "received", "received", "received"}));

  // Ninety-nine codes start with D000 (ids 1 to 99), so `LIKE 'd000%'` has
  // more than two matches.
  std::size_t prefixed = 0;
  for (const Row& depot : tables[0].rows) {
    if (TextAt(depot, 1).starts_with("D000")) {
      ++prefixed;
    }
  }
  CHECK_EQ(prefixed, std::size_t{99});
}

TEST(test_scale_data_text_follows_its_rules) {
  const std::vector<ScaleTable>& tables = ScaleTables();
  if (tables.size() != 3) {
    return;
  }
  const std::string forbidden = ",\"\r\n";
  for (const ScaleTable& table : tables) {
    for (const Row& row : table.rows) {
      for (const auto& value : row) {
        if (const std::string* text = std::get_if<std::string>(&value)) {
          CHECK(text->find_first_of(forbidden) == std::string::npos);
        }
      }
    }
  }
  for (const Row& parcel : tables[1].rows) {
    const std::string& label = TextAt(parcel, 1);
    CHECK(label.size() >= 13 && label.size() <= 119);
    CHECK(label.back() == '.');
    CHECK(label.front() >= 'A' && label.front() <= 'Z');
  }
  std::size_t received = 0;
  std::size_t delayed = 0;
  std::size_t empty = 0;
  for (const Row& scan : tables[2].rows) {
    const std::string& status = TextAt(scan, 3);
    const std::string& remark = TextAt(scan, 4);
    if (status == "delayed") {
      ++delayed;
      CHECK(remark.size() >= 11 && remark.size() <= 68);
    } else {
      CHECK_EQ(remark, std::string("-"));
      if (status.empty()) {
        ++empty;
      } else {
        CHECK_EQ(status, std::string("received"));
        ++received;
      }
    }
  }
  // About 54, 36 and 10 percent of the rows.
  CHECK(received > 4800 && received < 6000);
  CHECK(delayed > 3000 && delayed < 4200);
  CHECK(empty > 800 && empty < 1200);
  CHECK_EQ(received + delayed + empty, kScaleRowCount);
}

TEST(test_scale_data_csv_files_have_the_documented_format) {
  strata::testing::TempDir directory;
  strata::testing::WriteScaleCsv(directory.path());
  const std::string depots =
      strata::testing::ReadFile(directory.path() / "depots.csv");
  const std::string parcels =
      strata::testing::ReadFile(directory.path() / "parcels.csv");
  const std::string scans =
      strata::testing::ReadFile(directory.path() / "scans.csv");

  for (const std::string* text : {&depots, &parcels, &scans}) {
    CHECK(text->ends_with("\r\n"));
    CHECK(text->find('"') == std::string::npos);
    // Every "\n" belongs to a "\r\n".
    CHECK_EQ(std::ranges::count(*text, '\n'), std::ranges::count(*text, '\r'));
  }
  const std::vector<std::string> depot_lines = LinesOf(depots);
  const std::vector<std::string> parcel_lines = LinesOf(parcels);
  const std::vector<std::string> scan_lines = LinesOf(scans);
  CHECK_EQ(depot_lines.size(), 1 + kScaleRowCount + kScaleRejectedLineCount);
  CHECK_EQ(parcel_lines.size(), 1 + kScaleRowCount);
  CHECK_EQ(scan_lines.size(), 1 + kScaleRowCount);
  if (depot_lines.size() < 19 || parcel_lines.empty() || scan_lines.empty()) {
    return;
  }
  CHECK_EQ(depot_lines[0], std::string("id,code,town"));
  CHECK_EQ(parcel_lines[0], std::string("id,label,depot_code,weight_g"));
  CHECK_EQ(scan_lines[0],
           std::string("id,parcel_id,depot_code,status,remark,scanned_at"));

  // The three lines that the schema rejects, at lines 12, 17 and 18.
  CHECK(depot_lines[11].starts_with("x10,"));
  CHECK_EQ(FieldCount(depot_lines[11]), std::size_t{3});
  CHECK(depot_lines[16].starts_with("15,"));
  CHECK_EQ(FieldCount(depot_lines[16]), std::size_t{2});
  CHECK(depot_lines[17].starts_with("15,"));
  CHECK_EQ(FieldCount(depot_lines[17]), std::size_t{4});
  CHECK(depot_lines[18].starts_with("15,"));  // row 15 follows its two copies
  CHECK_EQ(FieldCount(depot_lines[18]), std::size_t{3});
  std::size_t rejected = 0;
  for (std::size_t i = 1; i < depot_lines.size(); ++i) {
    if (FieldCount(depot_lines[i]) != 3 || depot_lines[i][0] == 'x') {
      ++rejected;
    }
  }
  CHECK_EQ(rejected, kScaleRejectedLineCount);
  for (std::size_t i = 1; i < parcel_lines.size(); ++i) {
    CHECK_EQ(FieldCount(parcel_lines[i]), std::size_t{4});
  }
  for (std::size_t i = 1; i < scan_lines.size(); ++i) {
    CHECK_EQ(FieldCount(scan_lines[i]), std::size_t{6});
  }

  // Any change to the generator changes this value, which makes the change
  // deliberate. The value does not depend on the platform.
  std::uint64_t fingerprint = 0xCBF29CE484222325ull;
  fingerprint = Fingerprint(depots, fingerprint);
  fingerprint = Fingerprint(parcels, fingerprint);
  fingerprint = Fingerprint(scans, fingerprint);
  CHECK_EQ(fingerprint, std::uint64_t{17521446498847180916ull});
}
