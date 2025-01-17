// Copyright (c) 2019 PaddlePaddle Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "lite/core/optimizer/mir/quantization_parameters_propagation_pass.h"
#include <cmath>
#include <string>
#include <unordered_set>
#include <vector>
#include "lite/core/optimizer/mir/graph_visualize_pass.h"
#include "lite/core/optimizer/mir/pass_registry.h"

namespace paddle {
namespace lite {
namespace mir {

static bool HasOutputThreshold(const OpInfo* op_info,
                               const std::string& name,
                               bool is_threshold_name) {
  bool res = false;
  if (is_threshold_name) {
    res = op_info->HasAttr(name);
  } else {
    std::string argname;
    int index;
    if (op_info->HasAttr("out_threshold")) {
      res = true;
    } else if (op_info->GetOutputArgname(name, &argname) &&
               op_info->GetOutputIndex(name, &index)) {
      res = op_info->HasAttr(argname + to_string(index) + "_threshold");
    }
  }
  return res;
}

static float GetOutputThreshold(const OpInfo* op_info,
                                const std::string& name,
                                bool is_threshold_name) {
  std::string threshold_name;
  if (is_threshold_name) {
    threshold_name = name;
  } else if (op_info->HasAttr("out_threshold")) {
    threshold_name = "out_threshold";
  } else {
    std::string argname;
    int index;
    CHECK(op_info->GetOutputArgname(name, &argname));
    CHECK(op_info->GetOutputIndex(name, &index));
    threshold_name = argname + to_string(index) + "_threshold";
  }
  return op_info->GetAttr<float>(threshold_name);
}

// Propagate the input scale which is from fake_quantize_xxx and
// fake_quantize_dequantize_xxx op
static bool SetOutScaleFromNextInScale(
    const std::unique_ptr<SSAGraph>& graph,
    int force_complete_quant_scale_level = 0) {
  bool found = false;
  for (auto& op_node : graph->StmtTopologicalOrder()) {
    if (!op_node->IsStmt()) continue;
    auto op_info = op_node->AsStmt().mutable_op_info();
    for (auto in_var_node : op_node->inlinks) {
      CHECK(in_var_node->IsArg());
      auto in_var_name = in_var_node->arg()->name;
      if (!op_info->HasInputScale(in_var_name)) continue;
      auto in_var_scale = op_info->GetInputScale(in_var_name);
      for (auto in_op_node : in_var_node->inlinks) {
        if (!in_op_node->IsStmt()) continue;
        auto in_op_info = in_op_node->AsStmt().mutable_op_info();
        bool in_op_is_quanted =
            HasOutputThreshold(in_op_info, in_var_name, false);
        auto in_op_in_vars = in_op_node->inlinks;
        for (auto in_op_in_var : in_op_in_vars) {
          in_op_is_quanted =
              in_op_is_quanted ||
              in_op_info->HasInputScale(in_op_in_var->arg()->name);
        }
        in_op_is_quanted =
            in_op_is_quanted || force_complete_quant_scale_level >= 1;
        if (in_op_is_quanted) {
          // Use this input scale to update the output scale of the quantized op
          in_op_info->SetOutputScale(in_var_name, in_var_scale);
          found = true;
        }
      }
    }
  }
  return found;
}

// Calculate the output scale according to its output threshold
static bool SetOutScaleFromCurOutThreshold(
    const std::unique_ptr<SSAGraph>& graph) {
  bool found = false;
  for (auto& op_node : graph->StmtTopologicalOrder()) {
    if (!op_node->IsStmt()) continue;
    auto op_info = op_node->AsStmt().mutable_op_info();
    for (auto out_var_node : op_node->outlinks) {
      CHECK(out_var_node->IsArg());
      auto out_var_name = out_var_node->arg()->name;
      if (op_info->HasOutputScale(out_var_name))
        continue;  // skip if the output scale had been already set
      if (!HasOutputThreshold(op_info, out_var_name, false)) continue;
      // If it's a quantized op, calculate its output scale according to the
      // output threshold(abs_max value)
      int bit_length = 8;  // op_info->GetAttr<int>("bit_length");
      int range = (1 << (bit_length - 1)) - 1;
      auto out_var_scale = std::vector<float>{
          GetOutputThreshold(op_info, out_var_name, false) / range};
      op_info->SetOutputScale(out_var_name, out_var_scale);
      found = true;
    }
  }
  return found;
}

// Set the input scale according to the output scale of the previous ops
static bool SetInScaleFromPrevOutScale(const std::unique_ptr<SSAGraph>& graph) {
  bool found = false;
  for (auto& op_node : graph->StmtTopologicalOrder()) {
    if (!op_node->IsStmt()) continue;
    auto op_info = op_node->AsStmt().mutable_op_info();
    for (auto in_var_node : op_node->inlinks) {
      CHECK(in_var_node->IsArg());
      auto in_var_name = in_var_node->arg()->name;
      if (op_info->HasInputScale(in_var_name)) continue;
      std::vector<float> in_var_scale;
      for (auto in_op_node : in_var_node->inlinks) {
        if (!in_op_node->IsStmt()) continue;
        auto in_op_info = in_op_node->AsStmt().mutable_op_info();
        if (!in_op_info->HasOutputScale(in_var_name)) continue;
        auto candidate_var_scale = in_op_info->GetOutputScale(in_var_name);
        if (!in_var_scale.empty()) {
          auto scale_size = in_var_scale.size();
          CHECK_EQ(scale_size, candidate_var_scale.size());
          for (size_t i = 0; i < scale_size; i++) {
            CHECK(fabs(in_var_scale[i] - candidate_var_scale[i]) <= 1e-7f);
          }
        } else {
          in_var_scale = candidate_var_scale;
        }
      }
      if (!in_var_scale.empty()) {
        op_info->SetInputScale(in_var_name, in_var_scale);
        found = true;
      }
    }
  }
  return found;
}

// Set the output scale according to the input scale
static bool SetOutScaleFromCurInScale(
    const std::unique_ptr<SSAGraph>& graph,
    const std::vector<std::string>& valid_op_types) {
  bool found = false;
  for (auto& op_node : graph->StmtTopologicalOrder()) {
    if (!op_node->IsStmt()) continue;
    auto op_info = op_node->AsStmt().mutable_op_info();
    auto op_type = op_info->Type();
    if (std::find(valid_op_types.begin(), valid_op_types.end(), op_type) ==
        valid_op_types.end())
      continue;
    for (auto out_var_node : op_node->outlinks) {
      CHECK(out_var_node->IsArg());
      auto out_var_name = out_var_node->arg()->name;
      if (op_info->HasOutputScale(out_var_name)) continue;
      for (auto in_var_node : op_node->inlinks) {
        CHECK(in_var_node->IsArg());
        auto in_var_name = in_var_node->arg()->name;
        if (!op_info->HasInputScale(in_var_name)) continue;
        op_info->SetOutputScale(out_var_name,
                                op_info->GetInputScale(in_var_name));
        found = true;
        break;
      }
    }
  }
  return found;
}

// Set the input scale according to the output scale
static bool SetInScaleFromCurOutScale(
    const std::unique_ptr<SSAGraph>& graph,
    const std::vector<std::string>& valid_op_types) {
  bool found = false;
  for (auto& op_node : graph->StmtTopologicalOrder()) {
    if (!op_node->IsStmt()) continue;
    auto op_info = op_node->AsStmt().mutable_op_info();
    auto op_type = op_info->Type();
    if (std::find(valid_op_types.begin(), valid_op_types.end(), op_type) ==
        valid_op_types.end())
      continue;
    for (auto in_var_node : op_node->inlinks) {
      CHECK(in_var_node->IsArg());
      auto in_var_name = in_var_node->arg()->name;
      if (op_info->HasInputScale(in_var_name)) continue;
      for (auto out_var_node : op_node->outlinks) {
        CHECK(out_var_node->IsArg());
        auto out_var_name = out_var_node->arg()->name;
        if (!op_info->HasOutputScale(out_var_name)) continue;
        op_info->SetInputScale(in_var_name,
                               op_info->GetOutputScale(out_var_name));
        found = true;
        break;
      }
    }
  }
  return found;
}

void QuantizationParametersPropagationPass::Apply(
    const std::unique_ptr<SSAGraph>& graph) {
  VLOG(5) << "\n" << Visualize(graph.get());
  int force_complete_quant_scale_level =
      GetIntFromEnv(QUANT_AUTO_COMPLETE_SCALE_LEVEL);
  SetOutScaleFromNextInScale(graph, force_complete_quant_scale_level);
  SetOutScaleFromCurOutThreshold(graph);
  SetInScaleFromPrevOutScale(graph);
  // In some special cases, quant info is still missing. Try following steps to
  // fill quant info.
  // Ops that allow input and output sacles are the same.
  std::vector<std::string> valid_op_types{"bilinear_interp_v2",
                                          "leaky_relu",
                                          "nearest_interp_v2",
                                          "pool2d",
                                          "relu"};
  if (force_complete_quant_scale_level >= 2) {
    bool found = true;
    while (found) {
      found = SetOutScaleFromCurInScale(graph, valid_op_types);
      SetInScaleFromPrevOutScale(graph);
    }
  }
  if (force_complete_quant_scale_level >= 3) {
    bool found = true;
    while (found) {
      found = SetInScaleFromCurOutScale(graph, valid_op_types);
      SetOutScaleFromNextInScale(graph);
    }
  }
  VLOG(5) << "\n" << Visualize(graph.get());
}

}  // namespace mir
}  // namespace lite
}  // namespace paddle

REGISTER_MIR_PASS(quantization_parameters_propagation_pass,
                  paddle::lite::mir::QuantizationParametersPropagationPass)
    .BindTargets({TARGET(kNNAdapter)});
