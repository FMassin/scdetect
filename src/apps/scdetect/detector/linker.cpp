#include "linker.h"

#include <boost/functional/hash.hpp>
#include <iterator>
#include <unordered_set>

#include "../utils.h"

namespace Seiscomp {
namespace detect {
namespace detector {

Linker::Linker(const Core::TimeSpan &onHold, double arrivalOffsetThres)
    : _thresArrivalOffset{arrivalOffsetThres}, _onHold{onHold} {}

Linker::~Linker() {}

void Linker::setThresArrivalOffset(const boost::optional<double> &thres) {
  _thresArrivalOffset = thres;
}

boost::optional<double> Linker::thresArrivalOffset() const {
  return _thresArrivalOffset;
}

void Linker::setThresResult(const boost::optional<double> &thres) {
  _thresResult = thres;
}

boost::optional<double> Linker::thresResult() const { return _thresResult; }

void Linker::setMinArrivals(const boost::optional<size_t> &n) {
  auto v{n};
  if (v && 1 > *v) {
    v = boost::none;
  }

  _minArrivals = v;
}

boost::optional<size_t> Linker::minArrivals() const { return _minArrivals; }

void Linker::setOnHold(const Core::TimeSpan &duration) { _onHold = duration; }

Core::TimeSpan Linker::onHold() const { return _onHold; }

Linker::Status Linker::status() const { return _status; }

size_t Linker::getAssociatedChannelCount() const {
  std::unordered_set<std::string> wfIds;
  for (const auto &procPair : _processors) {
    wfIds.emplace(procPair.second.arrival.pick.waveformStreamId);
  }

  return wfIds.size();
}

size_t Linker::getProcessorCount() const { return _processors.size(); }

void Linker::add(const TemplateWaveformProcessor *proc,
                 const Arrival &arrival) {
  if (proc) {
    _processors.emplace(proc->id(), Processor{proc, arrival});
    _potValid = false;
  }
}

void Linker::remove(const std::string &procId) {
  _processors.erase(procId);
  _potValid = false;
}

void Linker::reset() {
  _queue.clear();
  _potValid = false;

  _status = Status::kWaitingForData;
}

void Linker::terminate() {
  // flush pending events
  while (!_queue.empty()) {
    const auto event{_queue.front()};
    if (event.getArrivalCount() >= _minArrivals.value_or(getProcessorCount()) &&
        (!_thresResult || event.result.fit >= *_thresResult)) {
      emitResult(event.result);
    }

    _queue.pop_front();
  }
  _status = Status::kTerminated;
}

void Linker::feed(const TemplateWaveformProcessor *proc,
                  const TemplateWaveformProcessor::MatchResultCPtr &res) {
  if (!proc || !res) {
    return;
  }

  if (status() < Status::kTerminated) {
    auto it{_processors.find(proc->id())};
    if (it == _processors.end()) {
      return;
    }

    auto &linkerProc{it->second};
    // create a new arrival from a *template arrival*
    auto newArrival{linkerProc.arrival};
    const auto templateStartTime{linkerProc.proc->templateStartTime()};
    if (templateStartTime) {
      // XXX(damb): recompute the pickOffset; the template proc might have
      // changed the underlying template waveform (due to resampling)
      const auto pickOffset{linkerProc.arrival.pick.time - *templateStartTime};
      const auto time{res->timeWindow.startTime() + Core::TimeSpan{res->lag} +
                      pickOffset};
      newArrival.pick.time = time;

      process(proc, Result::TemplateResult{newArrival, res});
    }
  }
}

void Linker::setResultCallback(const PublishResultCallback &callback) {
  _resultCallback = callback;
}

void Linker::process(const TemplateWaveformProcessor *proc,
                     const Result::TemplateResult &res) {
  if (!_processors.empty()) {
    // update POT
    if (!_potValid) {
      createPot();
    }
    _pot.enable();

    const auto &procId{proc->id()};
    const auto &matchResult{res.matchResult};
    // merge result into existing events
    for (auto eventIt = std::begin(_queue); eventIt != std::end(_queue);
         ++eventIt) {
      if (eventIt->getArrivalCount() < getProcessorCount()) {
        auto &templResults{eventIt->result.results};
        auto it{templResults.find(procId)};
        if (it == templResults.end() ||
            matchResult->coefficient > it->second.matchResult->coefficient) {
          std::vector<Arrival> arrivals{res.arrival};
          std::unordered_set<std::string> wfIds;
          for (const auto &templResultPair : templResults) {
            const auto &a{templResultPair.second.arrival};
            arrivals.push_back(a);
            wfIds.emplace(a.pick.waveformStreamId);
          }

          POT pot{arrivals};

          if (_thresArrivalOffset) {
            // prepare reference POT
            _pot.disable(wfIds);

            std::unordered_set<std::string> exceeded;
            if (!validatePickOffsets(_pot, pot, exceeded,
                                     *_thresArrivalOffset) ||
                !exceeded.empty()) {
              continue;
            }
          }

          eventIt->mergeResult(procId, res, pot);
        }
        _pot.enable();
      }
    }

    const auto now{Core::Time::GMT()};
    // create new event
    Event event{now + _onHold};
    event.mergeResult(procId, res, POT{std::vector<Arrival>{res.arrival}});
    _queue.emplace_back(event);

    std::vector<EventQueue::iterator> ready;
    for (auto it = std::begin(_queue); it != std::end(_queue); ++it) {
      const auto arrivalCount{it->getArrivalCount()};
      // emit results which are ready and surpass threshold
      if (arrivalCount == getProcessorCount() ||
          (now >= it->expired &&
           arrivalCount >= _minArrivals.value_or(getProcessorCount()))) {
        if (!_thresResult || it->result.fit >= *_thresResult) {
          emitResult(it->result);
        }
        ready.push_back(it);
      }
      // drop expired result
      else if (now >= it->expired) {
        ready.push_back(it);
      }
    }

    // clean up result queue
    for (auto &it : ready) {
      _queue.erase(it);
    }
  }
}

void Linker::emitResult(const Result &res) {
  if (_resultCallback) {
    _resultCallback.value()(res);
  }
}

void Linker::createPot() {
  std::vector<Arrival> arrivals;
  using pair_type = Processors::value_type;
  std::transform(_processors.cbegin(), _processors.cend(),
                 back_inserter(arrivals),
                 [](const pair_type &p) { return p.second.arrival; });

  // XXX(damb): The current implementation simply recreates the POT
  _pot = POT(arrivals);
  _potValid = true;
}

/* ------------------------------------------------------------------------- */
size_t Linker::Result::getArrivalCount() const { return results.size(); }

std::string Linker::Result::debugString() const {
  const Core::Time startTime{
      results.at(refProcId).matchResult->timeWindow.startTime()};
  const Core::Time endTime{startTime +
                           Core::TimeSpan{pot.pickOffset().value_or(0)}};
  return std::string{"(" + startTime.iso() + " - " + endTime.iso() +
                     "): fit=" + std::to_string(fit) +
                     ", arrival_count=" + std::to_string(getArrivalCount())};
}

/* ------------------------------------------------------------------------- */
void Linker::Event::mergeResult(const std::string &procId,
                                const Result::TemplateResult &res,
                                const POT &pot) {
  auto &templResults{result.results};
  templResults.emplace(procId, res);

  std::vector<double> fits;
  std::transform(std::begin(templResults), std::end(templResults),
                 std::back_inserter(fits),
                 [](const Result::TemplateResults::value_type &p) {
                   return p.second.matchResult->coefficient;
                 });

  // XXX(damb): Currently, we use the mean in order to compute the overall
  // event's score
  result.fit = utils::cma(fits.data(), fits.size());
  result.pot = pot;
  if (!refPickTime || res.arrival.pick.time < refPickTime) {
    refPickTime = res.arrival.pick.time;
    result.refProcId = procId;
  }
}

size_t Linker::Event::getArrivalCount() const { return result.results.size(); }

}  // namespace detector
}  // namespace detect
}  // namespace Seiscomp

namespace std {

inline std::size_t
hash<Seiscomp::detect::detector::Linker::Result::TemplateResult>::operator()(
    const Seiscomp::detect::detector::Linker::Result::TemplateResult &tr)
    const noexcept {
  std::size_t ret{0};
  boost::hash_combine(
      ret, std::hash<Seiscomp::detect::detector::Arrival>{}(tr.arrival));

  if (tr.matchResult) {
    boost::hash_combine(ret, std::hash<double>{}(tr.matchResult->coefficient));
  }

  return ret;
}

}  // namespace std
