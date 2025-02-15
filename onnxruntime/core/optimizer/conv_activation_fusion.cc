// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include <deque>
#include "core/graph/graph_utils.h"
#include "core/optimizer/conv_activation_fusion.h"
#include "core/optimizer/initializer.h"
#include "core/optimizer/utils.h"

using namespace ONNX_NAMESPACE;
using namespace ::onnxruntime::common;
namespace onnxruntime {

Status ConvActivationFusion::ApplyImpl(Graph& graph, bool& modified, int graph_level, const logging::Logger& logger) const {
  GraphViewer graph_viewer(graph);
  const auto& order = graph_viewer.GetNodesInTopologicalOrder();

  for (auto index : order) {
    auto* node = graph.GetNode(index);
    // check that node hasn't already been removed
    if (!node)
      continue;

    ORT_RETURN_IF_ERROR(Recurse(*node, modified, graph_level, logger));

    if (!graph_utils::IsSupportedOptypeVersionAndDomain(*node, "Conv", {1, 11}) ||
        !graph_utils::IsSupportedProvider(*node, GetCompatibleExecutionProviders()) ||
        node->GetOutputEdgesCount() != 1) {
      continue;
    }

    const auto& next_node = *(node->OutputNodesBegin());

    if (next_node.GetExecutionProviderType() != node->GetExecutionProviderType()) {
      continue;
    }

    if (graph.NodeProducesGraphOutput(*node)) {
      continue;
    }

    if (node->GetExecutionProviderType() == onnxruntime::kCudaExecutionProvider) {
      if (node->InputDefs()[0]->TypeAsProto()->tensor_type().elem_type() !=
          ONNX_NAMESPACE::TensorProto_DataType_FLOAT) {
        continue;
      }
      if (graph_utils::IsSupportedOptypeVersionAndDomain(next_node, "Relu", {6, 13, 14})) {
        Node& conv_node = *node;
        Node& act_node = *graph.GetNode(next_node.Index());
        auto node_name = graph.GenerateNodeName(conv_node.Name() + "_" + act_node.Name());
        Node& fused_conv = graph.AddNode(node_name,
                                         "FusedConv",
                                         node_name,
                                         conv_node.MutableInputDefs(),
                                         {},
                                         &conv_node.GetAttributes(),
                                         onnxruntime::kMSDomain);
        fused_conv.SetExecutionProviderType(conv_node.GetExecutionProviderType());
        fused_conv.AddAttribute("activation", "Relu");
        graph_utils::FinalizeNodeFusion(graph, {conv_node, act_node}, fused_conv);
        modified = true;
      } else if (graph_utils::IsSupportedOptypeVersionAndDomain(next_node, "Add", {6, 7, 13, 14})) {
        if (next_node.GetOutputEdgesCount() != 1) {
          continue;
        }
        const auto& last_node = *(next_node.OutputNodesBegin());
        if (last_node.GetExecutionProviderType() != node->GetExecutionProviderType()) {
          continue;
        }
        if (graph_utils::IsSupportedOptypeVersionAndDomain(last_node, "Relu", {6, 13, 14})) {
          Node& conv_node = *node;
          Node& add_node = *graph.GetNode(next_node.Index());
          Node& act_node = *graph.GetNode(last_node.Index());
          auto conv_inputs = conv_node.MutableInputDefs();
          auto conv_outputs = conv_node.MutableOutputDefs();
          auto add_inputs = add_node.MutableInputDefs();
          int32_t dependent = 0, independent = 0;
          for (auto add_input : add_inputs) {
            if (add_input->Name() == conv_outputs[0]->Name()) {
              dependent++;
            } else {
              conv_inputs.push_back(add_input);
              independent++;
            }
          }
          if (dependent != 1 || independent != 1) {
            continue;
          }
          auto node_name = graph.GenerateNodeName(conv_node.Name() + "_" +
                                                  add_node.Name() + "_" +
                                                  act_node.Name());
          Node& fused_conv = graph.AddNode(node_name,
                                           "FusedConv",
                                           node_name,
                                           conv_inputs,
                                           {}, &conv_node.GetAttributes(),
                                           onnxruntime::kMSDomain);
          fused_conv.SetExecutionProviderType(conv_node.GetExecutionProviderType());
          fused_conv.AddAttribute("activation", "Relu");
          graph_utils::FinalizeNodeFusion(graph, {conv_node, add_node, act_node}, fused_conv);
          modified = true;
        }
      }
    } else {
      // Test if this is an activation that can be fused and also extract the
      // activation's parameters.
      InlinedVector<float> activation_params;
      if (!graph_utils::IsSupportedOptypeVersionAndDomain(next_node, "Relu", {6, 13, 14}) &&
          !graph_utils::IsSupportedOptypeVersionAndDomain(next_node, "Sigmoid", {6, 13}) &&
          !graph_utils::IsSupportedOptypeVersionAndDomain(next_node, "Tanh", {6, 13})) {
        if (graph_utils::IsSupportedOptypeVersionAndDomain(next_node, "LeakyRelu", {6})) {
          activation_params.push_back(graph_utils::GetNodeAttribute(next_node, "alpha")->f());
        } else if (graph_utils::IsSupportedOptypeVersionAndDomain(next_node, "Clip", {6, 11, 12, 13})) {
          float min, max;
          if (optimizer_utils::GetClipConstantMinMax(graph, next_node, min, max)) {
            activation_params.push_back(min);
            activation_params.push_back(max);
          } else {
            continue;
          }
        } else if ((node->GetExecutionProviderType().empty() || node->GetExecutionProviderType() == onnxruntime::kCpuExecutionProvider) &&
                   graph_utils::IsSupportedOptypeVersionAndDomain(next_node, "HardSigmoid", {6})) {
          auto* alpha_attr = graph_utils::GetNodeAttribute(next_node, "alpha");
          auto* beta_attr = graph_utils::GetNodeAttribute(next_node, "beta");
          float alpha = (alpha_attr == nullptr ? 0.2f : alpha_attr->f());
          float beta = (beta_attr == nullptr ? 0.5f : beta_attr->f());
          activation_params.push_back(alpha);
          activation_params.push_back(beta);
        } else {
          continue;
        }
      }

      Node& conv_node = *node;
      Node& act_node = *graph.GetNode(next_node.Index());

      Node& fused_conv = graph.AddNode(graph.GenerateNodeName("fused " + conv_node.Name()), "FusedConv",
                                       "fused Conv " + conv_node.Name() + "with activation " + act_node.OpType(),
                                       conv_node.MutableInputDefs(),
                                       {},
                                       &conv_node.GetAttributes(),
                                       "com.microsoft");

      // Assign provider to this new node. Provider should be same as the provider for old node.
      fused_conv.SetExecutionProviderType(conv_node.GetExecutionProviderType());

      // Add attributes to specify the activation type and parameters.
      fused_conv.AddAttribute("activation", next_node.OpType());
      if (!activation_params.empty()) {
        fused_conv.AddAttribute("activation_params", activation_params);
      }

      // move output definitions and edges from act_node to fused_conv. delete conv_node and act_node.
      graph_utils::FinalizeNodeFusion(graph, {conv_node, act_node}, fused_conv);

      modified = true;
    }
  }

  return Status::OK();
}
}  // namespace onnxruntime
