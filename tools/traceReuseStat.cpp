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
#include <vector>

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

double percentile(std::vector<double> &v, double p) {
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  size_t i = (size_t)std::min<double>((double)v.size() - 1,
                                      p * (double)v.size());
  return v[i];
}

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

  int64_t n_req = 0, distinct = 0;
  double accesses = 0, first = 0, same_sess = 0, cross_sess = 0, cross_user = 0;
  double cross_gap_sum = 0.0;
  bool users_present = false;
  std::vector<double> cross_gaps, same_gaps;

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
    obj_id_t largest = 0;
    for (obj_id_t id : r.blocks) largest = std::max(largest, id);
    if ((size_t)largest >= last_session.size()) {
      size_t want = (size_t)largest * 2 + 2;
      last_session.resize(want, 0);
      last_user.resize(want, 0);
      last_time.resize(want, 0.0);
      seen.resize(want, 0);
    }

    double r_first = 0, r_same = 0, r_cross = 0;
    for (obj_id_t id : r.blocks) {
      accesses += 1;
      if (!seen[id]) {
        seen[id] = 1;
        distinct += 1;
        first += 1;
        r_first += 1;
      } else if (last_session[id] == r.session) {
        same_sess += 1;
        r_same += 1;
        same_gaps.push_back(r.timestamp - last_time[id]);
      } else {
        cross_sess += 1;
        r_cross += 1;
        double gap = r.timestamp - last_time[id];
        cross_gap_sum += gap > 0 ? gap : 0.0;
        cross_gaps.push_back(gap);
        if (r.user != 0 && last_user[id] != 0 && last_user[id] != r.user) {
          cross_user += 1;
        }
      }
      last_session[id] = r.session;
      last_user[id] = r.user;
      last_time[id] = r.timestamp;
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
  std::vector<double> cg = cross_gaps, sg = same_gaps;

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
  printf("cross_session_gap_p50_s=%.1f\n", percentile(cg, 0.50));
  printf("cross_session_gap_p90_s=%.1f\n", percentile(cg, 0.90));
  printf("same_session_gap_p50_s=%.1f\n", percentile(sg, 0.50));
  printf("reuse_cross_user=%.0f reuse_cross_user_share=%.4f\n", cross_user,
         accesses > 0 ? cross_user / accesses : 0.0);

  if (a.gap_hist) {
    const int NB = 24;  // log2 buckets of cross-session reuse gaps, seconds
    std::vector<double> hist(NB, 0.0);
    for (double g : cross_gaps) {
      int b = g <= 0 ? 0 : (int)std::floor(std::log2(g)) + 1;
      if (b >= NB) b = NB - 1;
      hist[b] += 1;
    }
    for (int i = 0; i < NB; i++) {
      printf("gap_hist_le_%.0fs=%.0f\n", std::pow(2.0, i), hist[i]);
    }
  }

  if (tab) fclose(tab);
  return 0;
}
