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

#include "absl/memory/memory.h"
#include "absl/strings/str_split.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/Regex.h"
#include "llvm/Support/raw_ostream.h"
#include "mlir/Dialect/QuantOps/FakeQuantSupport.h"  // TF:local_config_mlir
#include "mlir/Dialect/QuantOps/QuantOps.h"  // TF:local_config_mlir
#include "mlir/Dialect/StandardOps/Ops.h"  // TF:local_config_mlir
#include "mlir/IR/AffineExpr.h"  // TF:local_config_mlir
#include "mlir/IR/AffineMap.h"  // TF:local_config_mlir
#include "mlir/IR/Attributes.h"  // TF:local_config_mlir
#include "mlir/IR/Location.h"  // TF:local_config_mlir
#include "mlir/IR/PatternMatch.h"  // TF:local_config_mlir
#include "mlir/IR/StandardTypes.h"  // TF:local_config_mlir
#include "mlir/Pass/Pass.h"  // TF:local_config_mlir
#include "mlir/Support/Functional.h"  // TF:local_config_mlir
#include "mlir/Support/LLVM.h"  // TF:local_config_mlir
#include "tensorflow/compiler/mlir/lite/quantization/quantization_info.pb.h"
#include "tensorflow/compiler/mlir/lite/quantization/quantization_passes.h"
#include "tensorflow/compiler/mlir/tensorflow/utils/import_utils.h"

// NOLINTNEXTLINE
static llvm::cl::opt<std::string> quantize_stats(
    "quant-test-stats", llvm::cl::value_desc("string"),
    llvm::cl::desc("serialized quant info string. Only used in tests"),
    llvm::cl::init(""));

//===----------------------------------------------------------------------===//
// The Pass to import quantization stats to the ops in a function. This requires
// a custom method to retrieve the unique name of the operation.

namespace mlir {
namespace quant {

using QuantParamsEntry = QuantizationInfo::QuantParams;

namespace {
class ImportQuantStatsPass : public FunctionPass<ImportQuantStatsPass> {
 public:
  explicit ImportQuantStatsPass(OperationToName op_to_name)
      : op_to_name_(op_to_name) {}

  void runOnFunction() override;

  // Parses the serialized quant stats protobuf and initialize the internal
  // data structure. This method must be called after the pass is created.
  bool ParseQuantStats(const std::string &stats_str);

 private:
  void ImportAsStatsOps(OpBuilder b, Operation *op, int index,
                        const QuantParamsEntry &info);

  void InsertStatsOpAtResult(OpBuilder b, Value *res, ElementsAttr layer_stats,
                             ElementsAttr axis_stats, IntegerAttr axis);

  // If the index is out of range, this method returns false. Otherwise it
  // returns true if the value is a float tensor.
  bool IsQuantizableResult(Operation *op, int index) {
    if (index < 0 || index >= op->getNumResults()) return false;
    Value *res = op->getResult(index);
    return res->getType().isa<ShapedType>() &&
           res->getType().cast<ShapedType>().getElementType().isa<FloatType>();
  }

  // A method to retrive the name for the given op.
  OperationToName op_to_name_;

