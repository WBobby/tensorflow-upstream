/* Copyright 2021 The TensorFlow Authors. All Rights Reserved.

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
#include "tensorflow/core/runtime_fallback/kernel/kernel_fallback_execute_compat.h"

#include <optional>
#include <string>

#include "llvm/ADT/StringRef.h"
#include "tensorflow/core/common_runtime/eager/context.h"
#include "tensorflow/core/framework/logging.h"
#include "tensorflow/core/framework/resource_mgr.h"
#include "tensorflow/core/lib/gtl/cleanup.h"
#include "tensorflow/core/platform/errors.h"
#include "tensorflow/core/platform/status.h"
#include "tensorflow/core/platform/threadpool_interface.h"
#include "tensorflow/core/platform/types.h"
#include "tensorflow/core/profiler/lib/traceme.h"
#include "tensorflow/core/runtime_fallback/kernel/kernel_fallback_compat_request_state.h"
#include "tensorflow/core/runtime_fallback/kernel/op_kernel_runner.h"
#include "tensorflow/core/runtime_fallback/runtime/kernel_utils.h"
#include "tensorflow/core/runtime_fallback/runtime/op_logger.h"
#include "tensorflow/core/runtime_fallback/util/attr_util.h"
#include "tensorflow/core/tfrt/utils/error_util.h"
#include "tensorflow/core/tfrt/utils/fallback_tensor.h"
#include "tensorflow/core/tfrt/utils/tensor_util.h"
#include "tfrt/core_runtime/execute_op_impl.h"  // from @tf_runtime
#include "tfrt/core_runtime/op_attrs.h"  // from @tf_runtime
#include "tfrt/host_context/async_dispatch.h"  // from @tf_runtime
#include "tfrt/host_context/async_value_ref.h"  // from @tf_runtime
#include "tfrt/host_context/chain.h"  // from @tf_runtime
#include "tfrt/host_context/execution_context.h"  // from @tf_runtime
#include "tfrt/host_context/kernel_registry.h"  // from @tf_runtime
#include "tfrt/host_context/sync_kernel_frame.h"  // from @tf_runtime
#include "tfrt/support/error_util.h"  // from @tf_runtime
#include "tfrt/support/forward_decls.h"  // from @tf_runtime
#include "tfrt/support/pointer_util.h"  // from @tf_runtime
#include "tfrt/support/string_util.h"  // from @tf_runtime
#include "tfrt/tensor/tensor.h"  // from @tf_runtime
#include "tfrt/tracing/tracing.h"  // from @tf_runtime

namespace tensorflow {
namespace tfd {
namespace {

using ::tfrt::AsyncValue;
using ::tfrt::AsyncValueRef;
using ::tfrt::Chain;
using ::tfrt::OpAttrsRef;
using ::tfrt::RCReference;
using ::tfrt::SmallVector;
using ::tfrt::string_view;

constexpr char kOpKernelRunnerTableResourceName[] =
    "OpKernelRunnerTableResourceName";

constexpr char kOpKernelRunnerCacheResourceName[] =
    "OpKernelRunnerCacheResourceName";

constexpr char kFallbackResourceArray[] = "FallbackResourceArray";

void KernelFallbackEmitError(
    const tfrt::ExecutionContext& exec_ctx, tfrt::string_view op_name,
    tfrt::AsyncValueRef<tfrt::Chain>* op_chain,
    llvm::MutableArrayRef<tfrt::RCReference<tfrt::AsyncValue>> results,
    const tensorflow::Status& status) {
  // Set all results to error, with the correct TFRT error code according to the
  // error propagated from runtime fallback execution.
  auto error =
      EmitErrorAsync(exec_ctx,
                     tfrt::StrCat("error running kernel fallback kernel ",
                                  op_name, ": ", status.error_message()),
                     tfrt::ConvertTfErrorCodeToTfrtErrorCode(status));
  std::fill(results.begin(), results.end(), error);
  if (op_chain) *op_chain = std::move(error);
}

}  // namespace

Status SetUpKernelFallbackCompatRequestContext(
    tfrt::RequestContextBuilder* builder,
    const tensorflow::DeviceMgr* device_manager,
    const tensorflow::ProcessFunctionLibraryRuntime* pflr,
    tensorflow::thread::ThreadPoolInterface* user_intra_op_threadpool,
    const absl::optional<tfrt::ModelMetadata>& model_metadata) {
  DCHECK(builder);
  DCHECK(device_manager);
  DCHECK(pflr);

  auto* runner_table =
      builder->resource_context()->GetOrCreateResource<OpKernelRunnerTable>(
          kOpKernelRunnerTableResourceName);

  auto* resource_array =
      builder->resource_context()->GetOrCreateResource<FallbackResourceArray>(
          kFallbackResourceArray);

  builder->context_data().emplace<KernelFallbackCompatRequestState>(
      device_manager, builder->id(), runner_table, resource_array,
      user_intra_op_threadpool, model_metadata, pflr);

  return Status::OK();
}

Status SetUpKernelFallbackCompatRequestContext(
    tfrt::RequestContextBuilder* builder, OpKernelRunnerTable* runner_table,
    tensorflow::EagerContext* eager_context,
    tensorflow::thread::ThreadPoolInterface* user_intra_op_threadpool,
    const absl::optional<tfrt::ModelMetadata>& model_metadata) {
  auto* resource_array =
      builder->resource_context()->GetOrCreateResource<FallbackResourceArray>(
          kFallbackResourceArray);

  if (runner_table == nullptr)
    runner_table =
        builder->resource_context()->GetOrCreateResource<OpKernelRunnerTable>(
            kOpKernelRunnerTableResourceName);

  auto step_id = builder->id();

  auto& fallback_request_state =
      builder->context_data().emplace<KernelFallbackCompatRequestState>(
          eager_context->local_device_mgr(), step_id,
          tfrt::OwnedOrUnownedPtr<ScopedStepContainer>{
              eager_context->StepContainer()},
          eager_context->GetCollectiveExecutorHandle(),
          tensorflow::core::RefCountPtr<tensorflow::Rendezvous>(
              eager_context->RendezvousCreator()(step_id)),
          runner_table, resource_array, user_intra_op_threadpool,
          model_metadata, eager_context->pflr());

  fallback_request_state.set_log_device_placement(
      eager_context->LogDevicePlacement());

  return Status::OK();
}

static llvm::Expected<gtl::InlinedVector<tensorflow::Tensor, 4>>
ConvertInputTensors(llvm::ArrayRef<tfrt::Tensor*> arguments,
                    const tfrt::ExecutionContext& exec_ctx) {
  gtl::InlinedVector<tensorflow::Tensor, 4> input_tf_tensors;
  input_tf_tensors.reserve(arguments.size());
  for (tfrt::Tensor* argument : arguments) {
    auto expected_tf_tensor = TFRTTensorToTFTensor(*argument, exec_ctx.host());
    if (!expected_tf_tensor) {
      return tfrt::MakeStringError(
          tfrt::StrCat(expected_tf_tensor.takeError()));
    }
    input_tf_tensors.push_back(std::move(expected_tf_tensor.get()));
  }

  return input_tf_tensors;
}

static Status ValidateInputTypes(
    tfrt::string_view op_name,
    const gtl::InlinedVector<tensorflow::Tensor, 4>& input_tf_tensors,
    const DataTypeVector& input_types) {
  const size_t n_inputs = input_tf_tensors.size();

  if (input_types.size() != n_inputs) {
    return tensorflow::errors::InvalidArgument("expected ", input_types.size(),
                                               " inputs, got ", n_inputs);
  }

  for (size_t i = 0; i < n_inputs; ++i) {
    if (input_tf_tensors[i].dtype() != input_types[i]) {
      return tensorflow::errors::InvalidArgument(
          "cannot compute ", op_name.str(), " as input #", i, "(zero-based)",
          " was expected to be a ", DataTypeString(input_types[i]),
          " tensor but is a ", DataTypeString(input_tf_tensors[i].dtype()),
          " tensor");
    }
  }

  return Status::OK();
}

namespace {

// OpKernelRunState keeps the states needed for per-kernel execution.
struct OpKernelRunState {
  gtl::InlinedVector<tensorflow::Tensor, 4> input_tf_tensors;
  gtl::InlinedVector<tensorflow::TensorValue, 4> input_tf_tensor_values;
  OpKernelContext::Params params;

  OpKernelRunState() = default;
  OpKernelRunState(
      const gtl::InlinedVector<tensorflow::TensorValue, 4>& tensor_values,
      const OpKernelContext::Params& p) {
    // `input_tf_tensor_values` contains the reference to all tensor used,
    // while `input_tf_tensors` only contains those needs ownership so their
    // sizes may not match. For this copy assignment, we conservatively copy all
    // tensors.
    input_tf_tensors.reserve(tensor_values.size());
    for (const auto& tensor_value : tensor_values) {
      input_tf_tensors.push_back(*tensor_value.tensor);
    }
    for (auto& tensor : input_tf_tensors) {
      input_tf_tensor_values.emplace_back(&tensor);
    }

    // Since `input_tf_tensor_values` and `params` contains pointers to
    // `input_tf_tensors`, we need to change those pointers to the correct ones
    // after copying.
    params = p;
    params.inputs = &input_tf_tensor_values;
  }

  OpKernelRunState(const OpKernelRunState& other) = delete;
  OpKernelRunState& operator=(const OpKernelRunState& other) = delete;

  ~OpKernelRunState() = default;

  void SetUpParams(
      const OpKernelRunner& runner,
      const KernelFallbackCompatRequestState& fallback_request_state) {
    params.inputs = &input_tf_tensor_values;

    // Replace the thread pool device if the custom device is specified.
    //
    // The device handling is copied from below link:
    // http://cs/?q=f:common_runtime%2Fexecutor.cc:692%20package:piper&rcl=351575626
    if (auto* custom_device = fallback_request_state.custom_device()) {
      params.device = custom_device;
    } else {
      params.device = runner.device();
    }

    params.op_kernel = runner.op_kernel();
    // Still use original device's resource_manager.
    params.resource_manager = runner.resource_manager();
    params.input_alloc_attrs = &runner.input_alloc_attrs();
    params.output_attr_array = runner.output_alloc_attrs().data();
    params.step_container = fallback_request_state.step_container();
    // Following two parameters are used to support executing tf.data via
    // fallback.
    params.function_library = runner.function_library_runtime();
    params.runner = fallback_request_state.runner();
    params.collective_executor = fallback_request_state.collective_executor();
    params.rendezvous = fallback_request_state.rendezvous();
    params.session_metadata = &fallback_request_state.session_metadata();
    params.cancellation_manager = fallback_request_state.cancellation_manager();
  }
};

// Keep states needed by kernel execution in a thread local storage to avoid
// repeated reallocation and destruction of them.
OpKernelRunState& GetThreadLocalOpKernelRunState() {
  thread_local OpKernelRunState run_state;
  return run_state;
}

}  // namespace

// Execute a tensorflow::OpKernel Asynchronously. `kernel_runner` and
// `input_tf_tensors` are expected to be alive during the call to this function.
// Set result AsyncValues in `results` and return a Chain that indicates the
// execution completion of error otherwise.
template <typename TensorType>
static void KernelFallbackExecuteCompatAsyncInternal(
    const tfrt::ExecutionContext& exec_ctx, OpKernelRunState* run_state,
    const OpKernelRunner& kernel_runner,
    tfrt::AsyncValueRef<tfrt::Chain>* op_chain,
    llvm::MutableArrayRef<tfrt::RCReference<tfrt::AsyncValue>> results) {
  auto chain =
      tfrt::MakeUnconstructedAsyncValueRef<tfrt::Chain>(exec_ctx.host());
  if (op_chain) *op_chain = chain.CopyRef();

  // Allocate unconstructed result tensors and set them in the output `results`.
  llvm::SmallVector<AsyncValueRef<TensorType>, 4> result_refs;
  result_refs.reserve(results.size());
  for (auto& result : results) {
    result_refs.emplace_back(
        tfrt::MakeUnconstructedAsyncValueRef<TensorType>(exec_ctx.host()));
    result = result_refs.back().CopyRef();
  }

  struct AsyncState {
    explicit AsyncState(const OpKernelRunState& rs, int num_outputs)
        : run_state(rs.input_tf_tensor_values, rs.params),
          context(&run_state.params, num_outputs) {}

    OpKernelRunState run_state;
    OpKernelContext context;

    tfrt::AsyncValueRef<tfrt::Chain> chain;
    llvm::SmallVector<tfrt::AsyncValueRef<TensorType>, 4> result_refs;
  };

  DCHECK_EQ(results.size(), kernel_runner.op_kernel()->num_outputs());
  auto async_state = std::make_shared<AsyncState>(*run_state, results.size());
  async_state->chain = chain.CopyRef();
  async_state->result_refs = std::move(result_refs);

  auto* context_ptr = &async_state->context;

  auto done_callback = [async_state = std::move(async_state), exec_ctx]() {
    auto& context = async_state->context;

    if (!context.status().ok()) {
      auto diag = tfrt::EmitError(
          exec_ctx,
          {tfrt::StrCat("error running kernel fallback kernel ",
                        context.op_kernel().name(), ": ",
                        context.status().error_message())},
          tfrt::ConvertTfErrorCodeToTfrtErrorCode(context.status()));
      for (auto& result : async_state->result_refs) result.SetError(diag);
      async_state->chain.SetError(diag);
      return;
    }

    // Set payload and mark async values available in TFRT's thread.
    tfrt::EnqueueWork(exec_ctx, [async_state = std::move(async_state)]() {
      auto& context = async_state->context;
      for (int i = 0; i < context.num_outputs(); ++i) {
        async_state->result_refs[i].emplace(
            std::move(*context.mutable_output(i)));
      }
      async_state->chain.emplace();
    });
  };

  kernel_runner.RunAsync(context_ptr, std::move(done_callback));
}

// Execute a tensorflow::OpKernel synchronously. `kernel_runner` and
// `input_tf_tensors` are expected to be alive during the call to this function.
// Set result AsyncValues in `results` and return OK status on successfully
// finishing the execution. TensorType is expected to be convert-constructible
// from tensorflow::Tensor.
template <typename TensorType>
static void KernelFallbackExecuteCompatSyncInternal(
    const tfrt::ExecutionContext& exec_ctx, OpKernelRunState* run_state,
    const OpKernelRunner& kernel_runner,
    tfrt::AsyncValueRef<tfrt::Chain>* op_chain,
    llvm::MutableArrayRef<tfrt::RCReference<tfrt::AsyncValue>> results) {
  DCHECK_EQ(results.size(), kernel_runner.op_kernel()->num_outputs());
  OpKernelContext context(&run_state->params, results.size());
  kernel_runner.Run(&context);

  if (!context.status().ok()) {
    KernelFallbackEmitError(exec_ctx, kernel_runner.op_kernel()->name(),
                            op_chain, results, context.status());
    return;
  }

  for (int i = 0; i < context.num_outputs(); ++i) {
    results[i] = tfrt::MakeAvailableAsyncValueRef<TensorType>(
        std::move(*context.mutable_output(i)));
  }

  if (op_chain) *op_chain = tfrt::MakeAvailableAsyncValueRef<tfrt::Chain>();
}

static std::string PrintTfrtOpAttrsToString(const tfrt::OpAttrsRef& attrs) {
  std::string str;
  llvm::raw_string_ostream ss(str);
  attrs.Print(ss);
  ss.flush();
  return str;
}

tfrt::AsyncValueRef<tfrt::Chain> KernelFallbackExecuteCompatCoreRuntimeDispatch(
    const tfrt::ExecutionContext& exec_ctx, tfrt::string_view op_name,
    tfrt::string_view device_name, llvm::ArrayRef<tfrt::Tensor*> arguments,
    llvm::MutableArrayRef<tfrt::RCReference<tfrt::AsyncValue>> results,
    const tfrt::OpAttrsRef& attrs) {
  auto op_chain = tfrt::GetReadyChain(exec_ctx.host());
  tensorflow::Status status;

  const auto* fallback_request_state =
      exec_ctx.request_ctx()
          ->GetDataIfExists<KernelFallbackCompatRequestState>();
  if (!fallback_request_state) {
    status = tensorflow::errors::NotFound(
        "KernelFallbackCompatRequestState not found in RequestContext.");
    KernelFallbackEmitError(exec_ctx, op_name, &op_chain, results, status);
    return op_chain;
  }

  DCHECK(exec_ctx.location());

  DCHECK(exec_ctx.request_ctx()->resource_context());
  auto* runner_cache = exec_ctx.request_ctx()
                           ->resource_context()
                           ->GetOrCreateResource<OpKernelRunnerCache>(
                               kOpKernelRunnerCacheResourceName);

  auto kernel_runner_or_status = runner_cache->GetOrCreate(
      exec_ctx.location(), ToAbslStringView(op_name),
      ToAbslStringView(device_name), arguments.size(),
      [&attrs, host = exec_ctx.host()](
          tensorflow::AttrValueMap* attr_value_map) -> llvm::Error {
        VLOG(1) << "KernelFallbackExecuteCompat creating op from OpAttrs: "
                << PrintTfrtOpAttrsToString(attrs);
        return FillAttrValueMap(attrs, host, attr_value_map);
      },
      *fallback_request_state);

  if (!kernel_runner_or_status.ok()) {
    KernelFallbackEmitError(exec_ctx, op_name, &op_chain, results,
                            kernel_runner_or_status.status());
    return op_chain;
  }

  auto expected_input_tf_tensors = ConvertInputTensors(arguments, exec_ctx);
  if (!expected_input_tf_tensors) {
    status = tensorflow::errors::Internal(
        tfrt::StrCat(expected_input_tf_tensors.takeError()));
    KernelFallbackEmitError(exec_ctx, op_name, &op_chain, results, status);
    return op_chain;
  }

  auto& kernel_runner = kernel_runner_or_status.ValueOrDie();

  auto& run_state = GetThreadLocalOpKernelRunState();
  auto clean_up_inputs =
      gtl::MakeCleanup([&]() { run_state.input_tf_tensors.clear(); });

  auto& input_tf_tensors = run_state.input_tf_tensors;
  input_tf_tensors = std::move(expected_input_tf_tensors.get());

  // Check if input tensor dtypes are valid.
  status = ValidateInputTypes(op_name, input_tf_tensors,
                              kernel_runner->op_kernel()->input_types());

  // TODO(b/176997538): Skip checking dtypes for tf._BatchFunctionFallback op
  // due to b/176997538. Remove the skipping once the SavedModel lowering
  // problem is fixed.
  if (!status.ok() && !op_name.equals("_BatchFunctionFallback")) {
    KernelFallbackEmitError(exec_ctx, op_name, &op_chain, results, status);
    return op_chain;
  }

  auto& input_tf_tensor_values = run_state.input_tf_tensor_values;
  input_tf_tensor_values.resize(input_tf_tensors.size());
  for (int i = 0; i < input_tf_tensors.size(); ++i) {
    input_tf_tensor_values[i].tensor = &input_tf_tensors[i];
  }

  run_state.SetUpParams(*kernel_runner, *fallback_request_state);

  // TODO(b/166705169): Figure out how to properly fallback GPU kernels.
  if (kernel_runner->IsAsync()) {
    KernelFallbackExecuteCompatAsyncInternal<KernelFallbackTensor>(
        exec_ctx, &run_state, *kernel_runner, &op_chain, results);
  } else {
    KernelFallbackExecuteCompatSyncInternal<KernelFallbackTensor>(
        exec_ctx, &run_state, *kernel_runner, &op_chain, results);
  }

  return op_chain;
}

llvm::Expected<Device*> GetTfDevice(const tfrt::ExecutionContext& exec_ctx,
                                    const tfrt::Device& device) {
  auto* fallback_request_state =
      exec_ctx.request_ctx()
          ->GetDataIfExists<KernelFallbackCompatRequestState>();
  if (!fallback_request_state) {
    return tfrt::MakeStringError(
        "KernelFallbackCompatRequestState not found in RequestContext.");
  }
  Device* tf_device;
  Status s = fallback_request_state->device_manager().LookupDevice(
      device.name().data(), &tf_device);
  if (!s.ok()) {
    return tfrt::MakeStringError(s.error_message());
  }
  return tf_device;
}

static absl::string_view StripTfPrefix(tfrt::string_view op_name) {
  return absl::StripPrefix(ToAbslStringView(op_name), "tf.");
}

// Generate metadata for an execution op event
std::string GetTracingMetadata(llvm::ArrayRef<tfrt::AsyncValue*> args,
                               const tfrt::ExecutionContext& exec_ctx,
                               const OpKernelRunner& kernel_runner) {
  auto request_id = exec_ctx.request_ctx()->id();
  auto current_tracing_level = tfrt::tracing::GetCurrentTracingLevel();

  if (current_tracing_level == tfrt::tracing::TracingLevel::Default) {
    return profiler::TraceMeEncode({{"id", request_id}});
  }

  // Get Long Name
  auto debug_info = exec_ctx.location().GetDebugInfo();
  auto long_name = debug_info.hasValue() ? debug_info.getValue().info : "";

  if (current_tracing_level == tfrt::tracing::TracingLevel::Verbose) {
    return profiler::TraceMeEncode(
        {{"id", request_id}, {"long_name", ToAbslStringView(long_name)}});
  }

  // Get Input Tensors
  std::string input_string;
  llvm::raw_string_ostream input_string_stream(input_string);

  for (size_t i = 0; i < args.size(); ++i) {
    const auto& tensor = args[i]->get<Tensor>();
    input_string_stream << DataTypeString(tensor.dtype())
                        << tensor.shape().DebugString();
    input_string_stream << ";";
  }

  // Get Attributes
  std::string attr_string;
  llvm::raw_string_ostream attr_string_stream(attr_string);

  for (const auto& entry : kernel_runner.op_kernel()->def().attr()) {
    attr_string_stream << entry.first << ": {" << entry.second.DebugString();
    if (!attr_string.empty() && attr_string[attr_string.size() - 1] == '\n') {
      attr_string[attr_string.size() - 1] = '}';
    }
    attr_string_stream << ";\n";
  }

  return profiler::TraceMeEncode({
      {"id", request_id},
      {"long_name", ToAbslStringView(long_name)},
      {"inputs", input_string},
      {"attributes", attr_string},
  });
}

namespace {

class FallbackKernelAttributeFrame {
 public:
  explicit FallbackKernelAttributeFrame(tfrt::AsyncKernelFrame* frame)
      : frame_(frame) {
    DCHECK(frame_);
  }

  tfrt::StringAttr device() const {
    return tfrt::StringAttr(frame_->GetAttribute(kDeviceAttrPosition));
  }

  tfrt::AggregateAttr op_attr() const {
    return tfrt::AggregateAttr(frame_->GetAttribute(kOpAttrPosition));
  }

  tfrt::AggregateAttr op_func_attr() const {
    return tfrt::AggregateAttr(frame_->GetAttribute(kOpFuncAttrPosition));
  }

  tfrt::I64Attr op_key() const {
    return tfrt::I64Attr(frame_->GetAttribute(kOpKeyAttrPosition));
  }

  tfrt::StringAttr op_name() const {
    return tfrt::StringAttr(frame_->GetAttribute(kOpNameAttrPosition));
  }

 private:
  static constexpr int kDeviceAttrPosition = 0;
  static constexpr int kOpAttrPosition = 1;
  static constexpr int kOpFuncAttrPosition = 2;
  static constexpr int kOpKeyAttrPosition = 3;
  static constexpr int kOpNameAttrPosition = 4;

  tfrt::AsyncKernelFrame* frame_ = nullptr;
};

// The BEF kernel for kernel fallback compat mode. The arguments and results are
// expected to tensorflow::tfrt_stub::FallbackTensor.
TF_ATTRIBUTE_ALWAYS_INLINE static void KernelFallbackExecuteOp(
    llvm::ArrayRef<tfrt::AsyncValue*> args,
    llvm::MutableArrayRef<tfrt::RCReference<tfrt::AsyncValue>> results,
    tfrt::AsyncValueRef<tfrt::Chain>* op_chain,
    const FallbackKernelAttributeFrame& frame,
    const tfrt::ExecutionContext& exec_ctx) {
  tensorflow::profiler::TraceMe trace_me(
      [&]() { return ToAbslStringView(frame.op_name().GetValue()); });

  const auto* fallback_request_state =
      exec_ctx.request_ctx()
          ->GetDataIfExists<KernelFallbackCompatRequestState>();
  if (!fallback_request_state) {
    KernelFallbackEmitError(
        exec_ctx, frame.op_name().GetValue(), op_chain, results,
        tensorflow::errors::NotFound(
            "KernelFallbackCompatRequestState not found in RequestContext."));
    return;
  }

  auto* runner_table = fallback_request_state->runner_table();
  DCHECK(runner_table);

  auto* kernel_runner = runner_table->Get(frame.op_key().GetValue());
  DCHECK(kernel_runner);
  DCHECK_EQ(kernel_runner->op_kernel()->name(),
            StripTfPrefix(frame.op_name().GetValue()));

  trace_me.AppendMetadata(
      [&]() { return GetTracingMetadata(args, exec_ctx, *kernel_runner); });

  if (fallback_request_state->log_device_placement() || VLOG_IS_ON(1)) {
    string msg =
        strings::StrCat("Executing op ", frame.op_name().GetValue().str(),
                        " in device ", frame.device().GetValue().str());
    if (!logging::LogToListeners(msg)) {
      LOG(INFO) << msg;
    }
  }

  auto& run_state = GetThreadLocalOpKernelRunState();
  auto clean_up_inputs =
      gtl::MakeCleanup([&]() { run_state.input_tf_tensors.clear(); });

  // Prepare the input tensors.
  auto& input_tf_tensors = run_state.input_tf_tensors;
  auto& input_tf_tensor_values = run_state.input_tf_tensor_values;
  DCHECK(input_tf_tensors.empty());
  input_tf_tensor_values.resize(args.size());
  for (int i = 0; i < args.size(); ++i) {
    auto* arg = args[i];
    auto& fallback_tensor = arg->get<tensorflow::tfrt_stub::FallbackTensor>();
    // If the argument is immutable or unique, we can just keep the reference
    // without copying that invovles expensive atomic reference counting. And if
    // the argument is unique but mutable, then tensorflow optimizations like
    // buffer forwarding can be utilized. Otherwise, we conservatively copy the
    // tensor.
    if (!fallback_tensor.is_immutable() && !arg->IsUnique()) {
      input_tf_tensors.push_back(fallback_tensor.tensor());
    }
    input_tf_tensor_values[i].tensor = &fallback_tensor.tensor();
  }

  run_state.SetUpParams(*kernel_runner, *fallback_request_state);

  if (kernel_runner->IsAsync()) {
    KernelFallbackExecuteCompatAsyncInternal<
        tensorflow::tfrt_stub::FallbackTensor>(
        exec_ctx, &run_state, *kernel_runner, op_chain, results);
  } else {
    KernelFallbackExecuteCompatSyncInternal<
        tensorflow::tfrt_stub::FallbackTensor>(
        exec_ctx, &run_state, *kernel_runner, op_chain, results);
  }
}

// The BEF kernel for creating tensorflow::OpKernel to be used in kernel
// fallback compat mode.
llvm::Expected<tfrt::Chain> KernelFallbackCreateOp(
    const tfrt::Chain& in_ch, tfrt::StringAttr device, tfrt::I64Attr num_args,
    tfrt::AggregateAttr op_attr_array, tfrt::AggregateAttr op_func_attr_array,
    tfrt::I64Attr op_key, tfrt::StringAttr op_name_attr,
    const tfrt::ExecutionContext& exec_ctx) {
  const auto* fallback_request_state =
      exec_ctx.request_ctx()
          ->GetDataIfExists<KernelFallbackCompatRequestState>();
  if (!fallback_request_state) {
    return tfrt::MakeStringError(
        "KernelFallbackCompatRequestState not found in RequestContext.");
  }

  auto* runner_table = fallback_request_state->runner_table();
  DCHECK(runner_table);

  auto attr_builder =
      [op_attr_array, op_func_attr_array](
          tensorflow::AttrValueMap* attr_value_map) -> llvm::Error {
    auto status =
        SetUpAttrValueMap(op_attr_array, op_func_attr_array, attr_value_map);

    if (!status.ok()) return tfrt::MakeStringError(status.error_message());
    return llvm::Error::success();
  };

  auto op_name = StripTfPrefix(op_name_attr.GetValue());

  auto statusor_runner = OpKernelRunner::Create(
      op_name, ToAbslStringView(device.GetValue()), num_args.GetValue(),
      attr_builder, *fallback_request_state);
  if (!statusor_runner.ok())
    return tfrt::MakeStatusError(statusor_runner.status());

  if (!runner_table->Insert(op_key.GetValue(),
                            std::move(statusor_runner).ValueOrDie())) {
    return tfrt::MakeStringError(
        absl::StrCat("KernelFallbackCreateOp: OpKernelRunner already exists: ",
                     op_name_attr.GetValue().str()));
  }

  return tfrt::Chain();
}

// FallbackSetResource is the fallback kernel that sets the tensor value in the
// fallback's resource array.
llvm::Expected<tfrt::Chain> FallbackSetResource(
    tfrt::Argument<tfrt::Chain> in_ch,
    tfrt::Argument<tensorflow::tfrt_stub::FallbackTensor> arg,
    tfrt::StringAttr device, tfrt::I64Attr index_attr,
    const tfrt::ExecutionContext& exec_ctx) {
  const auto* fallback_request_state =
      exec_ctx.request_ctx()
          ->GetDataIfExists<KernelFallbackCompatRequestState>();
  if (!fallback_request_state) {
    return tfrt::MakeStringError(
        "KernelFallbackCompatRequestState not found in RequestContext.");
  }

  auto* resource_array = fallback_request_state->resource_array();
  DCHECK(resource_array);

  int64_t index = index_attr.GetValue();

  // Setting the resource tensor to be immutable, so that we don't need
  // reference counting on it and that it cannot be buffer-forwarded.
  resource_array->SetResource(
      index,
      tensorflow::tfrt_stub::ImmutableTensor::Create(arg.get().tensor()));

  return tfrt::Chain();
}

// FallbackGetResource is the fallback kernel that retrieves the tensor value in
// the fallback's resource array.
void FallbackGetResource(tfrt::Argument<tfrt::Chain> in_ch,
                         tfrt::Result<tfrt::Chain> out_ch,
                         tfrt::RemainingResults results,
                         tfrt::StringAttr device, tfrt::ArrayAttr indices_attr,
                         const tfrt::ExecutionContext& exec_ctx) {
  tensorflow::profiler::TraceMe trace_me("tfrt_fallback_async.get_resource");
  trace_me.AppendMetadata([request_id = exec_ctx.request_ctx()->id()]() {
    return tensorflow::profiler::TraceMeEncode({{"id", request_id}});
  });

  const auto* fallback_request_state =
      exec_ctx.request_ctx()
          ->GetDataIfExists<KernelFallbackCompatRequestState>();
  if (!fallback_request_state) {
    tfrt::RCReference<tfrt::AsyncValue> error = tfrt::EmitErrorAsync(
        exec_ctx,
        "KernelFallbackCompatRequestState not found in RequestContext.");
    out_ch.Set(std::move(error));
    return;
  }

  auto* resource_array = fallback_request_state->resource_array();
  DCHECK(resource_array);

  llvm::ArrayRef<int64_t> indices = indices_attr.GetValue<int64_t>();

  for (int i = 0; i < indices.size(); ++i) {
    results[i] = tfrt::FormRef(resource_array->GetResource(indices[i]));
  }

  out_ch.Set(in_ch);
}

// The implementation of tfrt_fallback_async.executeop kernel. It executes a
// non-side-effecting TF op with the name of `op_name` in fallback. All relevant
// TF attributes are passed in `op_attr_array`.
void FallbackAsyncExecuteOp(tfrt::AsyncKernelFrame* frame) {
  FallbackKernelAttributeFrame attr_frame(frame);
#ifndef NDEBUG
  frame->GetExecutionContext()
      .host()
      ->GetOrCreateSharedContext<OpLogger>()
      .LogOp(attr_frame.op_name().GetValue());
#endif
  KernelFallbackExecuteOp(frame->GetArguments(), frame->GetResults(),
                          /*op_chain=*/nullptr, attr_frame,
                          frame->GetExecutionContext());
}

