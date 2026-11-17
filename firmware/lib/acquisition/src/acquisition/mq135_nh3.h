#pragma once
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include "models/model_profiles.h"
#include "acquisition/mq135_nh3_curve.h"

namespace sigap::acquisition {
// Offline preparation only. No registration, persistence, active profile or
// concentration output is connected to this research model. MQG1 stays null.
struct Mq135Nh3Point final {
  double concentration_ppm{}, ratio{}, temperature_c{}, humidity_pct{};
  bool stabilized{false};
};
struct Mq135Nh3Model final {
  uint32_t model_version{};
  models::ModelDeployment status{models::ModelDeployment::Unconfigured};
  std::array<char,32> reference_dataset_id{};
  uint16_t sample_count{};
  double curve_a{}, curve_b{}, minimum_ppm{}, maximum_ppm{}, training_log_rmse{};
  // Training residuals do not establish certified references, holdout error,
  // uncertainty, baseline identity, cross-sensitivity or physical acceptance.
  constexpr bool production_eligible() const noexcept { return false; }
};
inline bool fit_mq135_nh3_test_model(const Mq135Nh3Point* points, std::size_t count,
    std::string_view dataset, uint32_t version, Mq135Nh3Model& output) noexcept {
  output = {};
  if (points == nullptr || count < 3 || count > 64 || version == 0 || dataset.empty() || dataset.size() > 31) return false;
  for (const char c:dataset) if (!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.'||c==':')) return false;
  double mean_x{},mean_y{},minimum=points[0].concentration_ppm,maximum=minimum;
  for (std::size_t i=0;i<count;++i) {
    const auto& p=points[i];
    if (!p.stabilized || !std::isfinite(p.concentration_ppm) || p.concentration_ppm<=0 ||
        !std::isfinite(p.ratio) || p.ratio<=0 || !std::isfinite(p.temperature_c) ||
        !std::isfinite(p.humidity_pct) || p.humidity_pct<0 || p.humidity_pct>100) return false;
    mean_x+=std::log(p.concentration_ppm)/count; mean_y+=std::log(p.ratio)/count;
    if(p.concentration_ppm<minimum)minimum=p.concentration_ppm;
    if(p.concentration_ppm>maximum)maximum=p.concentration_ppm;
  }
  double xx{},xy{};
  for (std::size_t i=0;i<count;++i) {
    const double x=std::log(points[i].concentration_ppm)-mean_x;
    xx+=x*x;xy+=x*(std::log(points[i].ratio)-mean_y);
  }
  if (!std::isfinite(xx) || !std::isfinite(xy) || xx<=0 || minimum>=maximum) return false;
  const double b=xy/xx, intercept=mean_y-b*mean_x, a=std::exp(intercept);
  if (!std::isfinite(a) || a<=0 || !std::isfinite(b) || b==0) return false;
  double error{};
  for (std::size_t i=0;i<count;++i) {
    const double residual=std::log(points[i].ratio)-(intercept+b*std::log(points[i].concentration_ppm));
    error+=residual*residual/count;
  }
  if (!std::isfinite(error)) return false;
  output.model_version=version;output.status=models::ModelDeployment::TestOnly;
  for(std::size_t i=0;i<dataset.size();++i)output.reference_dataset_id[i]=dataset[i];
  output.sample_count=static_cast<uint16_t>(count);output.curve_a=a;output.curve_b=b;
  output.minimum_ppm=minimum;output.maximum_ppm=maximum;output.training_log_rmse=std::sqrt(error);
  return true;
}
inline bool mq135_nh3_test_estimate(const Mq135Nh3Model& model, double ratio, double& ppm) noexcept {
  ppm=0;
  if(model.status!=models::ModelDeployment::TestOnly || model.model_version==0 || model.sample_count<3 ||
     model.sample_count>64 || model.reference_dataset_id[0]==0 ||
     !std::isfinite(model.curve_a)||model.curve_a<=0 || !std::isfinite(model.curve_b)||model.curve_b==0 ||
     !std::isfinite(model.training_log_rmse)||model.training_log_rmse<0 ||
     !std::isfinite(model.minimum_ppm)||!std::isfinite(model.maximum_ppm)||model.minimum_ppm<=0||model.minimum_ppm>=model.maximum_ppm||
     !std::isfinite(ratio)||ratio<=0) return false;
  const double concentration=std::exp((std::log(ratio)-std::log(model.curve_a))/model.curve_b);
  if(!std::isfinite(concentration)||concentration<model.minimum_ppm||concentration>model.maximum_ppm)return false;
  ppm=concentration;return true;
}
} // namespace sigap::acquisition
