//===------------------------ writeExpressions.cpp ------------------------===//
//
// This file is distributed under the Universidade Federal de Minas Gerais - 
// UFMG Open Source License. See LICENSE.TXT for details.
//
// Copyright (C) 2015   Gleison Souza Diniz Mendon?a
//
//===----------------------------------------------------------------------===//
//
// WriteExpressions is an optimization that insert into the source file every
// struct to data manipulation for use automatic parallelization.
// This pass translate the access expressions in LLVM's I.R. and use the
// original name of variables to write the correct parallel code.
//
// The name of variables generated by pass stay in VETNAME string. The 
// programmer can change this name in writeExpressions.h
//
// To use this pass please use the flag "-writeExpressions", see the example
// available below:
//
// opt -load ${LIBR}/libLLVMArrayInference.so -writeExpressions ${BENCH}/$2.bc 
//
// The ambient variables and your signification:
//   -- LIBRNIKE => Set the location of NIKE library.
//   -- LIBR => Set the location of ArrayInference tool location.
//   -- BENCH => Set the benchmark's paste.
// 
//===----------------------------------------------------------------------===//

#include <fstream>
#include <queue>

#include "llvm/Analysis/RegionInfo.h"  
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/DIBuilder.h" 
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/DataTypes.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/ADT/Statistic.h"

#include "PtrRangeAnalysis.h"

#include "writeExpressions.h"

using namespace llvm;
using namespace std;
using namespace lge;

#define DEBUG_TYPE "writeExpressions"
#define ERROR_VALUE -1
#define ACC '0'
#define OMP_GPU '1'
#define OMP_CPU '2'

STATISTIC(numL , "Number of loops");
STATISTIC(numAL , "Number of analyzable loops");
STATISTIC(numWL , "Number of annotated loops"); 
STATISTIC(numFLC , "Number of safe call instructions inside loops");

static cl::opt<bool> ClEmitParallel("Emit-Parallel",
    cl::Hidden, cl::desc("Use Loop Parallel Analysis to anotate."));

static cl::opt<char> ClEmitOMP("Emit-OMP",
    cl::Hidden, cl::desc("Use opemmp directives."));

static cl::opt<std::string> ClInput("Parallel-File", cl::Hidden, 
    cl::desc("Use Extern information to insert parallel pragmas."));

static cl::opt<bool> ClDivergent("Discard-Divergent",
    cl::desc("Discarts parallel loops when divergences are found."));

static cl::opt<bool> ClCoalescing("Memory-Coalescing", 
    cl::desc("Annotate Pragmas using data coallesing."));

void WriteExpressions::analyzeCalls (Loop *L) {
  if (!isLoopAnalyzable(L))
    return;
  for (auto BB = L->block_begin(), BE = L->block_end(); BB != BE; BB++)
    for (auto I = (*BB)->begin(), IE = (*BB)->end(); I != IE; I++)
      if (CallInst *CI = dyn_cast<CallInst>(I))
        if (CI->getName() != std::string())  
          numFLC++;
}

void WriteExpressions::readParallelLoops () {
  fstream Infile(ClInput.c_str());
  std::string Line;
  while (!Infile.eof()) {
    Line = std::string(); 
    std::getline(Infile, Line);
    unsigned int nLine = 0;
    for (unsigned int i = 0, ie = Line.size(); i != ie; i++) {
      nLine *= 10;
      nLine += (Line[i] - '0');
    }
    std::string pragma = "#pragma acc loop independent\n";
    if ((ClEmitOMP == OMP_GPU) || (ClEmitOMP == OMP_CPU))
      pragma = "#pragma omp parallel for\n";
    addCommentToLine (pragma, nLine);
  }
}

void WriteExpressions::addCommentToLine (std::string Comment,
                                         unsigned int Line) {     
  if (Comments.count(Line) == 0)
    Comments[Line] = Comment;
  else if (Comments[Line].find(Comment) == std::string::npos)
    Comments[Line] += Comment;
} 