// The implementation of tfrt_fallback_async.executeop.seq kernel. It executes a
// side-effecting TF op with the name of `op_name` in fallback. All relevant
// TF attributes are passed in `op_attr_array`. `in_op_chain` and `out_op_chain`
// are used for side-effect visibility.
void FallbackAsyncExecuteOpSeq(tfrt::AsyncKernelFrame* frame) {
  auto all_args = frame->GetArguments();
  DCHECK_GT(all_args.size(), 0);
  tfrt::AsyncValueRef<tfrt::Chain> op_chain(tfrt::FormRef(all_args[0]));
  llvm::ArrayRef<tfrt::AsyncValue*> args = all_args.drop_front();

  auto all_results = frame->GetResults();
  DCHECK_GT(all_results.size(), 0);
  auto& out_op_chain = all_results[0];
  llvm::MutableArrayRef<tfrt::RCReference<tfrt::AsyncValue>> results =
      all_results.drop_front();

  KernelFallbackExecuteOp(args, results, &op_chain,
                          FallbackKernelAttributeFrame(frame),
                          frame->GetExecutionContext());
  out_op_chain = std::move(op_chain);
}

void FallbackCopyTensorIfSmall(
    tfrt::Argument<tensorflow::tfrt_stub::FallbackTensor> arg,
    tfrt::RemainingResults results) {
  const auto& fallback_tensor = arg.get();
  const auto& tensor = fallback_tensor.tensor();

  if (!fallback_tensor.is_immutable()) {
    // Create a new TensorBuffer which contains a new atomic counter for each
    // result, to avoid downstream threads contending the original atomic
    // counter.
    for (int i = 0; i < results.size(); ++i) {
      auto immutable_tensor =
          tensorflow::tfrt_stub::ImmutableTensor::Create(tensor);
      results[i] = tfrt::MakeAvailableAsyncValueRef<
          tensorflow::tfrt_stub::FallbackTensor>(
          std::move(immutable_tensor.tensor()));
    }
  } else {
    // For immutable tensors, we just need to copy the pointer. Note that we
    // still create a new AsyncValue in this case, to avoid atomic contention on
    // AsyncValue's refcount.
    for (int i = 0; i < results.size(); ++i) {
      results[i] = tfrt::MakeAvailableAsyncValueRef<
          tensorflow::tfrt_stub::FallbackTensor>(fallback_tensor);
    }
  }
}

