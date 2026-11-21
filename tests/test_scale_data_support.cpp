#include "test_scale_data_support.h"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "test_paged_support.h"
#include "test_support.h"

namespace strata::testing {

namespace {

using domain::DataType;
using domain::Row;
using domain::TableSchema;
using domain::Value;

constexpr std::uint64_t kSeed = 20261119;
constexpr std::int64_t kFirstScanTime = 1735689600;  // 2025-01-01 00:00:00 UTC
constexpr std::int64_t kLastScanTime = 1767225599;   // 2025-12-31 23:59:59 UTC

// The status of each of the first six scans, which belong to depot 1.
constexpr std::string_view kPlantedStatuses[] = {
    "received", "received", "received", "delayed", "delayed", ""};

// Town names are a stem and a suffix: 85 x 62 = 5270 combinations, none of
// them kScaleProbeTown.
constexpr std::string_view kTownStems[] = {
    "Alder",   "Ash",     "Barrow", "Beacon", "Birch",  "Bramble", "Briar",
    "Brook",   "Cairn",   "Cedar",  "Chalk",  "Cinder", "Clay",    "Clover",
    "Copper",  "Cove",    "Crag",   "Cress",  "Dale",   "Drift",   "Dune",
    "Elder",   "Elm",     "Ember",  "Fallow", "Fen",    "Fern",    "Flint",
    "Gable",   "Garnet",  "Gorse",  "Grove",  "Hazel",  "Heath",   "Ivy",
    "Juniper", "Kelp",    "Kiln",   "Larch",  "Laurel", "Linden",  "Loch",
    "Maple",   "Meadow",  "Mill",   "Mist",   "Moor",   "Moss",    "Nettle",
    "Oak",     "Orchard", "Otter",  "Pebble", "Pine",   "Plover",  "Poppy",
    "Quarry",  "Quill",   "Rowan",  "Sable",  "Sage",   "Salt",    "Sedge",
    "Shale",   "Sloe",    "Sorrel", "Spruce", "Swan",   "Tarn",    "Thistle",
    "Thorn",   "Tide",    "Timber", "Violet", "Walnut", "Willow",  "Wren",
    "Yarrow",  "Yew",     "Bluff",  "Crow",   "Harbor", "Iron",    "Lynx",
    "Pike",
};

constexpr std::string_view kTownSuffixes[] = {
    "bridge", "brook",  "burn",   "bury",   "by",    "cliff", "combe", "cross",
    "down",   "field",  "ford",   "gate",   "glen",  "ham",   "hurst", "holm",
    "ley",    "mere",   "mouth",  "ness",   "pool",  "port",  "ridge", "shaw",
    "side",   "stead",  "stone",  "thorpe", "ton",   "wick",  "wood",  "worth",
    "haven",  "hollow", "land",   "moor",   "mount", "pass",  "rise",  "row",
    "run",    "shire",  "spring", "vale",   "view",  "ville", "ward",  "water",
    "well",   "wold",   "yard",   "bank",   "bay",   "beck",  "bend",  "bourne",
    "den",    "dene",   "fell",   "firth",  "garth", "kirk",
};

// The words of parcel labels and scan remarks.
constexpr std::string_view kSentenceWords[] = {
    "crate",    "carton", "pallet",  "sack",    "drum",      "parcel",
    "envelope", "tube",   "case",    "bundle",  "basket",    "barrel",
    "box",      "bag",    "tray",    "reel",    "roll",      "spool",
    "bin",      "chest",  "lamp",    "kettle",  "chair",     "mirror",
    "rug",      "clock",  "vase",    "ladder",  "toolkit",   "bicycle",
    "tent",     "guitar", "camera",  "printer", "monitor",   "shelf",
    "table",    "stool",  "curtain", "blanket", "pillow",    "jar",
    "pan",      "teapot", "lantern", "compass", "telescope", "saddle",
    "helmet",   "boot",   "jacket",  "scarf",   "glove",     "umbrella",
    "fragile",  "heavy",  "light",   "sealed",  "wrapped",   "padded",
    "labeled",  "spare",  "bulky",   "narrow",  "wide",      "tall",
    "small",    "large",  "loose",   "folded",  "boxed",     "stacked",
    "coiled",   "packed", "stored",  "loaded",  "moved",     "held",
    "kept",     "marked", "checked", "sorted",  "shipped",   "returned",
    "with",     "near",   "beside",  "under",   "inside",    "behind",
    "from",     "for",    "and",     "the",     "red",       "blue",
    "green",    "grey",   "brown",   "amber",   "white",     "black",
    "copper",   "silver", "golden",  "olive",   "ivory",     "plain",
};

// SplitMix64: a pseudo-random generator that gives the same sequence on every
// platform and standard library, unlike std::uniform_int_distribution.
class SplitMix64 {
 public:
  explicit SplitMix64(std::uint64_t seed) : state_(seed) {}

