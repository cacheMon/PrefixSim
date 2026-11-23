// Fixed-memory, deterministic reuse-gap quantiles.
// Exact for at most 4096 observations; for larger traces returns a bucket
// enclosure and midpoint with <=1/64 relative bin width on positive gaps.
// Unlike retaining all gaps, memory is independent of number of accesses.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace prefixsim {

class GapQuantiles {
 public:
  struct Estimate {
    double value = 0.0;
    double lower = 0.0;
    double upper = 0.0;
    bool exact = true;
  };

  static constexpr size_t kExactLimit = 4096;
  static constexpr int kPartsPerOctave = 64;
  static constexpr int kMinExponent = -1074;
  static constexpr int kMaxExponent = 1024;
  static constexpr int kBins = (kMaxExponent-kMinExponent+1)*kPartsPerOctave;
  static constexpr int kCoarseBins = 24;

  GapQuantiles() : buckets_(kBins, 0), coarse_(kCoarseBins, 0) {
    exact_.reserve(kExactLimit);
  }

  bool add(double gap) {
    // Timestamps should be nondecreasing. Do not silently turn malformed
    // negative/NaN gaps into valid quantiles.
    if (!std::isfinite(gap) || gap < 0.0 || total_ == std::numeric_limits<uint64_t>::max())
      return false;
    ++total_;
    if (exact_.size() < kExactLimit) exact_.push_back(gap);
    if (gap == 0.0) {
      ++zero_;
      ++coarse_[0];
      return true;
    }
    int exp = 0;
    const double frac = std::frexp(gap, &exp); // [0.5,1), gap=frac*2^exp
    if (exp < kMinExponent || exp > kMaxExponent) return false;
    int sub = static_cast<int>((frac*2.0-1.0)*kPartsPerOctave);
    sub = std::max(0, std::min(kPartsPerOctave-1, sub));
    const size_t index = static_cast<size_t>(exp-kMinExponent)*kPartsPerOctave+
                         static_cast<size_t>(sub);
    ++buckets_[index];
    const int coarse = std::max(0,std::min(kCoarseBins-1,exp));
    ++coarse_[coarse];
    return true;
  }

  Estimate percentile(double p) const {
    if (total_ == 0 || !std::isfinite(p) || p < 0.0 || p > 1.0) return {};
    if (total_ <= kExactLimit) {
      std::vector<double> vals = exact_;
      std::sort(vals.begin(),vals.end());
      const size_t rank = static_cast<size_t>(p*static_cast<double>(vals.size()-1));
      return {vals[rank],vals[rank],vals[rank],true};
    }
    uint64_t rank = static_cast<uint64_t>(p*static_cast<double>(total_-1));
    if (rank < zero_) return {0.0,0.0,0.0,false};
    rank -= zero_;
    for (size_t b = 0; b < buckets_.size(); ++b) {
      if (rank < buckets_[b]) {
        const int exp = kMinExponent + static_cast<int>(b/kPartsPerOctave);
        const int sub = static_cast<int>(b%kPartsPerOctave);
        const double lower = std::ldexp(1.0+static_cast<double>(sub)/kPartsPerOctave,exp-1);
        const double upper = std::min(std::numeric_limits<double>::max(),
            std::ldexp(1.0+static_cast<double>(sub+1)/kPartsPerOctave,exp-1));
        return {lower+(upper-lower)/2.0,lower,upper,false};
      }
      rank -= buckets_[b];
    }
    return {}; // defensive; unreachable for valid floating-point gaps
  }

  uint64_t total() const {return total_;}
  uint64_t coarse_count(int i) const {return coarse_.at(static_cast<size_t>(i));}
  size_t fixed_bucket_bytes() const {return (buckets_.size()+coarse_.size())*sizeof(uint64_t);}

 private:
  std::vector<uint64_t> buckets_;
  std::vector<uint64_t> coarse_;
  std::vector<double> exact_;
  uint64_t total_ = 0;
  uint64_t zero_ = 0;
};

} // namespace prefixsim
