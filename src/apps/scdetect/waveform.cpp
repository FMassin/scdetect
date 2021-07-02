#include "waveform.h"

#include <fstream>
#include <memory>

#include <boost/algorithm/string/join.hpp>
#include <boost/filesystem.hpp>
#include <boost/functional/hash.hpp>

#include <seiscomp/core/recordsequence.h>
#include <seiscomp/core/strings.h>
#include <seiscomp/io/recordinput.h>
#include <seiscomp/io/records/mseedrecord.h>
#include <seiscomp/io/recordstream.h>
#include <seiscomp/math/filter.h>
#include <seiscomp/utils/files.h>

#include "log.h"
#include "resamplerstore.h"
#include "utils.h"

namespace Seiscomp {
namespace detect {
namespace waveform {

namespace {

template <class T> T NextPowerOfTwo(T a, T min = 1, T max = 1 << 31) {
  int b = min;
  while (b < a) {
    b <<= 1;
    if (b > max)
      return -1;
  }
  return b;
}

} // namespace

bool Trim(GenericRecord &trace, const Core::TimeWindow &tw) {
  auto offset{
      static_cast<int>(static_cast<double>(tw.startTime() - trace.startTime()) *
                       trace.samplingFrequency())};
  auto samples{static_cast<int>(tw.length() * trace.samplingFrequency())};

  // Not enough data at start of time window
  if (offset < 0) {
    SCDETECT_LOG_WARNING("%s: Need %d more samples in past.",
                         trace.streamID().c_str(), -offset);
    return false;
  }

  // Not enough data at end of time window
  if (offset + samples > trace.data()->size()) {
    SCDETECT_LOG_WARNING("%s: Need %d more samples past the end.",
                         trace.streamID().c_str(),
                         -(trace.data()->size() - samples - offset));
    return false;
  }

  trace.setStartTime(trace.startTime() +
                     Core::TimeSpan{offset / trace.samplingFrequency()});
  trace.setData(trace.data()->slice(offset, offset + samples));

  return true;
}

bool Filter(GenericRecord &trace, const std::string &filter_string) {
  if (filter_string.empty())
    return false;

  auto data{DoubleArray::Cast(trace.data())};
  if (!Filter(*data, filter_string, trace.samplingFrequency())) {
    return false;
  }
  trace.dataUpdated();

  return true;
}

bool Filter(DoubleArray &data, const std::string &filter_string,
            double sampling_freq) {
  if (filter_string.empty() || sampling_freq <= 0)
    return false;

  std::string filter_error;
  auto filter = Math::Filtering::InPlaceFilter<double>::Create(filter_string,
                                                               &filter_error);
  if (!filter) {
    SCDETECT_LOG_WARNING("Filter creation failed for '%s': %s",
                         filter_string.c_str(), filter_error.c_str());
    return false;
  }
  filter->setSamplingFrequency(sampling_freq);
  filter->apply(data.size(), data.typedData());
  delete filter;

  return true;
}

bool Resample(GenericRecord &trace, double target_frequency) {
  if (target_frequency <= 0 || trace.samplingFrequency() == target_frequency)
    return true;

  auto resampler{
      RecordResamplerStore::Instance().Get(&trace, target_frequency)};
  std::unique_ptr<Record> resampled;
  resampled.reset(resampler->feed(&trace));
  if (!resampled) {
    SCDETECT_LOG_WARNING("%s: Failed to resample record "
                         "(sampling_frequency=%f): target_frequency=%f",
                         std::string{trace.streamID()}.c_str(),
                         trace.samplingFrequency(), target_frequency);
    return false;
  }

  trace.setStartTime(resampled->startTime());
  trace.setSamplingFrequency(static_cast<double>(target_frequency));
  trace.setData(resampled->data()->copy(Array::DataType::DOUBLE));
  return true;
}

void Demean(GenericRecord &trace) {
  auto data{DoubleArray::Cast(trace.data())};
  Demean(*data);
  trace.dataUpdated();
}

void Demean(DoubleArray &data) {
  const auto mean{utils::CMA(data.typedData(), data.size())};
  data -= mean;
}

bool Write(const GenericRecord &trace, std::ostream &out) {
  IO::MSeedRecord rec{trace};
  int rec_length = rec.data()->size() * rec.data()->elementSize() + 64;
  rec_length = NextPowerOfTwo<int>(rec_length, 128,
                                   1048576); // MINRECLEN 128, MAXRECLEN 1048576
  if (rec_length <= 0)
    return false;

  try {
    rec.setOutputRecordLength(rec_length);
    rec.write(out);
  } catch (std::exception &e) {
    SCDETECT_LOG_WARNING("Failed writing waveform: %s", e.what());
    return false;
  }
  return true;
}

bool Read(GenericRecord &trace, std::istream &in) {
  IO::MSeedRecord rec(Array::DOUBLE, Record::Hint::DATA_ONLY);
  try {
    rec.read(in);

    trace = GenericRecord(rec);
    trace.setData(rec.data()->clone());
  } catch (std::exception &e) {
    SCDETECT_LOG_WARNING("Failed reading waveform: %s", e.what());
    return false;
  }
  return true;
}

} // namespace waveform

WaveformHandlerIface::BaseException::BaseException()
    : Exception{"base waveform handler exception"} {}

const double WaveformHandler::download_margin_{2};

void WaveformHandlerIface::Process(const GenericRecordPtr &trace,
                                   const ProcessingConfig &config,
                                   const Core::TimeWindow &tw_trim) const {

  if (config.demean) {
    waveform::Demean(*trace);
  }

  if (config.target_frequency) {
    waveform::Resample(*trace, config.target_frequency);
  }

  if (!config.filter_string.empty()) {
    if (!waveform::Filter(*trace, config.filter_string)) {
      throw BaseException{Core::stringify(
          "%s: Filtering failed with filter: filter=%s,"
          "start=%s, end=%s",
          trace->streamID().c_str(), config.filter_string.c_str(),
          trace->startTime().iso().c_str(), trace->endTime().iso().c_str())};
    }
  }

  if (tw_trim) {
    if (!waveform::Trim(*trace, tw_trim)) {
      throw BaseException{Core::stringify(
          "%s: Incomplete trace; not enough data for requested time:"
          "start=%s, end=%s",
          trace->streamID().c_str(), tw_trim.startTime().iso().c_str(),
          tw_trim.endTime().iso().c_str())};
    }
  }
}

WaveformHandler::NoData::NoData() : BaseException{"no data avaiable"} {}

WaveformHandler::WaveformHandler(const std::string &record_stream_url)
    : record_stream_url_(record_stream_url) {}

GenericRecordCPtr WaveformHandler::Get(const DataModel::WaveformStreamID &id,
                                       const Core::TimeWindow &tw,
                                       const ProcessingConfig &config) {
  return Get(id.networkCode(), id.stationCode(), id.locationCode(),
             id.channelCode(), tw, config);
}

GenericRecordCPtr WaveformHandler::Get(const DataModel::WaveformStreamID &id,
                                       const Core::Time &start,
                                       const Core::Time &end,
                                       const ProcessingConfig &config) {
  Core::TimeWindow tw{start, end};
  return Get(id.networkCode(), id.stationCode(), id.locationCode(),
             id.channelCode(), tw, config);
}

GenericRecordCPtr
WaveformHandler::Get(const std::string &net_code, const std::string &sta_code,
                     const std::string &loc_code, const std::string &cha_code,
                     const Core::Time &start, const Core::Time &end,
                     const ProcessingConfig &config) {
  Core::TimeWindow tw{start, end};
  return Get(net_code, sta_code, loc_code, cha_code, tw, config);
}

GenericRecordCPtr WaveformHandler::Get(const std::string &net_code,
                                       const std::string &sta_code,
                                       const std::string &loc_code,
                                       const std::string &cha_code,
                                       const Core::TimeWindow &tw,
                                       const ProcessingConfig &config) {

  utils::WaveformStreamID wf_stream_id{net_code, sta_code, loc_code, cha_code};
  if (!wf_stream_id.IsValid()) {
    throw BaseException{"Invalid waveform stream identifier."};
  }

  IO::RecordStreamPtr rs = IO::RecordStream::Open(record_stream_url_.c_str());
  if (!rs) {
    throw BaseException{
        std::string{"Failed to open RecordStream: " + record_stream_url_}};
  }

  Core::TimeSpan download_margin{download_margin_};
  Core::TimeWindow tw_with_margin{tw.startTime() - download_margin,
                                  tw.endTime() + download_margin};
  if (!config.filter_string.empty()) {
    Core::TimeSpan margin{config.filter_margin_time};
    tw_with_margin.setStartTime(tw_with_margin.startTime() - margin);
    tw_with_margin.setEndTime(tw_with_margin.endTime() + margin);
  }

  rs->setTimeWindow(tw_with_margin);
  rs->addStream(net_code, sta_code, loc_code, cha_code);

  IO::RecordInput inp{rs.get(), Array::DOUBLE, Record::DATA_ONLY};
  std::unique_ptr<RecordSequence> seq{
      utils::make_unique<TimeWindowBuffer>(tw_with_margin)};
  RecordPtr rec;
  while ((rec = inp.next())) {
    seq->feed(rec.get());
  }
  rs->close();

  if (seq->empty()) {
    throw NoData{Core::stringify(
        "%s.%s.%s.%s: No data: start=%s, end=%s", net_code.c_str(),
        sta_code.c_str(), loc_code.c_str(), cha_code.c_str(),
        tw.startTime().iso().c_str(), tw.endTime().iso().c_str())};
  }

  GenericRecordPtr trace{seq->contiguousRecord<double>()};
  if (!trace) {
    throw BaseException{Core::stringify(
        "%s.%s.%s.%s: Failed to merge records into single trace: start=%s, "
        "end=%s",
        net_code.c_str(), sta_code.c_str(), loc_code.c_str(), cha_code.c_str(),
        tw.startTime().iso().c_str(), tw.endTime().iso().c_str())};
  }

  Process(trace, config, tw);
  return trace;
}

const std::string Cached::cache_key_sep_{"."};

Cached::Cached(WaveformHandlerIfacePtr waveform_handler, bool raw)
    : waveform_handler_(waveform_handler), raw_(raw) {}

GenericRecordCPtr
Cached::Get(const DataModel::WaveformStreamID &id, const Core::TimeWindow &tw,
            const WaveformHandlerIface::ProcessingConfig &config) {
  return Get(id.networkCode(), id.stationCode(), id.locationCode(),
             id.channelCode(), tw, config);
}

GenericRecordCPtr
Cached::Get(const DataModel::WaveformStreamID &id, const Core::Time &start,
            const Core::Time &end,
            const WaveformHandlerIface::ProcessingConfig &config) {
  Core::TimeWindow tw{start, end};
  return Get(id.networkCode(), id.stationCode(), id.locationCode(),
             id.channelCode(), tw, config);
}

GenericRecordCPtr
Cached::Get(const std::string &net_code, const std::string &sta_code,
            const std::string &loc_code, const std::string &cha_code,
            const Core::Time &start, const Core::Time &end,
            const WaveformHandlerIface::ProcessingConfig &config) {
  Core::TimeWindow tw{start, end};
  return Get(net_code, sta_code, loc_code, cha_code, tw, config);
}

GenericRecordCPtr
Cached::Get(const std::string &net_code, const std::string &sta_code,
            const std::string &loc_code, const std::string &cha_code,
            const Core::TimeWindow &tw,
            const WaveformHandlerIface::ProcessingConfig &config) {

  auto SetCache = [&](const std::string &cache_key,
                      GenericRecordCPtr trace) -> bool {
    if (!Set(cache_key, trace)) {
      SCDETECT_LOG_DEBUG("Failed to cache trace for key: %s",
                         cache_key.c_str());
      return false;
    }
    return true;
  };

  utils::WaveformStreamID wf_stream_id{net_code, sta_code, loc_code, cha_code};
  if (!wf_stream_id.IsValid()) {
    throw BaseException{"Invalid waveform stream identifier."};
  }

  std::string cache_key;
  MakeCacheKey(net_code, sta_code, loc_code, cha_code, tw, config, cache_key);

  bool cached = true;
  GenericRecordCPtr trace{Get(cache_key)};
  if (!trace) {
    cached = false;

    ProcessingConfig disabled{config};
    disabled.filter_string = "";
    disabled.target_frequency = 0;
    disabled.demean = false;

    Core::TimeWindow corrected{tw};
    if (!config.filter_string.empty()) {
      const Core::TimeSpan margin{config.filter_margin_time};
      corrected.setStartTime(tw.startTime() - margin);
      corrected.setEndTime(tw.endTime() + margin);
    }
    trace = waveform_handler_->Get(net_code, sta_code, loc_code, cha_code,
                                   corrected, disabled);
  }

  // cache the raw data
  if (!cached && !CacheProcessed()) {
    SetCache(cache_key, trace);
    // TODO (damb): Find a better solution! -> Ideally,
    // `WaveformHandlerIface::Get()` would return a pointer of type
    // `GenericRecordPtr` i.e. a non-const pointer.

    // make sure we do not modified the data cached i.e. create a copy
    trace = utils::make_smart<const GenericRecord>(*trace);
  }

  Process(const_cast<GenericRecord *>(trace.get()), config, tw);

  // cache processed data
  if (!cached && CacheProcessed()) {
    SetCache(cache_key, trace);
  }

  return trace;
}

void Cached::MakeCacheKey(const std::string &net_code,
                          const std::string &sta_code,
                          const std::string &loc_code,
                          const std::string &cha_code,
                          const Core::TimeWindow &tw,
                          const WaveformHandlerIface::ProcessingConfig &config,
                          std::string &result) const {

  Core::TimeWindow tw_with_margin{tw};
  if (!CacheProcessed()) {
    if (!config.filter_string.empty()) {
      Core::TimeSpan margin{config.filter_margin_time};
      tw_with_margin.setStartTime(tw.startTime() - margin);
      tw_with_margin.setEndTime(tw.endTime() + margin);
    }
  }

  std::vector<std::string> key_components{net_code,
                                          sta_code,
                                          loc_code,
                                          cha_code,
                                          tw_with_margin.startTime().iso(),
                                          tw_with_margin.endTime().iso()};

  if (CacheProcessed()) {
    key_components.push_back(
        std::to_string(std::hash<ProcessingConfig>{}(config)));
  }

  MakeCacheKey(key_components, result);
}

void Cached::MakeCacheKey(std::vector<std::string> key_components,
                          std::string &result) const {
  result = boost::algorithm::join(key_components, cache_key_sep_);
}

FileSystemCache::FileSystemCache(WaveformHandlerIfacePtr waveform_handler,
                                 const std::string &path, bool raw)
    : Cached(waveform_handler, raw), path_cache_(path) {}

GenericRecordCPtr FileSystemCache::Get(const std::string &key) {
  std::string fpath{(boost::filesystem::path(path_cache_) / key).string()};
  if (!Util::fileExists(fpath))
    return nullptr;

  std::ifstream ifs{fpath};
  auto trace{utils::make_smart<GenericRecord>()};
  if (!waveform::Read(*trace, ifs))
    return nullptr;

  return trace;
}

bool Cached::CacheProcessed() const { return !raw_; }

bool FileSystemCache::Set(const std::string &key, GenericRecordCPtr value) {
  if (!value)
    return false;

  std::string fpath{(boost::filesystem::path(path_cache_) / key).string()};
  std::ofstream ofs(fpath);
  if (!waveform::Write(*value, ofs)) {
    SCDETECT_LOG_DEBUG("Failed to set cache for file: %s", fpath.c_str());
    return false;
  }
  return true;
}

bool FileSystemCache::Exists(const std::string &key) {
  std::string fpath{(boost::filesystem::path(path_cache_) / key).string()};
  return Util::fileExists(fpath);
}

InMemoryCache::InMemoryCache(WaveformHandlerIfacePtr waveform_handler, bool raw)
    : Cached(waveform_handler, raw) {}

GenericRecordCPtr InMemoryCache::Get(const std::string &key) {
  const auto it = cache_.find(key);
  if (cache_.end() == it)
    return nullptr;
  return it->second;
}

bool InMemoryCache::Set(const std::string &key, GenericRecordCPtr value) {
  cache_[key] = value;
  return true;
}

bool InMemoryCache::Exists(const std::string &key) {
  return cache_.find(key) != cache_.end();
}

} // namespace detect
} // namespace Seiscomp

namespace std {

inline std::size_t
hash<Seiscomp::detect::WaveformHandlerIface::ProcessingConfig>::operator()(
    const Seiscomp::detect::WaveformHandlerIface::ProcessingConfig &c)
    const noexcept {
  std::size_t ret{0};
  boost::hash_combine(ret, std::hash<std::string>{}(c.filter_string));
  boost::hash_combine(ret, std::hash<double>{}(c.filter_margin_time));
  boost::hash_combine(ret, std::hash<double>{}(c.target_frequency));
  boost::hash_combine(ret, std::hash<bool>{}(c.demean));
  return ret;
}

} // namespace std