void WriteExpressions::copyComments (std::map <unsigned int, std::string>
                                      CommentsIn) {
  for (auto I = CommentsIn.begin(), E = CommentsIn.end(); I != E; ++I)
    addCommentToLine(I->second,I->first);
} 

int WriteExpressions::getLineNo (Value *V) {
  if (!V)
    return ERROR_VALUE;
  if (Instruction *I = dyn_cast<Instruction>(V))
    if (I)
      if (MDNode *N = I->getMetadata("dbg"))
        if (N)
          if (DILocation *DI = dyn_cast<DILocation>(N))
            return DI->getLine();
  return ERROR_VALUE;
}

void WriteExpressions::clearExpression () {
  Expression.erase(Expression.begin(),Expression.end());
}

std::string WriteExpressions::getUniqueString () {
  std::string result = "";
  for (unsigned int i = 0; i < Expression.size(); i++) {
    result += Expression[i];
  }
  return result;
}

void WriteExpressions::denotateLoopParallel (Loop *L, std::string condition) {
  std::string pragma = "#pragma acc loop independent " + condition + "\n";
  if ((ClEmitOMP == OMP_GPU) || (ClEmitOMP == OMP_CPU) )
    pragma = "#pragma omp parallel for " + condition + "\n";
  BasicBlock *BB = L->getLoopLatch();
  MDNode *MD = nullptr;
  MDNode *MDDivergent = nullptr;
  if (BB == nullptr)
    return;
  MD = BB->getTerminator()->getMetadata("isParallel");
  MDDivergent = BB->getTerminator()->getMetadata("isDivergent");
  if (ClDivergent && MDDivergent != nullptr)
    return;
  if (!MD)
    return;
  int line = L->getStartLoc()->getLine();
  numWL++;
  addCommentToLine(pragma, line);
  for (Loop *SubLoop : L->getSubLoops())
    denotateLoopParallel(SubLoop, condition);
}

bool WriteExpressions::isLoopParallel (Loop *L) {
  BasicBlock *BB = L->getLoopLatch();
  MDNode *MD = nullptr;
  MDNode *MDDivergent = nullptr;
  if (BB == nullptr)
    return false;
  MD = BB->getTerminator()->getMetadata("isParallel");
  MDDivergent = BB->getTerminator()->getMetadata("isDivergent");
  if (!MD)
    return false;
  if (ClDivergent && MDDivergent != nullptr)
    return false;
  return true;
}

bool WriteExpressions::hasLoopParallel (Region *R) {
  for (Region::block_iterator B = R->block_begin(), BE = R->block_end();
       B != BE; B++)
    if (B->getTerminator()->getMetadata("isParallel"))
      return true;
  return false;
}

void WriteExpressions::marknumAL (Loop *L) {
  numAL++;
  for (Loop *SubLoop : L->getSubLoops())
    marknumAL(SubLoop);
} 

int WriteExpressions::returnLoopEndLine (Loop *L) {
  // Using the line of the first instruction 
  BasicBlock *B = L->getUniqueExitBlock();
  if (!B)
    return ERROR_VALUE;
  int line = ERROR_VALUE, maxLine = ERROR_VALUE;
  for (auto BB = L->block_begin(), BE = L->block_end(); BB != BE; BB++) {
    for (auto I = (*BB)->begin(), IE = (*BB)->end(); I != IE; I++) {
      line = getLineNo(I);
      if ((line != ERROR_VALUE) && (line > maxLine))
        maxLine = line;
    }
  }
  // Increase the value to return the next line outise the loop.
  if (maxLine != ERROR_VALUE)
    maxLine++;
  return maxLine;
}

int WriteExpressions::returnRegionEndLine (Region *R) {
  int line = ERROR_VALUE, maxLine = ERROR_VALUE;
  for (auto BB = R->block_begin(), BE = R->block_end(); BB != BE; BB++) {
    for (auto I = (*BB)->begin(), IE = (*BB)->end(); I != IE; I++) {
      line = getLineNo(I);
      if ((line != ERROR_VALUE) && (line > maxLine)) {
        maxLine = line;
      }
    }
  }
  if (maxLine != ERROR_VALUE)
    maxLine++;
  return maxLine;
}

