#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <limits>
#include "../tools/gap_quantiles.hpp"

static void check(bool ok) {
  if (!ok) {std::fputs("GapQuantiles regression failure\n",stderr);std::abort();}
}

int main() {
  using prefixsim::GapQuantiles;
  GapQuantiles g;
  check(g.total() == 0 && g.percentile(.5).value == 0);
  for (double x: {0.0,0.1,1.0,2.0,3.0}) check(g.add(x));
  check(g.percentile(.5).exact && g.percentile(.5).value == 1.0);
  check(g.percentile(.9).value == 2.0);
  check(g.coarse_count(0) == 2);
  check(g.coarse_count(1) == 1);
  check(g.coarse_count(2) == 2);
  check(g.coarse_count(3) == 0);
  check(!g.add(-0.1));
  check(!g.add(std::numeric_limits<double>::quiet_NaN()));
  check(!g.add(std::numeric_limits<double>::infinity()));
  check(g.total() == 5);

  GapQuantiles many;
  for (int i=0;i<2000000;++i) check(many.add(.05+(i%1000)*.01));
  check(many.total() == 2000000);
  check(many.fixed_bucket_bytes() < 2*1024*1024);
  const auto p50=many.percentile(.5),p90=many.percentile(.9);
  check(!p50.exact && !p90.exact);
  check(p50.lower <= 5.04 && 5.04 <= p50.upper); // exact median
  check(p90.lower <= 9.04 && 9.04 <= p90.upper); // exact 90th
  check(p50.upper <= 1.016*p50.lower);
  check(p90.upper <= 1.016*p90.lower);
  const double small=std::numeric_limits<double>::denorm_min();
  GapQuantiles tiny;
  for(int i=0;i<5000;++i) check(tiny.add(small));
  auto q=tiny.percentile(.5);
  check(q.lower<=small && small<=q.upper);
  return 0;
}
