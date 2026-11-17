#include <unity.h>
#include <cmath>
#include "acquisition/mq135_nh3.h"
using namespace sigap::acquisition;
void setUp() {}
void tearDown() {}
void test_synthetic_log_fit_is_test_only_bounded_and_not_a_production_profile() {
  const Mq135Nh3Point points[]{{1,2,25,50,true},{2,1,25,50,true},{4,.5,25,50,true},{8,.25,25,50,true}};
  Mq135Nh3Model model{};
  TEST_ASSERT_TRUE(fit_mq135_nh3_test_model(points,4,"SYNTHETIC",1,model));
  TEST_ASSERT_TRUE(std::fabs(model.curve_a-2.0)<1e-9);
  TEST_ASSERT_TRUE(std::fabs(model.curve_b+1.0)<1e-9);
  TEST_ASSERT_TRUE(std::fabs(model.training_log_rmse)<1e-9);
  double ppm{};
  TEST_ASSERT_TRUE(mq135_nh3_test_estimate(model,.5,ppm));
  TEST_ASSERT_TRUE(std::fabs(ppm-4.0)<1e-9);
  TEST_ASSERT_FALSE(model.production_eligible());
  TEST_ASSERT_FALSE(mq135_nh3_test_estimate(model,.01,ppm));
  TEST_ASSERT_FALSE(mq135_nh3_test_estimate(model,INFINITY,ppm));
  model.curve_b=0; TEST_ASSERT_FALSE(mq135_nh3_test_estimate(model,1,ppm));
  model.curve_b=-1; model.curve_a=-2; TEST_ASSERT_FALSE(mq135_nh3_test_estimate(model,1,ppm));
}
void test_invalid_reference_points_do_not_create_a_model() {
  Mq135Nh3Point points[]{{1,2,25,50,true},{2,1,25,50,true},{4,.5,25,50,true}};
  Mq135Nh3Model model{};
  points[1].stabilized=false;
  TEST_ASSERT_FALSE(fit_mq135_nh3_test_model(points,3,"SYNTHETIC",1,model));
  points[1].stabilized=true;points[1].concentration_ppm=0;
  TEST_ASSERT_FALSE(fit_mq135_nh3_test_model(points,3,"SYNTHETIC",1,model));
  points[1].concentration_ppm=2;points[1].humidity_pct=NAN;
  TEST_ASSERT_FALSE(fit_mq135_nh3_test_model(points,3,"SYNTHETIC",1,model));
}
int main(){UNITY_BEGIN();RUN_TEST(test_synthetic_log_fit_is_test_only_bounded_and_not_a_production_profile);RUN_TEST(test_invalid_reference_points_do_not_create_a_model);return UNITY_END();}