int WriteExpressions::returnRegionStartLine (Region *R) {
  int line = INT_MAX, minLine = INT_MAX;
  std::map<Loop*, bool> loops;
  for (auto BB = R->block_begin(), BE = R->block_end(); BB != BE; BB++) {
    Loop *L = li->getLoopFor(*BB);
    if (L && loops.count(L) == 0)
      loops[L] = true;
  }
  
  for (auto L = loops.begin(), LE = loops.end(); L != LE; L++) {
    line = L->first->getStartLoc()->getLine();
    if (line < minLine)
      minLine = line;
  }
  return minLine;
}

void WriteExpressions::marknumWL (Loop *L) {
  numWL++;
  for (Loop *SubLoop : L->getSubLoops())
    marknumWL(SubLoop);
}

void WriteExpressions::regionIdentify (Region *R) {
  // For each region of function, we need run this void and call the void for
  // every loop in the program. Or if this region R are in loop, we need write
  // the pragmas here.
  // Here, we will know the loop those region.
  Loop *l = li->getLoopFor(*(R->block_begin()));

  if (!l || (!isLoopParallel(l) && ClEmitParallel)) {
    for (auto SR = R->begin(), SRE = R->end(); SR != SRE; ++SR)
      regionIdentify(&(**SR));
    return;
  }

  if (!isLoopAnalyzable(l) || !st->isSafetlyRegionLoops(R)) {
    for (auto SR = R->begin(), SRE = R->end(); SR != SRE; ++SR)
      regionIdentify(&(**SR));
    return;
  }

  marknumAL(l);

  int line = l->getStartLoc().getLine();
  if (line == ERROR_VALUE)
    return;

  NewVars++;
  std::string computationName = std::string();
  computationName = "AI" + std::to_string(NewVars);
  RecoverCode RC;
  RC.setNAME(computationName);
  RC.setRecoverNames(rn);
  RC.initializeNewVars();
  RC.setOMP(ClEmitOMP); 

  // Variable to know the if the restrict pragma exists.
  // Case exists, use to add the test on pragmas.
  std::string test;
  if (RC.analyzeLoop(l, line, ERROR_VALUE, ptrRA, rp, aa, se, li, dt, test)) {
  
    std::map<std::string, bool> m;
    for (auto BB = l->block_begin(), BE = l->block_end(); BB != BE; BB++) {
      for (auto I = (*BB)->begin(), IE = (*BB)->end(); I != IE; I++) {
        if (CallInst *CI = dyn_cast<CallInst>(I)) {
          if (routines.count(CI->getCalledFunction()->getName()) == 0) {
             findACCroutines(CI->getCalledFunction());
          }
        }
      }
    }


    copyComments(RC.Comments);
    clearExpression();

    if (ClEmitParallel) {
      denotateLoopParallel(l, test);
      return;
    }
    
    marknumWL(l);
  }
}
bool WriteExpressions::isSafeMemoryCoalescing (Region *R) {
  if ((!R->getEnteringBlock() && !R->isTopLevelRegion()))
    return false;
  if (!ClEmitParallel)
    return true;
  std::map<Loop*, bool> loops;
  // Find all loops present in the same 
  for (auto BB = R->block_begin(), BE = R->block_end(); BB != BE; BB++) {
    Loop *l = li->getLoopFor(*BB);
    if (!l)
      continue;
    if (loops.count(l) == 0) {
      for (Loop *subLoop : l->getSubLoops())
        loops[subLoop] = true;
    }
    if (!isLoopParallel(l))
      return false;
    }
  return true;
}
 