  // We split the normal names and regex names, since the former can use hash
  // map to lookup and the latter needs to iterate all the regex to find the
  // match.
  // The `int` in the following two containers are to specify the result index
  // of the given op. -1 indicates all the floating-point results.
  llvm::StringMap<std::pair<int, const QuantParamsEntry>> name_to_info_;
  llvm::StringMap<std::pair<int, const QuantParamsEntry>> regex_to_info_;
};
}  // namespace

bool ImportQuantStatsPass::ParseQuantStats(const std::string &stats_str) {
  QuantizationInfo quant_stats;
  if (!tensorflow::LoadProtoFromBuffer(stats_str, &quant_stats).ok()) {
    return true;
  }

  for (const auto &entry : quant_stats.entries()) {
    if (!entry.name().empty()) {
      std::vector<std::string> name_and_port =
          absl::StrSplit(entry.name(), ':');
      int port = name_and_port.size() == 2 ? std::stoi(name_and_port[1]) : -1;
      name_to_info_.insert({name_and_port[0], {port, entry}});
    } else if (!entry.name_regex().empty()) {
      std::vector<std::string> name_and_port =
          absl::StrSplit(entry.name_regex(), ':');
      int port = name_and_port.size() == 2 ? std::stoi(name_and_port[1]) : -1;
      regex_to_info_.insert({name_and_port[0], {port, entry}});
    }
  }
  return false;
}

void ImportQuantStatsPass::InsertStatsOpAtResult(OpBuilder b, Value *res,
                                                 ElementsAttr layer_stats,
                                                 ElementsAttr axis_stats,
                                                 IntegerAttr axis) {
  auto stats_op = b.create<quant::StatisticsOp>(b.getUnknownLoc(), res,
                                                layer_stats, axis_stats, axis);
  res->replaceAllUsesWith(stats_op);
  stats_op.getOperation()->replaceUsesOfWith(stats_op, res);
}

void ImportQuantStatsPass::ImportAsStatsOps(OpBuilder b, Operation *op,
                                            int index,
                                            const QuantParamsEntry &info) {
  if (info.params_size() == 0) return;

  SmallVector<APFloat, 4> min_maxs;
  min_maxs.reserve(info.params_size() * 2);
  for (const auto &param : info.params()) {
    llvm::APFloat min(param.min_max().min());
    llvm::APFloat max(param.min_max().max());
    min_maxs.push_back(min);
    min_maxs.push_back(max);
  }
  // The layer stats contain only the first min/max pairs.
  ElementsAttr layer_stats = DenseFPElementsAttr::get(
      b.getTensorType({2}, b.getF32Type()), {min_maxs[0], min_maxs[1]});
  ElementsAttr axis_stats;
  IntegerAttr axis;

  if (info.params_size() > 1) {
    SmallVector<int64_t, 4> axis_stats_shape{info.params_size(), 2};
    axis_stats = DenseFPElementsAttr::get(
        b.getTensorType(axis_stats_shape, b.getF32Type()), min_maxs);
    axis = b.getI64IntegerAttr(info.meta().quantize_axis());
  }

  b.setInsertionPointAfter(op);
  if (IsQuantizableResult(op, index)) {
    InsertStatsOpAtResult(b, op->getResult(index), layer_stats, axis_stats,
                          axis);
  } else {
    for (int i = 0; i < op->getNumResults(); ++i) {
      if (IsQuantizableResult(op, i)) {
        InsertStatsOpAtResult(b, op->getResult(i), layer_stats, axis_stats,
                              axis);
      }
    }
  }
}

void ImportQuantStatsPass::runOnFunction() {
  FuncOp func = getFunction();
  OpBuilder builder(func);

  func.walk([&](Operation *op) {
    if (op->isKnownTerminator()) return;
    auto op_name = op_to_name_(op);

    // Check the named info collection first.
    auto it = name_to_info_.find(op_name);
    if (it != name_to_info_.end()) {
      ImportAsStatsOps(builder, op, it->second.first, it->second.second);
      return;
    }

    // Iterate all the regex names and matches the first one.
    for (auto &regex : regex_to_info_) {
      if (llvm::Regex(regex.first()).match(op_name)) {
        ImportAsStatsOps(builder, op, regex.second.first, regex.second.second);
        break;
      }
    }
  });
}

// Creates an instance of the default quant parameters pass.
std::unique_ptr<OpPassBase<FuncOp>> CreateImportQuantStatsPass(
    OperationToName op_to_name, const std::string &stats_str) {
  auto pass = absl::make_unique<ImportQuantStatsPass>(op_to_name);
  if (pass->ParseQuantStats(stats_str)) return nullptr;
  return pass;
}

// Creates an instance pass to import quantization stats to the operations in
// the function. A custom method to get the name from the op is used because
// different dialect ops might have different ways to assign the name.
std::unique_ptr<OpPassBase<FuncOp>>
CreateImportQuantStatsPassForTFControlDialect(const std::string &stats_str) {
  auto get_name_func = [](Operation *op) {
    if (auto name = op->getAttrOfType<StringAttr>("name"))
      return name.getValue();
    else
      return llvm::StringRef("");
  };

  return CreateImportQuantStatsPass(get_name_func, stats_str);
}

// Registers this pass with default values, only for test
static PassRegistration<ImportQuantStatsPass> pass(
    "quant-import-stats", "Import quantization stats to the model", [] {
      return CreateImportQuantStatsPassForTFControlDialect(quantize_stats);
    });

}  // namespace quant
}  // namespace mlir
