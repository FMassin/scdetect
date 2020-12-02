#ifndef SCDETECT_APPS_SCDETECT_UTILS_H_
#define SCDETECT_APPS_SCDETECT_UTILS_H_

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Seiscomp {
namespace detect {
namespace utils {

template <typename T> bool IsGeZero(const T num) { return 0 <= num; }
bool ValidatePhase(const std::string &phase);
bool ValidateXCorrThreshold(const double &thres);

template <typename TMap>
auto map_keys(const TMap &map) -> std::vector<decltype(TMap::key_type)> {
  std::vector<decltype(TMap::key_type)> retval;
  for (const auto &pair : map)
    retval.push_back(pair.first);

  return retval;
}

template <typename TMap>
auto map_values(const TMap &map) -> std::vector<decltype(TMap::key_type)> {
  std::vector<decltype(TMap::key_type)> retval;
  for (const auto &pair : map)
    retval.push_back(pair.second);

  return retval;
}

template <typename TEnum>
auto as_integer(const TEnum value) ->
    typename std::underlying_type<TEnum>::type {
  return static_cast<typename std::underlying_type<TEnum>::type>(value);
}

// Provide C++11 make_unique<T>()
template <typename T, typename... Ts>
std::unique_ptr<T> make_unique(Ts &&... params) {
  return std::unique_ptr<T>(new T(std::forward<Ts>(params)...));
}

template <typename TMap, typename Predicate>
std::vector<typename TMap::key_type> filter_keys(const TMap &m, Predicate &p) {

  std::vector<typename TMap::key_type> retval;
  for (const auto &pair : m) {
    if (p(pair)) {
      retval.push_back(pair.first);
    }
  }
  return retval;
}

/* ------------------------------------------------------------------------- */
class WaveformStreamID {
public:
  explicit WaveformStreamID(const std::string &net_sta_loc_cha);
  WaveformStreamID(const std::string &net_code, const std::string &sta_code,
                   const std::string &loc_code, const std::string &cha_code);

  const std::string &net_code() const;
  const std::string &sta_code() const;
  const std::string &loc_code() const;
  const std::string &cha_code() const;

  bool IsValid() const;

  friend std::ostream &operator<<(std::ostream &os, const WaveformStreamID &id);

protected:
  const std::string delimiter_{"."};

private:
  std::string net_code_;
  std::string sta_code_;
  std::string loc_code_;
  std::string cha_code_;
};

} // namespace utils
} // namespace detect
} // namespace Seiscomp

#endif // SCDETECT_APPS_SCDETECT_UTILS_H_
