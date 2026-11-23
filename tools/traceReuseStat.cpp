// traceReuseStat -- how much of a trace's block reuse crosses session boundaries.
//
// This is the statistic that decides whether victim choice can matter at all.
// An eviction policy spends its budget on blocks that some request stream will
// come back for later. When almost every reuse stays inside a single session,
// recency already captures it and a frequency/admission policy has nothing left
// to buy; when a large share of reuse crosses sessions, a policy that protects a
// one-hit block can win. Measured on the freeinference trace (sigma_cross =
// 0.16) the SIEVE-LRU gap is +0.014%; measured on a cross-session conversation
// workload (sigma_cross = 0.995) the same pair differs by +24.6%. See
// doc/reuse_locality.md.
//
// Usage:
//   traceReuseStat <trace> [--block-id prefix|raw] [--format lcp|qwen-jsonl]
//                            [--gap-hist] [--per-request-out FILE]
//
// Output is stable key=value lines so runs can be diffed across traces.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <limits>
#include <vector>
#include "gap_quantiles.hpp"

#include "trace.hpp"

namespace {

struct Args {
  std::string trace;
  prefixsim::BlockIdMode block_id = prefixsim::BlockIdMode::kPrefix;
  bool have_format = false;
  prefixsim::TraceFormat format = prefixsim::TraceFormat::kLcp;
  bool gap_hist = false;
  std::string per_request_out;
};

void usage(const char *argv0) {
  fprintf(stderr,
          "usage: %s <trace> [--block-id prefix|raw] [--format lcp|qwen-jsonl]\n"
          "          [--gap-hist] [--per-request-out FILE]\n",
          argv0);
}

struct RawState {
  uint64_t session = 0;
  uint64_t user = 0;
  double time = 0.0;
  uint8_t seen = 0;
};

}  // namespace

