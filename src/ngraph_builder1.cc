/*******************************************************************************
 * Copyright 2017-2018 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *******************************************************************************/

#include "ngraph_builder1.h"
//#include "ngraph_conversions.h"
#include "ngraph_log.h"
#include "ngraph_utils.h"

#include "ngraph/builder/autobroadcast.hpp"
#include "ngraph/builder/numpy_transpose.hpp"

#include "tensorflow/core/framework/tensor.pb.h"
#include "tensorflow/core/framework/tensor_shape.pb.h"
#include "tensorflow/core/framework/tensor_shape.pb_text.h"
#include "tensorflow/core/graph/algorithm.h"
#include "tensorflow/core/graph/edgeset.h"
#include "tensorflow/core/lib/core/errors.h"

using namespace std;
namespace ng = ngraph;

namespace tensorflow {

namespace ngraph_bridge {

Status Builder1::TranslateGraph(
    OpKernelContext* ctx, std::shared_ptr<ngraph::Function>& ng_function) {
  // TODO: confirm that static_input_map cannot be constructed in constructor.
  // It could be different everytime right?
  // TODO: static_input_map can't be a class variable right? Its different
  // everytime TranslateGraph is called,
  // so it must be passed around?

  if (!is_initialized) Initialize();

  std::vector<const Tensor*> static_input_map;

  std::vector<TensorShape> inputs(ctx->num_inputs());

  static_input_map.resize(ctx->num_inputs());
  for (int i = 0; i < ctx->num_inputs(); i++) {
    const Tensor& input_tensor = ctx->input(i);
    if (m_input_is_static[i]) {
      static_input_map[i] = &input_tensor;
    }
    inputs[i] = input_tensor.shape();
  }
  // TODO: pass static_input_map to translate_each_op

  vector<shared_ptr<ng::op::Parameter>> ng_parameter_list;
  TF_RETURN_IF_ERROR(GetInputParams(inputs, tf_params, &ng_parameter_list));

  TF_RETURN_IF_ERROR(TranslateEachOp(tf_ops, static_input_map));

  vector<shared_ptr<ng::Node>> ng_result_list;
  TF_RETURN_IF_ERROR(GetOutputNodes(tf_ops, ng_result_list));

  // Create the nGraph function.
  ng_function = make_shared<ng::Function>(ng_result_list, ng_parameter_list);
  return Status::OK();  // TODO
}

//TODO, is static_input_map const?
Status Builder1::TranslateEachOp(const vector<const Node*>& tf_ops, const std::vector<const Tensor*>& static_input_map) {
  // Create the nGraph ops from TensorFlow ops.

  for (auto op : tf_ops) {
    NGRAPH_VLOG(2) << "Constructing op " << op->name() << " which is "
                   << op->type_string();

    try {
      //extract out parent node finding and savengops
      // have the translateops as a list of pluggable functions (pimpl?)

      // TODO.....TODO....TODO
      // TF_RETURN_IF_ERROR(TRANSLATE_OP_MAP.at(op->type_string())(
      //   op, static_input_map, ng_op_map));
     
      //The static_input_map thing:
      //some ops have static input, not everyone needs this input.
      //we can make static_input_map a class data member, and set it to nullptr once the TranslateGraph is done
      // That way, we do not pass it around, and make sure we are not reusing stale static_input_map


      //Note: abstracting parents = GetInputNodes():
      // unknown number of nodes for each op
      //std::shared_ptr<ng::Node> ng_lhs, ng_rhs;
      //TF_RETURN_IF_ERROR(GetInputNodes(op, &ng_lhs, &ng_rhs));

      vector<shared_ptr<ng::Node>> subgraph_out_nodes;
      //TF_RETURN_IF_ERROR(TRANSLATE_OP_MAP.at(op->type_string())(op, static_input_map));
      auto iter = TRANSLATE_OP_MAP.find(op->type_string());
      if (iter != TRANSLATE_OP_MAP.end()){
        Builder1::TranslatorFn translate_fn = iter->second.first;
        vector<int> input_idxs = iter->second.second;
        //std::tie(translate_fn, input_idxs) = iter->second;
        // input_idxs can be size 0 (to indicate/handle variadic inputs nodes like Addn)
        bool variadic_input = input_idxs.size()==0;
        int num_inputs = variadic_input ? op->num_inputs() : input_idxs.size();
        std::vector<shared_ptr<ng::Node>> ng_arg_vec(num_inputs);
        for (int idx = 0; idx < num_inputs; idx++){
          TF_RETURN_IF_ERROR(GetInputNode(op, (variadic_input ? idx : input_idxs[idx]), &ng_arg_vec[idx]));
        }
        TF_RETURN_IF_ERROR(iter->second.first(op, ng_arg_vec, static_input_map, subgraph_out_nodes));
      }
      else{
        //TODO TODO::: if-else or try-catch
        // -----------------------------
        // Catch-all for unsupported ops
        // -----------------------------
        NGRAPH_VLOG(3) << "Unsupported Op: " << op->name() << " ("
                      << op->type_string() << ")";
        NGRAPH_VLOG(3) << op->def().DebugString();
        return errors::InvalidArgument("Unsupported Op: ", op->name(), " (",
                                      op->type_string(), ")");
      }

      
      //for (auto ng_node : subgraph_out_nodes)
      //  SaveNgOp(ng_op_map, op->name(), ng_node);
      ng_op_map[op->name()] = subgraph_out_nodes; //SaveNgOp

      //TranslateBinaryOp(op, static_input_map, ng_op_map, ng_floormod)

    } 
    //TODO: catching the error above. do we keep the try catch or use if-else?
    catch (const std::out_of_range&) {
      // -----------------------------
      // Catch-all for unsupported ops
      // -----------------------------
      NGRAPH_VLOG(3) << "Unsupported Op: " << op->name() << " ("
                     << op->type_string() << ")";
      NGRAPH_VLOG(3) << op->def().DebugString();
      return errors::InvalidArgument("Unsupported Op: ", op->name(), " (",
                                     op->type_string(), ")");
    }
  }
  return Status::OK();
}

Status Builder1::ClassifyNodes(const vector<Node*>& ordered,
                                vector<const Node*>& tf_params,
                                vector<const Node*>& tf_ret_vals,
                                vector<const Node*>& tf_ops) {
  // Split ops into params, retvals, and all others.

  for (const auto n : ordered) {
    if (n->IsSink() || n->IsSource()) {
      continue;
    }

    if (n->IsControlFlow()) {
      return errors::Unimplemented(
          "Encountered a control flow op in the nGraph bridge: ",
          n->DebugString());
    }

    if (n->type_string() == "_Arg") {
      tf_params.push_back(n);
    } else if (n->type_string() == "_Retval") {
      tf_ret_vals.push_back(n);
    } else {
      tf_ops.push_back(n);
    }
  }
  return Status::OK();
}

Status Builder1::GetInputParams(
    const std::vector<TensorShape>& inputs, vector<const Node*> tf_params,
    vector<shared_ptr<ng::op::Parameter>>* ng_parameter_list) {
  // Populate the parameter list, and also put parameters into the op map.

  ng_parameter_list->resize(tf_params.size());

  for (auto parm : tf_params) {
    DataType dtype;
    if (GetNodeAttr(parm->attrs(), "T", &dtype) != Status::OK()) {
      return errors::InvalidArgument("No data type defined for _Arg");
    }
    int index;
    if (GetNodeAttr(parm->attrs(), "index", &index) != Status::OK()) {
      return errors::InvalidArgument("No index defined for _Arg");
    }

    ng::element::Type ng_et;
    TF_RETURN_IF_ERROR(TFDataTypeToNGraphElementType(dtype, &ng_et));

    ng::Shape ng_shape;
    TF_RETURN_IF_ERROR(TFTensorShapeToNGraphShape(inputs[index], &ng_shape));

    auto ng_param = make_shared<ng::op::Parameter>(ng_et, ng_shape);
    SaveNgOp(parm->name(), ng_param);
    (*ng_parameter_list)[index] = ng_param;
  }
  return Status::OK();
}

Status Builder1::GetOutputNodes(
    const vector<const Node*>& tf_ret_vals,
    vector<shared_ptr<ng::Node>>& ng_result_list) {
  // Populate the result list.

  ng_result_list.resize(tf_ret_vals.size());

  for (auto n : tf_ret_vals) {
    // Make sure that this _Retval only has one input node.
    if (n->num_inputs() != 1) {
      return errors::InvalidArgument("_Retval has ", n->num_inputs(),
                                     " inputs, should have 1");
    }

    int index;
    if (GetNodeAttr(n->attrs(), "index", &index) != Status::OK()) {
      return errors::InvalidArgument("No index defined for _Retval");
    }

    shared_ptr<ng::Node> result;
    // TF_RETURN_IF_ERROR(GetInputNode(ng_op_map, n, 0, &result)); //TODO...TODO

    ng_result_list[index] = result;
  }
  return Status::OK();
}

// TODO: combine the 2 MakePaddings together? with default args?
// would want the default args to be at the end of the paramlist,
// but 'outputs' (like ng_padding_below, ng_padding_above) are usually at the
// end of the param list
// TODO: why does MakePadding handle only 2 dimensions... generalize it?
// TODO: add unit tests for 1D conv. 1d pooling etc. check if MakePadding works
// in that case
template <typename T>
void MakePadding(const std::string& tf_padding_type,
                 const ngraph::Shape& ng_image_shape,
                 const ngraph::Shape& ng_kernel_shape,
                 const ngraph::Strides& ng_strides,
                 const ngraph::Shape& ng_dilations, T& ng_padding_below,
                 T& ng_padding_above) {
  ngraph::Shape ng_dilation_kernel_shape{
      (ng_kernel_shape[0] - 1) * ng_dilations[0] + 1,
      (ng_kernel_shape[1] - 1) * ng_dilations[1] + 1};

  MakePadding(tf_padding_type, ng_image_shape, ng_dilation_kernel_shape,
              ng_strides, ng_padding_below, ng_padding_above);
}

template <typename T>
void MakePadding(const std::string& tf_padding_type,
                 const ngraph::Shape& ng_image_shape,
                 const ngraph::Shape& ng_kernel_shape,
                 const ngraph::Strides& ng_strides, T& ng_padding_below,
                 T& ng_padding_above) {
  if (tf_padding_type == "SAME") {
    for (size_t i = 0; i < 2; i++) {
      /*
      size_t image_size = ng_image_shape[i];
      size_t filter_shape = ng_kernel_shape[i];
      size_t filter_stride = ng_strides[i];

      int64 padding_needed;
      if (image_size % filter_stride == 0) {
        padding_needed = filter_shape - filter_stride;
      } else {
        padding_needed = filter_shape - (image_size % filter_stride);
      }
      if (padding_needed < 0) {
        padding_needed = 0;
      }
      */

      // TODO: check this:. This formula is documented well documented here.
      // So I prefer this, compared to the one above (though both are exactly
      // the same)
      // https://www.tensorflow.org/api_guides/python/nn#Notes_on_SAME_Convolution_Padding
      int64 out_shape = ceil(ng_image_shape[i] / ng_strides[i]);
      int64 padding_needed = ng_strides[i] * (out_shape - 1) +
                             ng_kernel_shape[i] - ng_image_shape[i];
      padding_needed = padding_needed < 0 ? 0 : padding_needed;

      size_t padding_lhs = padding_needed / 2;
      size_t padding_rhs = padding_needed - padding_lhs;
      ng_padding_below[i] = padding_lhs;
      ng_padding_above[i] = padding_rhs;
    }
  }

  NGRAPH_VLOG(3) << "ng_padding_below: " << ngraph::join(ng_padding_below);
  NGRAPH_VLOG(3) << "ng_padding_above: " << ngraph::join(ng_padding_above);
}

Status Builder1::ValidateInputCount(const Node* op, size_t count) {
  if (op->num_inputs() != count) {
    return errors::InvalidArgument("\"", op->name(), "\" requires ", count,
                                   " input(s), got ", op->num_inputs(),
                                   " instead");
  }
  return Status::OK();
}

Status Builder1::ValidateInputCountMin(const Node* op, size_t count) {
  if (op->num_inputs() < count) {
    return errors::InvalidArgument("\"", op->name(), "\" requires at least ",
                                   count, " input(s), got ", op->num_inputs(),
                                   " instead");
  }
  return Status::OK();
}

void Builder1::SaveNgOp(const std::string& op_name,
                        const shared_ptr<ng::Node>& output_node) {
  // no need to try-catch, map[key] will create vector object
  // if not exists
  ng_op_map[op_name].push_back(output_node);
}

Status Builder1::Initialize() {
  if (is_initialized) {
    //
    // We will visit ops in topological order.
    //
    // ought to be `const Node*`, but GetReversePostOrder doesn't use `const`

    GetReversePostOrder(tf_graph, &ordered);

    TF_RETURN_IF_ERROR(ClassifyNodes(ordered, tf_params, tf_ret_vals, tf_ops));
  
  // TODO: since we have an init anyway, why not move this stuff there

    // TODO: maybe we do not need to copy ctx into a pvt variable., as we do not
    // use it later
    // TODO: move m_input_is_static construction to init??

    //
    // Initialize the "m_input_is_static" vector as follows:
    // (1) create m_input_is_static with n+1 elements, where n is the max arg
    //     index
    // (2) for each _Arg node n, set m_input_is_static[n.index] to true if n
    //     is driving any static input; else set it to false.
    //

    // Create the vector.
    int32 max_arg_index = -1;
    std::vector<const Node*> arg_nodes;

    for (auto node : tf_graph.nodes()) {
      if (node->type_string() == "_Arg") {
        arg_nodes.push_back(node);

        int32 index;
        // macro defn:
        // https://github.com/petewarden/tensorflow_makefile/blob/master/tensorflow/core/framework/op_kernel.h#L1265
        
        //TODO check : removing OP_REQUIRES_OK for now
        //// OP_REQUIRES_OK(ctx, GetNodeAttr(node->attrs(), "index", &index));
        TF_RETURN_IF_ERROR(GetNodeAttr(node->attrs(), "index", &index));
        if (index > max_arg_index) max_arg_index = index;
      }
    }

    m_input_is_static = std::vector<bool>(max_arg_index + 1, false);

    // Fill the vector.
    for (auto node : arg_nodes) {
      int32 index;
      //// OP_REQUIRES_OK(ctx, GetNodeAttr(node->attrs(), "index", &index));
      TF_RETURN_IF_ERROR(GetNodeAttr(node->attrs(), "index", &index));

      // bool is_static = false;
      for (auto edge : node->out_edges()) {
        if (edge->IsControlEdge() || !edge->dst()->IsOp()) {
          continue;
        }

        NGRAPH_VLOG(5) << "For arg " << index << " checking edge "
                       << edge->DebugString();

        if (InputIsStatic(edge->dst(), edge->dst_input())) {
          NGRAPH_VLOG(5) << "Marking edge static: " << edge->DebugString();
          // is_static = true;
          m_input_is_static[index] = true;
          break;
        }
      }

      // NGRAPH_VLOG(5) << "Marking arg " << index << " is_static: " <<
      // is_static;
      // m_input_is_static[index] = is_static;
      NGRAPH_VLOG(5) << "Marking arg " << index
                     << " is static: " << m_input_is_static[index];
    }

  is_initialized = true;
  }
  return Status::OK();
}

Status Builder1::GetInputNode(const Node* op, size_t input_idx,
                              shared_ptr<ng::Node>* result) {
  // input op may have resulted in more than one ng::Node (eg. Split)
  // we need to look at Edge to check index of the input op
  std::vector<const Edge*> edges;
  TF_RETURN_IF_ERROR(op->input_edges(&edges));
  size_t src_output_idx;
  try {
    src_output_idx = edges.at(input_idx)->src_output();
  } catch (const out_of_range&) {
    return Status(tensorflow::error::NOT_FOUND, "Edge not found");
  }

  Node* tf_input;
  TF_RETURN_IF_ERROR(op->input_node(input_idx, &tf_input));
  try {
    *result = ng_op_map.at(tf_input->name()).at(src_output_idx);
  } catch (const out_of_range&) {
    return Status(tensorflow::error::NOT_FOUND, "Input node not found");
  }
  return Status::OK();
}


//TODO: inner class?
// namespace detail {
Status Builder1::detail_GetInputNodes(const Node* op, size_t index) {
  return Status::OK();
}

template <typename... Arguments>
Status Builder1::detail_GetInputNodes(const Node* op, size_t index,
                                      shared_ptr<ng::Node>* result,
                                      Arguments&&... remaining) {
  if (result != nullptr) {
    TF_RETURN_IF_ERROR(GetInputNode(op, index, result));
  }
  return Builder1::detail_GetInputNodes(op, index + 1, remaining...);
}
//}  // namespace detail


template <typename... Arguments>
Status Builder1::GetInputNodes(const Node* op, Arguments&&... remaining) {
  constexpr size_t args_len = sizeof...(Arguments);
  TF_RETURN_IF_ERROR(ValidateInputCount(op, args_len));
  // return detail::GetInputNodes(ng_op_map, op, 0, remaining...);  //TODO..
  // detail namespace
  return detail_GetInputNodes(op, 0, remaining...);
}

// TODO: move translate ops to a different file
// TODO: make TranslateOps not static?
Status TranslateFloorDivOp1(const Node* op, const std::vector<shared_ptr<ng::Node>>& ng_arg_vec, const std::vector<const Tensor*>& static_input_map, vector<shared_ptr<ng::Node>>& subgraph_out_nodes) {
  subgraph_out_nodes[0] = std::make_shared<ng::op::Floor>(ng_arg_vec[0] / ng_arg_vec[1]);
  return Status::OK();
}

Status TranslateFloorModOp1(const Node* op, const std::vector<shared_ptr<ng::Node>>& ng_arg_vec, const std::vector<const Tensor*>& static_input_map, vector<shared_ptr<ng::Node>>& subgraph_out_nodes) {
  auto floordiv = std::make_shared<ng::op::Floor>(ng_arg_vec[0] / ng_arg_vec[1]);
  subgraph_out_nodes[0] = ng_arg_vec[0] - (floordiv * ng_arg_vec[1]);
  return Status::OK();
}


static Status TranslateBinaryOp(
    const Node* op, const std::vector<const Tensor*>& static_input_map,
    vector<shared_ptr<ng::Node>>& subgraph_out_nodes,
    std::function<std::shared_ptr<ng::Node>(std::shared_ptr<ng::Node>,
                                            std::shared_ptr<ng::Node>)>
        create_binary_op) {
  std::shared_ptr<ng::Node> ng_lhs, ng_rhs;

  std::tie(ng_lhs, ng_rhs) =
      ng::builder::numpy_broadcast(std::make_pair(ng_lhs, ng_rhs));
  return Status::OK();
}

// Helper function to translate a binary op in cases where there is a one-to-one
// mapping from TensorFlow ops to nGraph ops.
//
// Example usage:
//
//  if (n->type_string == "Add") {
//    TF_RETURN_IF_ERROR(TranslateBinaryOp<ng::op::Add>(op, static_input_map,
//    ng_op_map));
//  }
//
template <typename T>
static Status TranslateBinaryOp(
    const Node* op, const std::vector<const Tensor*>& static_input_map,
    vector<shared_ptr<ng::Node>>& subgraph_out_nodes) {
  return TranslateBinaryOp(
      op, static_input_map,
      [](std::shared_ptr<ng::Node> ng_lhs, std::shared_ptr<ng::Node> ng_rhs) {
        return make_shared<T>(ng_lhs, ng_rhs);
      });
}


const std::map<
    const string, std::pair<Builder1::TranslatorFn, vector<int>>
    >
    Builder1::TRANSLATE_OP_MAP{{"FloorDiv", {TranslateFloorDivOp1, {0,1}}},
                               {"FloorMod", {TranslateFloorModOp1, {0,1}}}};


}  // namespace ngraph_bridge

}  // namespace tensorflow