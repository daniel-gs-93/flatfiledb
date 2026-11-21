#ifndef STRATA_TESTS_TEST_SCALE_DATA_SUPPORT_H_
#define STRATA_TESTS_TEST_SCALE_DATA_SUPPORT_H_

#include <cstddef>
#include <filesystem>
#include <vector>

#include "strata/domain/value.h"

// The data of the volume tests (docs/TestDocument.md, Environment tier): three
// tables of 10000 rows each, large enough that a 64-frame buffer pool must
// evict pages. The tables track parcels through depots:
//
//   depots(id INT, code TEXT, town TEXT)
//   parcels(id INT, label TEXT, depot_code TEXT, weight_g INT)
//   scans(id INT, parcel_id INT, depot_code TEXT, status TEXT, remark TEXT,
//         scanned_at INT)
//
// Every name, town and sentence comes from word lists in the .cpp file, and
// every choice comes from a fixed pseudo-random sequence, so each run sees the
// same rows. No file is read: the rows exist in memory, and WriteScaleCsv
// writes them as CSV files when a test needs files.

namespace strata::testing {

inline constexpr std::size_t kScaleRowCount = 10000;

// The town of depot 1. No other depot has it, and the generator draws the depot
// of every other scan from depots 2 to 10000, so the scans of depot 1 are the
// six that it plants: three with status `received`, two with `delayed` and one
// with an empty status. Depot 1 has no parcel either.
inline constexpr char kScaleProbeTown[] = "Wrenfold";

// WriteScaleCsv puts three lines into depots.csv that the schema rejects: line
// 12 has `x10` where the id belongs, line 17 has 2 fields, and line 18 has 4.
inline constexpr std::size_t kScaleRejectedLineCount = 3;

struct ScaleTable {
  domain::TableSchema schema;
  std::vector<domain::Row> rows;  // in id order, without the rejected lines
};

// Builds the three tables, parents before children (depots, parcels, scans).
// Each call returns an independent copy that is equal to every other.
std::vector<ScaleTable> GenerateScaleTables();

// The tables of GenerateScaleTables, built once per process. The reference is
// valid until the process ends, and the caller must not modify it.
const std::vector<ScaleTable>& ScaleTables();

// Writes depots.csv, parcels.csv and scans.csv into `directory`, which is
// created when missing, overwriting files of the same name. The files have a
// header line, "\r\n" line ends, and no quoted field. depots.csv also holds
// the kScaleRejectedLineCount lines described above. A write failure is
// reported through ReportFailure.
void WriteScaleCsv(const std::filesystem::path& directory);

}  // namespace strata::testing

#endif  // STRATA_TESTS_TEST_SCALE_DATA_SUPPORT_H_
