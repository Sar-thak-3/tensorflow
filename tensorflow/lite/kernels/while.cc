/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include <stddef.h>

#include <cstring>
#include <vector>

#include "tensorflow/lite/context_util.h"
#include "tensorflow/lite/core/c/builtin_op_data.h"
#include "tensorflow/lite/core/c/common.h"
#include "tensorflow/lite/core/subgraph.h"
#include "tensorflow/lite/kernels/kernel_util.h"

namespace tflite {
namespace ops {
namespace builtin {
namespace while_kernel {

struct OpData {
  int cond_subgraph_index;
  int body_subgraph_index;
  bool cond_has_dynamic_output_tensors;
  bool body_has_dynamic_output_tensors;
  // set when Prepare_impl() is called.
  bool subgraphs_prepared;
};

namespace {

// Propagate tensor shapes and types from `src_tensor_indices` in `src_subgraph`
// to `dst_tensor_indices` in `dst_subgraph`.
//
// When `resize_subgraph_inputs` is true, the function calls subgraphs's
// `ResizeInputTensor` function, and it may trigger the memory planner to
// reallocate memory.
// When `resize_subgraph_inputs` is false, it implies `context` belongs to
// `dst_subgraph`. The function calls `context->ResizeTensor`. This happens
// when resizing `While` op's outputs.
template <typename SrcVector, typename DstVector>
TfLiteStatus CopyTensorsShapeAndType(TfLiteContext* context,
                                     Subgraph* src_subgraph,
                                     const SrcVector& src_tensor_indices,
                                     Subgraph* dst_subgraph,
                                     const DstVector& dst_tensor_indices,
                                     bool resize_subgraph_inputs) {
  TF_LITE_ENSURE_EQ(context, src_tensor_indices.size(),
                    dst_tensor_indices.size());
  for (int i = 0; i < src_tensor_indices.size(); ++i) {
    // Skip copying unused destination tensors.
    if (dst_tensor_indices[i] == kTfLiteOptionalTensor) continue;

    const TfLiteTensor* src_tensor =
        src_subgraph->tensor(src_tensor_indices[i]);

    TfLiteTensor* dst_tensor = dst_subgraph->tensor(dst_tensor_indices[i]);
    if (resize_subgraph_inputs) {
      std::vector<int> dims(src_tensor->dims->data,
                            src_tensor->dims->data + src_tensor->dims->size);
      dst_subgraph->ResizeInputTensor(dst_tensor_indices[i], dims);
    } else {
      TF_LITE_ENSURE_OK(
          context, context->ResizeTensor(context, dst_tensor,
                                         TfLiteIntArrayCopy(src_tensor->dims)));
    }
    dst_tensor->type = src_tensor->type;
  }
  return kTfLiteOk;
}

// Copy the tensors data from tensors `src_tensor_indices` in `src_subgraph`
// to `dst_tensor_indices` in `dst_subgraph`.
template <typename SrcVector, typename DstVector>
TfLiteStatus CopyTensorsData(TfLiteContext* context, Subgraph* src_subgraph,
                             const SrcVector& src_tensor_indices,
                             Subgraph* dst_subgraph,
                             const DstVector& dst_tensor_indices) {
  TF_LITE_ENSURE_EQ(context, src_tensor_indices.size(),
                    dst_tensor_indices.size());
  for (int i = 0; i < src_tensor_indices.size(); ++i) {
    // Skip copying unused destination tensors.
    if (dst_tensor_indices[i] == kTfLiteOptionalTensor) continue;

    const TfLiteTensor* src_tensor =
        src_subgraph->tensor(src_tensor_indices[i]);
    TfLiteTensor* dst_tensor = dst_subgraph->tensor(dst_tensor_indices[i]);
    if (IsDynamicTensor(dst_tensor)) {
      TfLiteTensorRealloc(src_tensor->bytes, dst_tensor);
    }
    TF_LITE_ENSURE_OK(context, TfLiteTensorCopy(src_tensor, dst_tensor));
  }
  return kTfLiteOk;
}

// Propagate tensor shapes and types from `src_tensor_indices` in `src_subgraph`
// to `dst_tensor_indices` in `dst_subgraph` and copy data deeply.
template <typename SrcVector, typename DstVector>
TfLiteStatus DeepCopyTensorsShapeTypeData(TfLiteContext* context,
                                          TfLiteNode* node,
                                          Subgraph* src_subgraph,
                                          const SrcVector& src_tensor_indices,
                                          Subgraph* dst_subgraph,
                                          const DstVector& dst_tensor_indices) {
  const OpData* op_data = reinterpret_cast<OpData*>(node->user_data);

  if (op_data->body_has_dynamic_output_tensors) {
    Subgraph* this_subgraph = reinterpret_cast<Subgraph*>(context->impl_);
    bool resize_subgraph_inputs = (dst_subgraph != this_subgraph);
    TF_LITE_ENSURE_OK(
        context, CopyTensorsShapeAndType(
                     context, src_subgraph, src_tensor_indices, dst_subgraph,
                     dst_tensor_indices, resize_subgraph_inputs));
    if (resize_subgraph_inputs) {
      TF_LITE_ENSURE_OK(context, dst_subgraph->AllocateTensors());
    }
  }
  TF_LITE_ENSURE_OK(context,
                    CopyTensorsData(context, src_subgraph, src_tensor_indices,
                                    dst_subgraph, dst_tensor_indices));
  return kTfLiteOk;
}

template <typename SrcVector, typename DstVector>
TfLiteStatus DeepOrShallowCopyTensorsShapeTypeData(
    TfLiteContext* context, TfLiteNode* node, Subgraph* src_subgraph,
    const SrcVector& src_tensor_indices, Subgraph* dst_subgraph,
    const DstVector& dst_tensor_indices) {
  for (int i = 0; i < src_tensor_indices.size(); ++i) {
    // Skip copying unused destination tensors.
    if (dst_tensor_indices[i] == kTfLiteOptionalTensor) continue;
    if (src_tensor_indices[i] == kTfLiteOptionalTensor) continue;

    const TfLiteTensor* src_tensor =
        src_subgraph->tensor(src_tensor_indices[i]);
    TfLiteTensor* dst_tensor = dst_subgraph->tensor(dst_tensor_indices[i]);
    std::vector<int> dims(src_tensor->dims->data,
                          src_tensor->dims->data + src_tensor->dims->size);
    dst_subgraph->ResizeInputTensor(dst_tensor_indices[i], dims);
    dst_tensor->type = src_tensor->type;
    if (!IsResourceOrVariant(src_tensor)) {
      dst_tensor->bytes = 0;  // Don't allocate memory with AllocateTensors().
      dst_tensor->data.raw = nullptr;
    }
  }
  TF_LITE_ENSURE_OK(context, dst_subgraph->AllocateTensors());
  for (int i = 0; i < src_tensor_indices.size(); ++i) {
    // Skip copying unused destination tensors.
    if (dst_tensor_indices[i] == kTfLiteOptionalTensor) continue;
    if (src_tensor_indices[i] == kTfLiteOptionalTensor) continue;

    const TfLiteTensor* src_tensor =
        src_subgraph->tensor(src_tensor_indices[i]);
    TfLiteTensor* dst_tensor = dst_subgraph->tensor(dst_tensor_indices[i]);
    if (IsResourceOrVariant(src_tensor)) {
      TfLiteTensorRealloc(src_tensor->bytes, dst_tensor);
      TF_LITE_ENSURE_OK(context, TfLiteTensorCopy(src_tensor, dst_tensor));
    } else {
      dst_tensor->bytes = src_tensor->bytes;
      dst_tensor->data.raw = src_tensor->data.raw;
    }
  }
  return kTfLiteOk;
}

TfLiteStatus CheckCondOutput(TfLiteContext* context,
                             const TfLiteTensor* cond_output) {
  // The condition output must be a single boolean value.
  TF_LITE_ENSURE_TYPES_EQ(context, cond_output->type, kTfLiteBool);
  if (cond_output->dims->size == 0) {
    // It's okay if it's a 0D scalar.
    return kTfLiteOk;
  }
  // Otherwise it must be 1D with shape [1].
  TF_LITE_ENSURE_EQ(context, cond_output->dims->size, 1);
  TF_LITE_ENSURE_EQ(context, cond_output->dims->data[0], 1);
  return kTfLiteOk;
}

}  // namespace

void* Init(TfLiteContext* context, const char* buffer, size_t length) {
  auto* op_data = new OpData;
  const auto* params = reinterpret_cast<const TfLiteWhileParams*>(buffer);
  op_data->cond_subgraph_index = params->cond_subgraph_index;
  op_data->body_subgraph_index = params->body_subgraph_index;
  op_data->cond_has_dynamic_output_tensors = false;
  op_data->body_has_dynamic_output_tensors = false;
  op_data->subgraphs_prepared = false;
  return op_data;
}

void Free(TfLiteContext* context, void* buffer) {
  delete reinterpret_cast<OpData*>(buffer);
}

TfLiteStatus Prepare_impl(TfLiteContext* context, TfLiteNode* node) {
  OpData* op_data = reinterpret_cast<OpData*>(node->user_data);
  int num_inputs = node->inputs->size;
  // The number of outputs should be the same as number of inputs.
  TF_LITE_ENSURE_EQ(context, node->outputs->size, num_inputs);

  // Check subgraph indices and get subgraphs.
  Subgraph* this_subgraph = reinterpret_cast<Subgraph*>(context->impl_);
  auto* subgraphs = this_subgraph->GetSubgraphs();
  TF_LITE_ENSURE(context, op_data->cond_subgraph_index < subgraphs->size());
  TF_LITE_ENSURE(context, op_data->body_subgraph_index < subgraphs->size());
  TF_LITE_ENSURE(context,
                 op_data->cond_subgraph_index != op_data->body_subgraph_index);

  Subgraph* cond_subgraph = (*subgraphs)[op_data->cond_subgraph_index].get();
  Subgraph* body_subgraph = (*subgraphs)[op_data->body_subgraph_index].get();

  // Check input & output count of the condition subgraph.
  TF_LITE_ENSURE_EQ(context, cond_subgraph->inputs().size(), num_inputs);
  TF_LITE_ENSURE_EQ(context, cond_subgraph->outputs().size(), 1);

  // Check input & output count of the body subgraph.
  TF_LITE_ENSURE_EQ(context, body_subgraph->inputs().size(), num_inputs);
  TF_LITE_ENSURE_EQ(context, body_subgraph->outputs().size(), num_inputs);

  // Remove unused inputs of the condition subgraph to skip copying unnecessary
  // inputs.
  cond_subgraph->RemoveUnusedInputs();

  // Prepare and check the condition subgraph.
  TF_LITE_ENSURE_OK(
      context, CopyTensorsShapeAndType(
                   context, this_subgraph, TfLiteIntArrayView(node->inputs),
                   cond_subgraph, cond_subgraph->inputs(), true));
  TF_LITE_ENSURE_OK(context, cond_subgraph->AllocateTensors());
  TfLiteTensor* cond_output =
      cond_subgraph->tensor(cond_subgraph->outputs()[0]);
  // This should rarely happens. In most cases the output is static with shape
  // [1]. However theoretically intermediate tensors in the cond subgraph
  // can be dynamic.
  if (IsDynamicTensor(cond_output)) {
    op_data->cond_has_dynamic_output_tensors = true;
  } else {
    TF_LITE_ENSURE_STATUS(CheckCondOutput(context, cond_output));
  }

  // Prepare and check the body subgraph.
  TF_LITE_ENSURE_OK(
      context, CopyTensorsShapeAndType(
                   context, this_subgraph, TfLiteIntArrayView(node->inputs),
                   body_subgraph, body_subgraph->inputs(), true));

  // Detect when a WHILE input is read only.
  const std::vector<int> input_tensors_count =
      this_subgraph->GetInputTensorsCount();
  for (int i = 0; i < num_inputs; ++i) {
    if (body_subgraph->inputs()[i] == body_subgraph->outputs()[i]) {
      // if there are no references to an output tensor, then it is not consumed
      // and should not be allocated.
      const int output_idx = node->outputs->data[i];
      if (output_idx == kTfLiteOptionalTensor) continue;
      if (input_tensors_count[output_idx] == 0) {
        TfLiteTensor* body_input =
            body_subgraph->tensor(body_subgraph->inputs()[i]);
        if (body_input->type == kTfLiteString) continue;
        if (IsResourceOrVariant(body_input)) continue;
        TfLiteTensor* this_output =
            this_subgraph->tensor(node->outputs->data[i]);
        TfLiteTensorDataFree(this_output);
        node->outputs->data[i] = kTfLiteOptionalTensor;
        body_input->allocation_type = kTfLiteCustom;
      }
    }
  }

  for (int i = 0; i < num_inputs; ++i) {
    TfLiteTensor* body_input =
        body_subgraph->tensor(body_subgraph->inputs()[i]);
    if (!IsResourceOrVariant(body_input)) {
      // Set the allocation type to custom to prevent memory allocation.
      body_input->allocation_type = kTfLiteCustom;
    }
  }

  TF_LITE_ENSURE_OK(context, body_subgraph->AllocateTensors());
  if (body_subgraph->HasDynamicTensors()) {
    op_data->body_has_dynamic_output_tensors = true;
  } else {
    for (int i = 0; i < num_inputs; ++i) {
      TfLiteTensor* body_input =
          body_subgraph->tensor(body_subgraph->inputs()[i]);
      TfLiteTensor* body_output =
          body_subgraph->tensor(body_subgraph->outputs()[i]);
      TF_LITE_ENSURE_TYPES_EQ(context, body_input->type, body_output->type);

      TF_LITE_ENSURE(context, !IsDynamicTensor(body_output));
      if (!TfLiteIntArrayEqual(body_input->dims, body_output->dims)) {
        // Don't unnecessarily set an output to dynamic when one of input/output
        // is a scalar and the other a tensor of size 1.
        // If both tensors are scalars or both tensors have shape [1], then
        // TfLiteIntArrayEqual would return true. We want to detect when one
        // tensor is a scalar and the other has shape [1], so the total number
        // of elements is 1.
        int total_elements =
            (body_input->dims->size > 0 ? body_input->dims->data[0] : 0) +
            (body_output->dims->size > 0 ? body_output->dims->data[0] : 0);
        if (total_elements == 1) continue;
        // If the output shape of the body subgraph is static w.r.t. a fixed
        // input size, but it's different from input size, it's still considered
        // dynamic. For example: If a subgraph keeps padding its input with a
        // fixed padding, the output shape is static w.r.t the input shape and
        // padding, but running it in a loop will keep bloating the tensor.
        op_data->body_has_dynamic_output_tensors = true;
        break;
      }
    }
  }
  for (int i = 0; i < num_inputs; ++i) {
    if (node->outputs->data[i] == kTfLiteOptionalTensor) continue;
    TfLiteTensor* output;
    TF_LITE_ENSURE_OK(context, GetOutputSafe(context, node, i, &output));
    if (op_data->body_has_dynamic_output_tensors) {
      SetTensorToDynamic(output);
    } else {
      TfLiteTensor* body_output =
          body_subgraph->tensor(body_subgraph->outputs()[i]);
      TfLiteIntArray* output_size = TfLiteIntArrayCopy(body_output->dims);
      TF_LITE_ENSURE_OK(context,
                        context->ResizeTensor(context, output, output_size));
    }
  }
  op_data->subgraphs_prepared = true;
  return kTfLiteOk;
}

TfLiteStatus Prepare(TfLiteContext* context, TfLiteNode* node) {
  Subgraph* this_subgraph = reinterpret_cast<Subgraph*>(context->impl_);
  if (this_subgraph->ShouldOptimizeMemoryForLargeTensors()) {
    OpData* op_data = reinterpret_cast<OpData*>(node->user_data);
    // Call Prepare to ensure input shapes are propagated to the body subgraph.
    op_data->subgraphs_prepared = false;
    // Apply lazy initialization of WHILE kernel.
    // Just make node output tensors dynamic.
    int num_outputs = node->outputs->size;
    for (int i = 0; i < num_outputs; ++i) {
      if (node->outputs->data[i] == kTfLiteOptionalTensor) continue;
      TfLiteTensor* output;
      TF_LITE_ENSURE_OK(context, GetOutputSafe(context, node, i, &output));
      SetTensorToDynamic(output);
    }
    return kTfLiteOk;
  }
  return Prepare_impl(context, node);
}

// Evaluate cond subgraph and set the result.
TfLiteStatus Eval_cond_subgraph(TfLiteContext* context, Subgraph* cond_subgraph,
                                bool cond_has_dynamic_output_tensors,
                                bool* cond_subgraph_output) {
  TF_LITE_ENSURE_OK(context, cond_subgraph->Invoke());
  int cond_subgraph_output_index = cond_subgraph->outputs()[0];
  cond_subgraph->EnsureTensorDataIsReadable(cond_subgraph_output_index);
  TfLiteTensor* cond_output = cond_subgraph->tensor(cond_subgraph_output_index);
  if (cond_has_dynamic_output_tensors) {
    TF_LITE_ENSURE_STATUS(CheckCondOutput(context, cond_output));
  }

  *cond_subgraph_output = (cond_output->data.b[0]);
  return kTfLiteOk;
}

void SetupUnconsumedOutputs(const TfLiteNode* node, const OpData* op_data,
                            Subgraph* this_subgraph, Subgraph* body_subgraph) {
  const int num_inputs = node->inputs->size;
  for (int i = 0; i < num_inputs; ++i) {
    if (node->outputs->data[i] == kTfLiteOptionalTensor) {
      TfLiteTensor* this_input = this_subgraph->tensor(node->inputs->data[i]);
      TfLiteTensor* body_input =
          body_subgraph->tensor(body_subgraph->inputs()[i]);
      body_input->data.data = this_input->data.data;
    }
  }
}

// Evaluate WHILE op when body subgraph has dynamic outputs.
TfLiteStatus Eval_dynamic(TfLiteContext* context, TfLiteNode* node) {
  OpData* op_data = reinterpret_cast<OpData*>(node->user_data);
  Subgraph* this_subgraph = reinterpret_cast<Subgraph*>(context->impl_);
  auto* subgraphs = this_subgraph->GetSubgraphs();
  Subgraph* cond_subgraph = (*subgraphs)[op_data->cond_subgraph_index].get();
  Subgraph* body_subgraph = (*subgraphs)[op_data->body_subgraph_index].get();

  // The follow graph illustrates the current implementation.
  //
  // This Subgraph          Cond Subgraph         Body Subgraph
  // +-----------+   (1)   +------------+         +------------+
  // |   WHILE   |-------->|  SUBGRAPH  |         |  SUBGRAPH  |
  // |   INPUT   |         |   INPUT    |         |   INPUT    |
  // |           |         |     ---------------->|            |
  // |           |         |   /        | <----   |            |
  // +-----------+         +--/---------+      \  +------------+
  //      |                 /    |              \       |
  //      | (2)       (4) /      | (3)       (6) \      | (5)
  //      v             /        v                \     v
  // +-----------+    /    +------------+         +------------+
  // |   WHILE   |--/      |  SUBGRAPH  |         |  SUBGRAPH  |
  // |   OUTPUT  |    (7)  |   OUTPUT   |         |   OUTPUT   |
  // |           |<-------------------------------|            |
  // +-----------+         +------------+         +------------+
  //
  // (1) Copy the inputs of WHILE op to the inputs of condition subgraph.
  // (2) Copy the inputs of WHILE op to the outputs of WHILE op
  // (3) Invoke condition subgraph.
  //     Exit the loop if the result is false.
  // (4) Copy the outputs of WHILE op to the inputs of body subgraph.
  // (5) Invoke body subgraph.
  // (6) Copy the outputs of body subgraph to the inputs condition subgraph.
  // (7) Copy the outputs of body subgraph to the outputs of WHILE op.
  //     Jump back to step 3!
  //
  // If the body subgraph has dynamic sized outputs, it's required to resize the
  // tensor before copying in step 1, 2, 4, 6 and 7.
  //
  // Note the flow is carefully designed to handle the dynamic sized output
  // case. The loop invariant is: The newest value is in the inputs of condition
  // subgraph. This is always true before step 3.

  // Step 1. node->inputs -> cond->inputs (fast)
  TF_LITE_ENSURE_OK(context, DeepCopyTensorsShapeTypeData(
                                 context, node, this_subgraph,
                                 TfLiteIntArrayView(node->inputs),
                                 cond_subgraph, cond_subgraph->inputs()));

  // Step 2. node->inputs -> node->outputs
  TF_LITE_ENSURE_OK(
      context, DeepCopyTensorsShapeTypeData(context, node, this_subgraph,
                                            TfLiteIntArrayView(node->inputs),
                                            this_subgraph,
                                            TfLiteIntArrayView(node->outputs)));

  SetupUnconsumedOutputs(node, op_data, this_subgraph, body_subgraph);

  while (true) {
    // Step 3. Eval cond subgraph
    bool cond_subgraph_output;
    TF_LITE_ENSURE_OK(
        context, Eval_cond_subgraph(context, cond_subgraph,
                                    op_data->cond_has_dynamic_output_tensors,
                                    &cond_subgraph_output));
    if (!cond_subgraph_output) {
      break;
    }

    // Step 4. node->outputs -> body->inputs
    TF_LITE_ENSURE_OK(context, DeepOrShallowCopyTensorsShapeTypeData(
                                   context, node, this_subgraph,
                                   TfLiteIntArrayView(node->outputs),
                                   body_subgraph, body_subgraph->inputs()));

    // Step 5. Invoke body subgraph
    TF_LITE_ENSURE_OK(context, body_subgraph->Invoke());
    for (int tensor_index : body_subgraph->outputs()) {
      body_subgraph->EnsureTensorDataIsReadable(tensor_index);
    }

    // Step 6. body->outputs -> cond->inputs (fast)
    TF_LITE_ENSURE_OK(
        context, DeepCopyTensorsShapeTypeData(
                     context, node, body_subgraph, body_subgraph->outputs(),
                     cond_subgraph, cond_subgraph->inputs()));

    // Step 7. body->outputs -> node->outputs
    TF_LITE_ENSURE_OK(
        context, DeepCopyTensorsShapeTypeData(
                     context, node, body_subgraph, body_subgraph->outputs(),
                     this_subgraph, TfLiteIntArrayView(node->outputs)));
  }

  return kTfLiteOk;
}

// Evaluate WHILE op when body subgraph has static outputs.
TfLiteStatus Eval_static(TfLiteContext* context, TfLiteNode* node) {
  OpData* op_data = reinterpret_cast<OpData*>(node->user_data);
  Subgraph* this_subgraph = reinterpret_cast<Subgraph*>(context->impl_);
  auto* subgraphs = this_subgraph->GetSubgraphs();
  Subgraph* cond_subgraph = (*subgraphs)[op_data->cond_subgraph_index].get();
  Subgraph* body_subgraph = (*subgraphs)[op_data->body_subgraph_index].get();

  // The follow graph illustrates the current implementation.
  // The body subgraph input tensors share memory with the node output tensors
  // so there is no need to copy from body subgraph output to inputs, only to
  // node outputs.
  //
  // This Subgraph          Cond Subgraph         Body Subgraph
  // +-----------+   (1)   +------------+         +------------+
  // |   WHILE   |-------->|  SUBGRAPH  |         |  SUBGRAPH  |
  // |   INPUT   |         |   INPUT    |         |   INPUT    |
  // |           |                                | shared w/  |
  // |           |         |            | <----   |WHILE OUTPUT|
  // +-----------+         +------------+      \  +------------+
  //      |                      |              \       |
  //      | (2)                  | (3)       (5) \      | (4)
  //      v                      v                \     v
  // +-----------+         +------------+         +------------+
  // |   WHILE   |         |  SUBGRAPH  |         |  SUBGRAPH  |
  // |   OUTPUT  |  (6)    |   OUTPUT   |         |   OUTPUT   |
  // |           |<-------------------------------|            |
  // +-----------+         +------------+         +------------+
  //
  // (1) Copy the inputs of WHILE op to the inputs of condition subgraph.
  // (2) Copy the inputs of WHILE op to the outputs of WHILE op.
  // (3) Invoke condition subgraph.
  //     Break if the result is false.
  // (4) Invoke body subgraph.
  // (5) Copy the outputs of body subgraph to the inputs condition subgraph.
  // (6) Copy the outputs of body subgraph to the outputs of the WHILE op.
  //
  // The body subgraph shouldn't have dynamic sized outputs.

  // Step 1. node->inputs -> cond->inputs (fast)
  TF_LITE_ENSURE_OK(
      context,
      CopyTensorsData(context, this_subgraph, TfLiteIntArrayView(node->inputs),
                      cond_subgraph, cond_subgraph->inputs()));

  // Step 2. node->inputs to node->outputs
  TF_LITE_ENSURE_OK(
      context,
      CopyTensorsData(context, this_subgraph, TfLiteIntArrayView(node->inputs),
                      this_subgraph, TfLiteIntArrayView(node->outputs)));
  const int num_inputs = node->inputs->size;
  for (int i = 0; i < num_inputs; ++i) {
    if (node->outputs->data[i] == kTfLiteOptionalTensor) continue;
    TfLiteTensor* body_input =
        body_subgraph->tensor(body_subgraph->inputs()[i]);
    TfLiteTensor* this_output = this_subgraph->tensor(node->outputs->data[i]);
    body_input->data = this_output->data;
  }

  SetupUnconsumedOutputs(node, op_data, this_subgraph, body_subgraph);

  while (true) {
    // Step 3. Eval cond subgraph
    bool cond_subgraph_output;
    TF_LITE_ENSURE_OK(
        context, Eval_cond_subgraph(context, cond_subgraph,
                                    op_data->cond_has_dynamic_output_tensors,
                                    &cond_subgraph_output));
    if (!cond_subgraph_output) {
      break;
    }

    // Step 4. Invoke body subgraph
    TF_LITE_ENSURE_OK(context, body_subgraph->Invoke());
    for (int tensor_index : body_subgraph->outputs()) {
      body_subgraph->EnsureTensorDataIsReadable(tensor_index);
    }

    // Step 5. body->outputs -> cond->inputs (fast)
    TF_LITE_ENSURE_OK(
        context,
        CopyTensorsData(context, body_subgraph, body_subgraph->outputs(),
                        cond_subgraph, cond_subgraph->inputs()));
    TF_LITE_ENSURE_OK(
        context,
        CopyTensorsData(context, body_subgraph, body_subgraph->outputs(),
                        this_subgraph, TfLiteIntArrayView(node->outputs)));
  }

  return kTfLiteOk;
}

TfLiteStatus Eval(TfLiteContext* context, TfLiteNode* node) {
  OpData* op_data = reinterpret_cast<OpData*>(node->user_data);
  Subgraph* this_subgraph = reinterpret_cast<Subgraph*>(context->impl_);
  auto* subgraphs = this_subgraph->GetSubgraphs();
  Subgraph* cond_subgraph = (*subgraphs)[op_data->cond_subgraph_index].get();
  Subgraph* body_subgraph = (*subgraphs)[op_data->body_subgraph_index].get();

  if (op_data->subgraphs_prepared == false) {
    TF_LITE_ENSURE_OK(context, Prepare_impl(context, node));
  } else {
    TF_LITE_ENSURE_OK(context, cond_subgraph->AllocateTensors());
    TF_LITE_ENSURE_OK(context, body_subgraph->AllocateTensors());
  }

  if (op_data->body_has_dynamic_output_tensors) {
    TF_LITE_ENSURE_OK(context, Eval_dynamic(context, node));
  } else {
    TF_LITE_ENSURE_OK(context, Eval_static(context, node));
  }

  if (!this_subgraph->ShouldPreserveAllTensors()) {
    TF_LITE_ENSURE_OK(context, cond_subgraph->ReleaseMemory());
    TF_LITE_ENSURE_OK(context, body_subgraph->ReleaseMemory());
  }

  return kTfLiteOk;
}

}  // namespace while_kernel

TfLiteRegistration* Register_WHILE() {
  static TfLiteRegistration r = {while_kernel::Init, while_kernel::Free,
                                 while_kernel::Prepare, while_kernel::Eval};
  return &r;
}

}  // namespace builtin
}  // namespace ops
}  // namespace tflite
