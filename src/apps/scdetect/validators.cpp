#include "validators.h"

#include "processor.h"

namespace Seiscomp {
namespace detect {
namespace config {

bool ValidateXCorrThreshold(const double &thres) {
  return -1 <= thres && 1 >= thres;
}

bool ValidateArrivalOffsetThreshold(double thres) {
  return thres < 0 || (thres >= 2.0e-6);
}

bool ValidateMinArrivals(int n, int num_stream_configs) {
  if (n < 0) {
    return true;
  }

  return num_stream_configs > 0 ? n >= 1 : n >= 1 && n <= num_stream_configs;
}

bool ValidateFilter(const std::string &filter_string, std::string &err) {
  auto filter{Processor::Filter::Create(filter_string, &err)};
  if (!filter) {
    return false;
  }
  delete filter;
  return true;
}

} // namespace config
} // namespace detect
} // namespace Seiscomp