void WriteExpressions::writeKernels (Loop *L, std::string NAME, bool restric) {
  if (!L)
    return;
  std::string flag = std::string();
  if (restric)
    flag = " if(!RST_" + NAME + ")";

  int line = L->getStartLoc()->getLine();
  std::string pragma = "#pragma acc kernels" + flag + "\n";
  if (!ClEmitParallel && (ClEmitOMP == ACC)) {
    addCommentToLine(pragma, line);
    return;
  }
  BasicBlock *BB = L->getLoopLatch();
  MDNode *MD = nullptr;
  MDNode *MDDivergent = nullptr;
  if (BB == nullptr)
    return;
  MD = BB->getTerminator()->getMetadata("isParallel");
  //MDDivergent = BB->getTerminator()->getMetadata("isDivergent");
  //if (ClDivergent && MDDivergent != nullptr)
  //  return;
  if (!MD)
    return;
  numWL++;
  if (ClEmitOMP == ACC)
    addCommentToLine(pragma, line);
  if (ClEmitParallel) {
    denotateLoopParallel(L, std::string());
    marknumWL(L);
  }
}

bool WriteExpressions::annotateAccKernels (Region *R, std::string NAME,
                                           bool restric) {
  if (!isSafeMemoryCoalescing(R))
    return false;
  std::map<Loop*, bool> loops;
  // Find all loops present in the same 
  for (auto BB = R->block_begin(), BE = R->block_end(); BB != BE; BB++) {
    Loop *l = li->getLoopFor(*BB);
    if (!l)
      continue;
    if (!R->contains(l->getHeader()))
      continue;
    if (loops.count(l) == 0) {
      loops[l] = true;
      writeKernels(l, NAME, restric);
    }
    std::queue<Loop*> q;
    q.push(l);
    while (!q.empty()) {
      Loop *ll = q.front();
      q.pop();
      loops[ll] = true;
      for (Loop *SubLoop : ll->getSubLoops())
        q.push(SubLoop);
    }
  }
}

void WriteExpressions::writeComputation (int line, int lineEnd,
                                         Region *R) {
  NewVars++;
  std::string computationName = std::string();
  computationName = "AI" + std::to_string(NewVars);
  RecoverCode RC;
  RC.setNAME(computationName);
  RC.setRecoverNames(rn);
  RC.initializeNewVars();
  RC.setOMP(ClEmitOMP); 

  // Variable to know the if the restrict pragma exists.
  // Case exists, use to add the test on pragmas.
  std::string test;
  if (RC.analyzeRegion(R, line, ERROR_VALUE, ptrRA, rp, aa, se, li, dt, test)) {

    std::map<std::string, bool> m;
    for (auto BB = R->block_begin(), BE = R->block_end(); BB != BE; BB++) {
      for (auto I = (*BB)->begin(), IE = (*BB)->end(); I != IE; I++) {
        if (CallInst *CI = dyn_cast<CallInst>(I)) {
          if (routines.count(CI->getCalledFunction()->getName()) == 0) {
             findACCroutines(CI->getCalledFunction());
          }
        }
      }
    }

    copyComments(RC.Comments);
    clearExpression();
    annotateAccKernels(R, computationName, RC.restric);
    std::string pragma = "}\n";
    addCommentToLine(pragma, lineEnd);
  }
}

void WriteExpressions::regionIdentifyCoalescing (Region *R) {
  // Use the first line of the region to try to find the location and annotate
  // the data transference pragma.
  int line = st->getStartRegionLoops(R).first;
  int lineEnd = st->getEndRegionLoops(R).first + 1;
  if (!isSafeMemoryCoalescing(R) || !st->isSafetlyRegionLoops(R)) {
    for (auto SR = R->begin(), SRE = R->end(); SR != SRE; ++SR)
      regionIdentifyCoalescing(&(**SR));
    return;

  }

  // For each region of function, we need to run an analysis, trying to identify
  // all memory access used.
  bool regionInvalid = !ptrRA->RegionsRangeData[R].HasFullSideEffectInfo;
  regionInvalid = regionInvalid || !(rr->isSafetly(R));
  if (regionInvalid) {
    ptrRA->analyzeReducedRegion(R);
    Region *RR = rr->returnReducedRegion(R);
    if (RR) {
    bool safe = rr->isSafetly(RR);
      if (safe && ptrRA->RegionsRangeData[RR].HasFullSideEffectInfo) {
        writeComputation(line, lineEnd, RR);
        return;
      }
    }
  }  
  
  if (regionInvalid) {
    for (auto SR = R->begin(), SRE = R->end(); SR != SRE; ++SR) {
      regionIdentifyCoalescing(&(**SR));
    }
    return;
  }
  writeComputation(line, lineEnd, R);  
}