llvm::Expected<tensorflow::tfrt_stub::FallbackTensor> ConstTensorProto(
    tfrt::StringAttr serialized_tensor_proto) {
  tensorflow::TensorProto tensor_proto;
  if (!tensor_proto.ParseFromString(serialized_tensor_proto.GetValue().str())) {
    return tfrt::MakeStringError("Failed to parse const tensor proto");
  }

  tensorflow::Tensor tensor;
  if (!tensor.FromProto(tensor_proto)) {
    return tfrt::MakeStringError("Failed to create tensor from tensor proto: ",
                                 tensor_proto.ShortDebugString());
  }

  return tensorflow::tfrt_stub::FallbackTensor(std::move(tensor));
}

void RegisterKernelFallbackCompatKernels(tfrt::KernelRegistry* registry) {
  registry->AddKernel("tfrt_fallback_async.const_tensor_proto",
                      TFRT_KERNEL(ConstTensorProto));
  registry->AddKernel("tfrt_fallback_async.executeop", FallbackAsyncExecuteOp);
  registry->AddKernel("tfrt_fallback_async.executeop.seq",
                      FallbackAsyncExecuteOpSeq);
  registry->AddKernel("tfrt_fallback_async.copy_if_small",
                      TFRT_KERNEL(FallbackCopyTensorIfSmall));
  registry->AddKernel("tfrt_fallback_async.createop",
                      TFRT_KERNEL(KernelFallbackCreateOp));
  registry->AddKernel("tfrt_fallback_async.set_resource",
                      TFRT_KERNEL(FallbackSetResource));
  registry->AddKernel("tfrt_fallback_async.get_resource",
                      TFRT_KERNEL(FallbackGetResource));
}

TFRT_STATIC_KERNEL_REGISTRATION(RegisterKernelFallbackCompatKernels);

}  // namespace
}  // namespace tfd
}  // namespace tensorflow
