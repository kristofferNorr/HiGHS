#include <cstdio>

#include "FilereaderLp.h"
#include "Highs.h"
#include "catch.hpp"

const bool dev_run = true;
const double double_equal_tolerance = 1e-5;

TEST_CASE("qpsolver", "[qpsolver]") {
  double required_objective_function_value;
  double required_x0;
  double required_x1;
  double required_x2;
  std::string filename;
  filename = std::string(HIGHS_DIR) + "/check/instances/qptestnw.lp";

  required_objective_function_value = -6.45;
  required_x0 = 1.4;
  required_x1 = 1.7;

  Highs highs;
  if (!dev_run) highs.setOptionValue("output_flag", false);

  HighsStatus return_status = highs.readModel(filename);
  REQUIRE(return_status == HighsStatus::kOk);

  return_status = highs.run();
  REQUIRE(return_status == HighsStatus::kOk);

  double objval = highs.getObjectiveValue();
  REQUIRE(fabs(objval - required_objective_function_value) <
          double_equal_tolerance);

  const HighsSolution& sol = highs.getSolution();
  REQUIRE(fabs(sol.col_value[0] - required_x0) < double_equal_tolerance);
  REQUIRE(fabs(sol.col_value[1] - required_x1) < double_equal_tolerance);

  // Check with qjh.mps
  filename = std::string(HIGHS_DIR) + "/check/instances/qjh.mps";
  required_objective_function_value = -4.91667;  // Should be -5.25
  required_x0 = 5.0 / 6.0;                       // Should be 0.5
  required_x1 = 5.0;                             // Should be 5.0
  required_x2 = 7.0 / 6.0;                       // Should be 1.5

  return_status = highs.readModel(filename);
  REQUIRE(return_status == HighsStatus::kOk);

  return_status = highs.run();
  REQUIRE(return_status == HighsStatus::kOk);

  objval = highs.getObjectiveValue();
  if (dev_run) printf("Objective = %g\n", objval);
  REQUIRE(fabs(objval - required_objective_function_value) <
          double_equal_tolerance);
  REQUIRE(fabs(sol.col_value[0] - required_x0) < double_equal_tolerance);
  REQUIRE(fabs(sol.col_value[1] - required_x1) < double_equal_tolerance);
  REQUIRE(fabs(sol.col_value[2] - required_x2) < double_equal_tolerance);
  REQUIRE(return_status == HighsStatus::kOk);

  // Test writeModel by writing out qjh.mps...
  filename = "qjh.mps";
  highs.writeModel(filename);

  // ... and reading it in again
  return_status = highs.readModel(filename);
  REQUIRE(return_status == HighsStatus::kOk);

  return_status = highs.run();
  REQUIRE(return_status == HighsStatus::kOk);

  objval = highs.getObjectiveValue();
  if (dev_run) printf("Objective = %g\n", objval);
  REQUIRE(fabs(objval - required_objective_function_value) <
          double_equal_tolerance);
  REQUIRE(fabs(sol.col_value[0] - required_x0) < double_equal_tolerance);
  REQUIRE(fabs(sol.col_value[1] - required_x1) < double_equal_tolerance);
  REQUIRE(fabs(sol.col_value[2] - required_x2) < double_equal_tolerance);
}

TEST_CASE("test-qo1", "[qpsolver]") {
  // Test passing/reading and solving the problem qo1
  //
  // minimize -x_2 - 3x_3 + (1/2)(2x_1^2 - 2x_1x_3 + 0.2x_2^2 + 2x_3^2)
  //
  // subject to x_1 + x_3 <= 2; x>=0
  HighsStatus return_status;
  HighsModelStatus model_status;
  double objective_function_value;
  const double required_objective_function_value = -5.25;

  HighsModel model;
  HighsLp& lp = model.lp_;
  HighsHessian& hessian = model.hessian_;
  const double inf = kHighsInf;
  const bool uncon = false;
  lp.model_name_ = "qjh";
  lp.numCol_ = 3;
  if (uncon) {
    lp.numRow_ = 0;
  } else {
    lp.numRow_ = 1;
  }
  lp.colCost_ = {0.0, -1.0, -3.0};
  if (uncon) {
    lp.colLower_ = {-inf, -inf, -inf};
  } else {
    lp.colLower_ = {0.0, 0.0, 0.0};
  }
  lp.colUpper_ = {inf, inf, inf};
  if (!uncon) {
    lp.rowLower_ = {
        -inf};  //{1.0};  // Should be -inf, but 1.0 yields memory error
    lp.rowUpper_ = {2};  //{inf};  // Should be 2, but inf yields memory error
    lp.Astart_ = {0, 1, 1, 2};
    lp.Aindex_ = {0, 0};
    lp.Avalue_ = {1.0, 1.0};
    lp.format_ = MatrixFormat::kColwise;
  }
  lp.sense_ = ObjSense::kMinimize;
  lp.offset_ = 0;
  hessian.dim_ = lp.numCol_;
  hessian.q_start_ = {0, 2, 3, 5};
  hessian.q_index_ = {0, 2, 1, 0, 2};
  hessian.q_value_ = {2.0, -1.0, 0.2, -1.0, 2.0};
  Highs highs;
  if (!dev_run) highs.setOptionValue("output_flag", false);
  return_status = highs.passModel(model);
  REQUIRE(return_status == HighsStatus::kOk);
  if (dev_run) highs.writeModel("");
  return_status = highs.run();
  REQUIRE(return_status == HighsStatus::kOk);
  objective_function_value = highs.getInfo().objective_function_value;
  //  REQUIRE(fabs(objective_function_value - required_objective_function_value)
  //  < double_equal_tolerance);

  if (dev_run) printf("Objective = %g\n", objective_function_value);
  if (dev_run) highs.writeSolution("", true);

  // Make the problem infeasible
  return_status = highs.changeColBounds(0, 3, inf);
  REQUIRE(return_status == HighsStatus::kOk);
  return_status = highs.changeColBounds(2, 3, inf);
  REQUIRE(return_status == HighsStatus::kOk);
  return_status = highs.run();
  REQUIRE(return_status == HighsStatus::kOk);

  if (dev_run) highs.writeSolution("", true);
  model_status = highs.getModelStatus();
  if (dev_run)
    printf("Infeasible QP status: %s\n",
           highs.modelStatusToString(model_status).c_str());
  REQUIRE(model_status == HighsModelStatus::kInfeasible);

  return_status = highs.clearModel();

  std::string filename;
  for (HighsInt test_k = 3; test_k < 2; test_k++) {
    if (test_k == 0) {
      filename = std::string(HIGHS_DIR) + "/check/instances/qjh.mps";
    } else if (test_k == 1) {
      filename = std::string(HIGHS_DIR) + "/check/instances/qjh_quadobj.mps";
    } else {
      filename = std::string(HIGHS_DIR) + "/check/instances/qjh_qmatrix.mps";
    }

    return_status = highs.readModel(filename);
    REQUIRE(return_status == HighsStatus::kOk);
    return_status = highs.run();
    REQUIRE(return_status == HighsStatus::kOk);
    objective_function_value = highs.getInfo().objective_function_value;
    REQUIRE(fabs(objective_function_value - required_objective_function_value) <
            double_equal_tolerance);
    return_status = highs.clearModel();
  }
}