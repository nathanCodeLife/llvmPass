#ifndef LLVM_TRANSFORMS_SCALAR_LOOPMERGE_H
#define LLVM_TRANSFORMS_SCALAR_LOOPMERGE_H

#include "llvm/IR/PassManager.h"

namespace llvm {

class Function;

class LoopMergePass : public PassInfoMixin<LoopMergePass> {
public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
  };
}
#endif