void WriteExpressions::functionIdentify (Function *F) {
  std::map<Loop*, bool> loops;
  // For top region in the function, call the void regionIdentify:
  for (auto B = F->begin(), BE = F->end(); B != BE; B++) {
    // For each Basic Block in the Function, try find your loop, if
    // possible, and mark this loop as cound.
    Loop *l = li->getLoopFor(B);
    if (l && loops.count(l) == 0) {
      loops[l] = true;
      numL++;
    }
  }
  
  // Indetify the top region.
  Region *region = rp->getRegionInfo().getRegionFor(F->begin()); 
  Region *topRegion = region;
  while (region != NULL) {
    topRegion = region;
    region = region->getParent();
  }
     
  // Try analyzes top region.
  if (ClCoalescing)
    regionIdentifyCoalescing(topRegion);
  else
   regionIdentify(topRegion);
}

Region* WriteExpressions::regionofBasicBlock(BasicBlock *bb) {
  Region *r = rp->getRegionInfo().getRegionFor(bb);
  return r;
}

void WriteExpressions::findACCroutines (Function *F) {
  if (F->isDeclaration() || F->isIntrinsic() ||
      F->hasAvailableExternallyLinkage() || (ClEmitOMP != ACC)) {
    return;
  }
  if (routines.count(F->getName()) == 0) {
    routines[F->getName()] = true;
  }
  for (auto BB = F->begin(), BE = F->end(); BB != BE; BB++) {
    for (auto I = BB->begin(), IE = BB->end(); I != IE; I++) {
      if (CallInst *CI = dyn_cast<CallInst>(I)) {
        if (routines.count(CI->getCalledFunction()->getName()) == 0) {
           findACCroutines(CI->getCalledFunction());
        }
      }
    }
  }
}

bool WriteExpressions::isLoopAnalyzable (Loop *L){
  Region *r = rp->getRegionInfo().getRegionFor(L->getHeader());
  if (!ptrRA->RegionsRangeData[r].HasFullSideEffectInfo)
    return false;

  for (Loop::block_iterator I = L->block_begin(), IE = L->block_end();
       I != IE; ++I) {
    r = regionofBasicBlock(*I);
    if (!ptrRA->RegionsRangeData[r].HasFullSideEffectInfo)
      return false;
  }
  return true;
}

bool WriteExpressions::runOnFunction(Function &F) {
  this->li = &getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
  this->rp = &getAnalysis<RegionInfoPass>();
  this->aa = &getAnalysis<AliasAnalysis>();
  this->se = &getAnalysis<ScalarEvolution>();
  this->dt = &getAnalysis<DominatorTreeWrapperPass>().getDomTree();
  this->ptrRA = &getAnalysis<PtrRangeAnalysis>();
  this->rn = &getAnalysis<RecoverNames>();
  this->rr = &getAnalysis<RegionReconstructor>();
  this->st = &getAnalysis<ScopeTree>();

  NewVars = 0;
  
  Comments.erase(Comments.begin(), Comments.end());
  isknowedLoop.erase(isknowedLoop.begin(), isknowedLoop.end());

  // In this step, the "functionIdentify" find the top level loop
  // to apply our techinic.
  functionIdentify(&F);

  return true;
}

char WriteExpressions::ID = 0;
static RegisterPass<WriteExpressions> Z("writeExpressions",
"Recover access Expressions to source File.");

//===------------------------ writeExpressions.cpp ------------------------===//