  std::uint64_t Next() {
    state_ += 0x9E3779B97F4A7C15ull;
    std::uint64_t mixed = state_;
    mixed = (mixed ^ (mixed >> 30)) * 0xBF58476D1CE4E5B9ull;
    mixed = (mixed ^ (mixed >> 27)) * 0x94D049BB133111EBull;
    return mixed ^ (mixed >> 31);
  }

  // A number in [0, bound). The modulo bias is below 2^-39 for the bounds of
  // this file (the largest is the 31,536,000 seconds of a year).
  std::size_t Below(std::size_t bound) {
    return static_cast<std::size_t>(Next() % bound);
  }

  // A number in [low, high].
  std::size_t Between(std::size_t low, std::size_t high) {
    return low + Below(high - low + 1);
  }

 private:
  std::uint64_t state_;
};

std::string PadNumber(std::size_t number, std::size_t width) {
  std::string digits = std::to_string(number);
  if (digits.size() < width) {
    digits.insert(0, width - digits.size(), '0');
  }
  return digits;
}

// "D00042" plus a letter that is a function of the id, so codes are unique and
// not simply ascending.
std::string DepotCode(std::size_t id) {
  std::string code = "D" + PadNumber(id, 5);
  code.push_back(static_cast<char>('A' + (id * 7) % 26));
  return code;
}

std::string TownName(SplitMix64& rng) {
  std::string town(kTownStems[rng.Below(std::size(kTownStems))]);
  town += kTownSuffixes[rng.Below(std::size(kTownSuffixes))];
  return town;
}

// A capitalized sentence of random words that ends in a period. Its length is
// at most `high` and, when `target` lies in [low, high], reaches `target`
// unless the next word would not fit.
std::string BuildSentence(SplitMix64& rng, std::size_t target,
                          std::size_t high) {
  std::string text;
  while (true) {
    const std::string_view word =
        kSentenceWords[rng.Below(std::size(kSentenceWords))];
    const std::size_t extra = word.size() + (text.empty() ? 0 : 1);
    if (text.size() + extra + 1 > high) {
      break;
    }
    if (!text.empty()) {
      text.push_back(' ');
    }
    text += word;
    if (text.size() + 1 >= target) {
      break;
    }
  }
  text.front() =
      static_cast<char>(std::toupper(static_cast<unsigned char>(text.front())));
  text.push_back('.');
  return text;
}

// A label of 13 to 119 characters, with most of them long.
std::string BuildLabel(SplitMix64& rng) {
  constexpr std::size_t kLow = 13;
  constexpr std::size_t kHigh = 119;
  constexpr std::size_t kSpan = kHigh - kLow;
  const std::size_t shortfall =
      rng.Below(kSpan + 1) * rng.Below(kSpan + 1) / kSpan;
  return BuildSentence(rng, kHigh - shortfall, kHigh);
}

// A remark of 11 to 68 characters.
std::string BuildRemark(SplitMix64& rng) {
  constexpr std::size_t kLow = 11;
  constexpr std::size_t kHigh = 68;
  const std::size_t target =
      (rng.Between(kLow, kHigh) + rng.Between(kLow, kHigh)) / 2;
  return BuildSentence(rng, target, kHigh);
}

std::string StatusOf(std::size_t percentile) {
  if (percentile < 54) {
    return "received";
  }
  if (percentile < 90) {
    return "delayed";
  }
  return "";
}

Value Number(std::size_t number) {
  return Value(static_cast<std::int64_t>(number));
}

ScaleTable BuildDepots(SplitMix64& rng) {
  ScaleTable table;
  table.schema = MakeSchema(
      "depots", {Column("id", DataType::kInt), Column("code", DataType::kText),
                 Column("town", DataType::kText)});
  for (std::size_t id = 1; id <= kScaleRowCount; ++id) {
    // The town is drawn for depot 1 too, so that the sequence does not depend
    // on which depot is planted.
    std::string town = TownName(rng);
    if (id == 1) {
      town = kScaleProbeTown;
    }
    table.rows.push_back(
        Row{Number(id), Value(DepotCode(id)), Value(std::move(town))});
  }
  return table;
}

// Parcels belong to depots 2 to 10000, never to depot 1.
ScaleTable BuildParcels(SplitMix64& rng) {
  ScaleTable table;
  table.schema = MakeSchema("parcels", {Column("id", DataType::kInt),
                                        Column("label", DataType::kText),
                                        Column("depot_code", DataType::kText),
                                        Column("weight_g", DataType::kInt)});
  for (std::size_t id = 1; id <= kScaleRowCount; ++id) {
    std::string label = BuildLabel(rng);
    const std::size_t depot_id = 2 + rng.Below(kScaleRowCount - 1);
    const std::size_t weight = 100 + rng.Below(20000);
    table.rows.push_back(Row{Number(id), Value(std::move(label)),
                             Value(DepotCode(depot_id)), Number(weight)});
  }
  return table;
}

// The first six scans are planted (kPlantedStatuses, depot 1); the others
// belong to depots 2 to 10000. The remark is text only for `delayed`.
ScaleTable BuildScans(SplitMix64& rng) {
  ScaleTable table;
  table.schema = MakeSchema(
      "scans",
      {Column("id", DataType::kInt), Column("parcel_id", DataType::kInt),
       Column("depot_code", DataType::kText), Column("status", DataType::kText),
       Column("remark", DataType::kText),
       Column("scanned_at", DataType::kInt)});
  const std::size_t planted_count = std::size(kPlantedStatuses);
  for (std::size_t id = 1; id <= kScaleRowCount; ++id) {
    const std::size_t parcel_id = 1 + rng.Below(kScaleRowCount);
    std::size_t depot_id = 2 + rng.Below(kScaleRowCount - 1);
    std::string status = StatusOf(rng.Below(100));
    const std::size_t scanned_at =
        rng.Between(static_cast<std::size_t>(kFirstScanTime),
                    static_cast<std::size_t>(kLastScanTime));
    if (id <= planted_count) {
      depot_id = 1;
      status = kPlantedStatuses[id - 1];
    }
    std::string remark = status == "delayed" ? BuildRemark(rng) : "-";
    table.rows.push_back(Row{Number(id), Number(parcel_id),
                             Value(DepotCode(depot_id)),
                             Value(std::move(status)), Value(std::move(remark)),
                             Number(scanned_at)});
  }
  return table;
}

std::string FieldText(const Value& value) {
  const std::int64_t* number = std::get_if<std::int64_t>(&value);
  if (number != nullptr) {
    return std::to_string(*number);
  }
  return std::get<std::string>(value);  // the tables hold no DOUBLE
}

std::string CsvLine(const Row& row) {
  std::string line;
  for (const Value& value : row) {
    if (!line.empty()) {
      line.push_back(',');
    }
    line += FieldText(value);
  }
  return line;
}

std::string HeaderLine(const TableSchema& schema) {
  std::string line;
  for (const domain::ColumnDef& column : schema.columns) {
    if (!line.empty()) {
      line.push_back(',');
    }
    line += column.name;
  }
  return line;
}

// Adds the three lines that the schema rejects. `lines` holds the header at
// index 0 and the row with id k at index k, so a line number is its index + 1.
// Line 12 is row 10 with an id that is not a number, and lines 17 and 18 are
// row 15 with a semicolon in place of its last comma (2 fields) and with one
// field too many (4 fields).
void InsertRejectedLines(std::vector<std::string>* lines) {
  const std::string bad_id = "x" + (*lines)[10];
  std::string two_fields = (*lines)[15];
  two_fields[two_fields.rfind(',')] = ';';
  const std::string four_fields = (*lines)[15] + ",spare";
  lines->insert(lines->begin() + 11, bad_id);
  lines->insert(lines->begin() + 16, two_fields);
  lines->insert(lines->begin() + 17, four_fields);
}

}  // namespace

std::vector<ScaleTable> GenerateScaleTables() {
  SplitMix64 rng(kSeed);
  std::vector<ScaleTable> tables;
  tables.push_back(BuildDepots(rng));
  tables.push_back(BuildParcels(rng));
  tables.push_back(BuildScans(rng));
  return tables;
}

const std::vector<ScaleTable>& ScaleTables() {
  static const std::vector<ScaleTable> tables = GenerateScaleTables();
  return tables;
}

void WriteScaleCsv(const std::filesystem::path& directory) {
  for (const ScaleTable& table : ScaleTables()) {
    std::vector<std::string> lines = {HeaderLine(table.schema)};
    for (const Row& row : table.rows) {
      lines.push_back(CsvLine(row));
    }
    if (table.schema.name == "depots") {
      InsertRejectedLines(&lines);
    }
    std::string text;
    for (const std::string& line : lines) {
      text += line;
      text += "\r\n";
    }
    WriteFile(directory / (table.schema.name + ".csv"), text);
  }
}

}  // namespace strata::testing
