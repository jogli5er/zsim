
#include <fstream>
#include <iostream>

#include "galloc.h"
#include "log.h"
#include "stats.h"
#include "zsim.h"

using std::endl;

class CsvBackendImpl : public GlobAlloc {
 private:
  const char* filename;
  AggregateStat* rootStat;

  void dumpStat(Stat* s, uint32_t level, std::ofstream* out) {
    if (std::string(s->name()) == "highPrefLatePc" ||
        std::string(s->name()) == "highPrefEarlyPc" ||
        std::string(s->name()) == "highMissPc" ||

        std::string(s->name()) == "highPrefHitPc") {
      *out << std::hex;
    } else {
      *out << std::dec;
    }
    for (uint32_t i = 0; i < level; i++) *out << ";";
    *out << "\"" << s->name() << "\""
         << ";";
    if (AggregateStat* as = dynamic_cast<AggregateStat*>(s)) {
      *out << "\"" << as->desc() << "\"" << endl;
      for (uint32_t i = 0; i < as->size(); i++) {
        dumpStat(as->get(i), level + 1, out);
      }
    } else if (ScalarStat* ss = dynamic_cast<ScalarStat*>(s)) {
      *out << "\"" << ss->get() << "\""
           << ";"
           << "\"" << ss->desc() << "\"" << endl;
    } else if (VectorStat* vs = dynamic_cast<VectorStat*>(s)) {
      *out << "\"" << vs->desc() << "\"" << endl;
      for (uint32_t i = 0; i < vs->size(); i++) {
        for (uint32_t j = 0; j < level + 1; j++) *out << ";";
        if (vs->hasCounterNames()) {
          *out << "\"" << vs->counterName(i) << "\""
               << ";"
               << "\"" << vs->count(i) << "\"" << endl;
        } else {
          *out << "\"" << i << "\""
               << ";"
               << "\"" << vs->count(i) << "\"" << endl;
        }
      }
    } else {
      panic("Unrecognized stat type");
    }
  }

 public:
  CsvBackendImpl(const char* _filename, AggregateStat* _rootStat)
      : filename(_filename), rootStat(_rootStat) {
    std::string newName = std::string(filename) + ".csv";

    filename = newName.c_str();
    std::ofstream out(newName.c_str(), std::ios_base::out);
    out << "# zsim stats" << endl;
  }

  void dump(bool buffered) {
    std::ofstream out(filename, std::ios_base::app);
    dumpStat(rootStat, 0, &out);
  }
};

CsvBackend::CsvBackend(const char* filename, AggregateStat* rootStat) {
  backend = new CsvBackendImpl(filename, rootStat);
}

void CsvBackend::dump(bool buffered) { backend->dump(buffered); }