int main(int argc, char **argv) {
  Args a;
  if (argc < 2) {
    usage(argv[0]);
    return 2;
  }
  a.trace = argv[1];
  for (int i = 2; i < argc; i++) {
    if (!strcmp(argv[i], "--block-id") && i + 1 < argc) {
      a.block_id = !strcmp(argv[++i], "raw") ? prefixsim::BlockIdMode::kRaw
                                             : prefixsim::BlockIdMode::kPrefix;
    } else if (!strcmp(argv[i], "--format") && i + 1 < argc) {
      std::string f = argv[++i];
      if (!prefixsim::parse_trace_format(f, a.format)) {
        fprintf(stderr, "unknown format %s\n", f.c_str());
        return 2;
      }
      a.have_format = true;
    } else if (!strcmp(argv[i], "--gap-hist")) {
      a.gap_hist = true;
    } else if (!strcmp(argv[i], "--per-request-out") && i + 1 < argc) {
      a.per_request_out = argv[++i];
    } else {
      usage(argv[0]);
      return 2;
    }
  }

  std::string error;
  prefixsim::TraceFormat fmt =
      a.have_format ? a.format : prefixsim::guess_trace_format(a.trace);
  auto reader = prefixsim::open_trace(a.trace, fmt, a.block_id, error);
  if (!reader) {
    fprintf(stderr, "open %s: %s\n", a.trace.c_str(), error.c_str());
    return 1;
  }

  // Per-id state, grown on demand: ids are dense (1, 2, 3, ...) in prefix mode,
  // which is the mode every simulator run uses.
  std::vector<uint64_t> last_session(1, 0), last_user(1, 0);
  std::vector<double> last_time(1, 0.0);
  std::vector<uint8_t> seen(1, 0);
  std::unordered_map<obj_id_t, RawState> raw_state;

  int64_t n_req = 0, distinct = 0;
  double accesses = 0, first = 0, same_sess = 0, cross_sess = 0, cross_user = 0;
  double cross_gap_sum = 0.0;
  bool users_present = false;
  prefixsim::GapQuantiles cross_gaps, same_gaps;

  FILE *tab = nullptr;
  if (!a.per_request_out.empty()) {
    tab = fopen(a.per_request_out.c_str(), "w");
    if (!tab) {
      fprintf(stderr, "cannot write %s\n", a.per_request_out.c_str());
      return 1;
    }
    fprintf(tab,
            "req\ttime_us\tsession\tuser\tn_blocks\tfirst\tsame_sess\t"
            "cross_sess\treuse_share\n");
  }

  prefixsim::Request r;
  while (reader->next(r)) {
    n_req += 1;
    if (r.user != 0) users_present = true;
    if (a.block_id == prefixsim::BlockIdMode::kPrefix) {
      obj_id_t largest = 0;
      for (obj_id_t id : r.blocks) largest = std::max(largest, id);
      if ((size_t)largest >= last_session.size()) {
        if ((size_t)largest > (std::numeric_limits<size_t>::max()-2)/2) {
          fprintf(stderr, "prefix-id allocation would overflow size_t\n");
          if (tab) fclose(tab);
          return 1;
        }
        size_t want = (size_t)largest * 2 + 2;
        last_session.resize(want, 0);
        last_user.resize(want, 0);
        last_time.resize(want, 0.0);
        seen.resize(want, 0);
      }
    }

    double r_first = 0, r_same = 0, r_cross = 0;
    for (obj_id_t id : r.blocks) {
      uint8_t *was_seen;
      uint64_t *prior_session, *prior_user;
      double *prior_time;
      if (a.block_id == prefixsim::BlockIdMode::kPrefix) {
        was_seen = &seen[id];
        prior_session = &last_session[id];
        prior_user = &last_user[id];
        prior_time = &last_time[id];
      } else {
        RawState &state = raw_state[id];
        was_seen = &state.seen;
        prior_session = &state.session;
        prior_user = &state.user;
        prior_time = &state.time;
      }
      accesses += 1;
      if (!*was_seen) {
        *was_seen = 1;
        distinct += 1;
        first += 1;
        r_first += 1;
      } else if (*prior_session == r.session) {
        same_sess += 1;
        r_same += 1;
        if (!same_gaps.add(r.timestamp - *prior_time)) {
          fprintf(stderr, "nonmonotone/nonfinite same-session gap\n");
          if (tab) fclose(tab);
          return 1;
        }
      } else {
        cross_sess += 1;
        r_cross += 1;
        const double gap = r.timestamp - *prior_time;
        if (!cross_gaps.add(gap)) {
          fprintf(stderr, "nonmonotone/nonfinite cross-session gap\n");
          if (tab) fclose(tab);
          return 1;
        }
        cross_gap_sum += gap;
        if (r.user != 0 && *prior_user != 0 && *prior_user != r.user)
          cross_user += 1;
      }
      *prior_session = r.session;
      *prior_user = r.user;
      *prior_time = r.timestamp;
    }
    if (tab) {
      const double nb = (double)r.blocks.size();
      fprintf(tab, "%lld\t%.0f\t%llu\t%llu\t%.0f\t%.0f\t%.0f\t%.0f\t%.4f\n",
              (long long)r.index, r.timestamp * 1e6,
              (unsigned long long)r.session, (unsigned long long)r.user, nb,
              r_first, r_same, r_cross, nb > 0 ? (r_same + r_cross) / nb : 0.0);
    }
  }

  const double reuse = same_sess + cross_sess;
  const double sigma_cross = reuse > 0 ? cross_sess / reuse : 0.0;
  const auto cross50 = cross_gaps.percentile(0.50);
  const auto cross90 = cross_gaps.percentile(0.90);
  const auto same50 = same_gaps.percentile(0.50);

  printf("trace=%s\n", a.trace.c_str());
  printf("format=%s block_id=%s\n", reader->format_name(),
         a.block_id == prefixsim::BlockIdMode::kPrefix ? "prefix" : "raw");
  printf("requests=%lld\n", (long long)n_req);
  printf("block_accesses=%.0f\n", accesses);
  printf("distinct_blocks=%lld\n", (long long)distinct);
  printf("first_touch=%.0f first_touch_share=%.4f\n", first,
         accesses > 0 ? first / accesses : 0.0);
  printf("reuse=%.0f reuse_share=%.4f\n", reuse,
         accesses > 0 ? reuse / accesses : 0.0);
  printf("reuse_same_session=%.0f reuse_same_session_share=%.4f\n", same_sess,
         reuse > 0 ? same_sess / reuse : 0.0);
  printf("reuse_cross_session=%.0f reuse_cross_session_share=%.4f\n",
         cross_sess, reuse > 0 ? cross_sess / reuse : 0.0);
  printf("sigma_cross=%.4f\n", sigma_cross);
  printf("cross_session_gap_mean_s=%.1f\n",
         cross_sess > 0 ? cross_gap_sum / cross_sess : 0.0);
  printf("cross_session_gap_p50_s=%.1f\n", cross50.value);
  printf("cross_session_gap_p90_s=%.1f\n", cross90.value);
  printf("same_session_gap_p50_s=%.1f\n", same50.value);
  printf("gap_quantile_method=%s\n",
         cross50.exact && cross90.exact && same50.exact
             ? "exact" : "bounded_linear_octave_64");
  printf("cross_gap_p50_interval_s=%.8g,%.8g\n",cross50.lower,cross50.upper);
  printf("cross_gap_p90_interval_s=%.8g,%.8g\n",cross90.lower,cross90.upper);
  printf("same_gap_p50_interval_s=%.8g,%.8g\n",same50.lower,same50.upper);
  printf("reuse_cross_user=%.0f reuse_cross_user_share=%.4f\n", cross_user,
         accesses > 0 ? cross_user / accesses : 0.0);

  if (a.gap_hist) {
    const int NB = 24;  // log2 buckets of cross-session reuse gaps, seconds
    for (int i = 0; i < NB; i++) {
      printf("gap_hist_le_%.0fs=%.0f\n",
             std::pow(2.0, i), (double)cross_gaps.coarse_count(i));
    }
  }

  if (tab) fclose(tab);
  return 0;
}
