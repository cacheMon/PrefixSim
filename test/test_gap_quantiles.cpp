#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include "../tools/gap_quantiles.hpp"

int main() {
  using prefixsim::GapQuantiles;
  GapQuantiles g;
  assert(g.total() == 0 && g.percentile(.5).value == 0);
  for (double x: {0.0,0.1,1.0,2.0,3.0}) assert(g.add(x));
  assert(g.percentile(.5).exact && g.percentile(.5).value == 1.0);
  assert(g.percentile(.9).value == 2.0);
  assert(g.coarse_count(0) == 2);
  assert(g.coarse_count(1) == 1);
  assert(g.coarse_count(2) == 2);
  assert(g.coarse_count(3) == 0);
  assert(!g.add(-0.1));
  assert(!g.add(std::numeric_limits<double>::quiet_NaN()));
  assert(!g.add(std::numeric_limits<double>::infinity()));
  assert(g.total() == 5);

  GapQuantiles many;
  for (int i=0;i<2000000;++i) assert(many.add(.05+(i%1000)*.01));
  assert(many.total() == 2000000);
  assert(many.fixed_bucket_bytes() < 2*1024*1024);
  const auto p50=many.percentile(.5),p90=many.percentile(.9);
  assert(!p50.exact && !p90.exact);
  assert(p50.lower <= 5.04 && 5.04 <= p50.upper); // exact median
  assert(p90.lower <= 9.04 && 9.04 <= p90.upper); // exact 90th
  assert(p50.upper <= 1.016*p50.lower);
  assert(p90.upper <= 1.016*p90.lower);
  const double small=std::numeric_limits<double>::denorm_min();
  GapQuantiles tiny;
  for(int i=0;i<5000;++i) assert(tiny.add(small));
  auto q=tiny.percentile(.5);
  assert(q.lower<=small && small<=q.upper);
  return 0;
}
