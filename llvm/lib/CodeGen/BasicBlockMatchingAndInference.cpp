//===- llvm/CodeGen/BasicBlockMatchingAndInference.cpp ----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// In Propeller's profile, we have already read the hash values of basic blocks,
// as well as the weights of basic blocks and edges in the CFG. In this file,
// we first match the basic blocks in the profile with those in the current
// MachineFunction using the basic block hash, thereby obtaining the weights of
// some basic blocks and edges. Subsequently, we infer the weights of all basic
// blocks using an inference algorithm.
//
// TODO: Integrate part of the code in this file with BOLT's implementation into
// the LLVM infrastructure, enabling both BOLT and Propeller to reuse it.
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/BasicBlockMatchingAndInference.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/CodeGen/BasicBlockSectionsProfileReader.h"
#include "llvm/CodeGen/MachineBlockHashInfo.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/Module.h"
#include "llvm/InitializePasses.h"
#include "llvm/ProfileData/SampleProf.h"
#include "llvm/ProfileData/SampleProfReader.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/VirtualFileSystem.h"
#include <cmath>

using namespace llvm;

#define DEBUG_TYPE "machine-block-match-infer"

static cl::opt<float>
    PropellerInferThreshold("propeller-infer-threshold",
                            cl::desc("Threshold for infer stale profile"),
                            cl::init(0.6));

static cl::opt<bool>
    PropellerMatchBBID("propeller-match-bbid",
                       cl::desc("Match basic blocks using BBIDs instead of "
                                "hashes for Propeller inference"),
                       cl::init(false), cl::Optional);

cl::opt<std::string> llvm::BasicBlockSectionAFDOProfile(
    "basic-block-section-afdo-profile",
    cl::desc("AFDO profile used to supplement the weights of basic blocks "
             "without samples in the Propeller profile"),
    cl::value_desc("filename"), cl::init(""));

cl::opt<std::string> llvm::BasicBlockSectionAFDORemappingFile(
    "basic-block-section-afdo-remapping-file",
    cl::desc("Symbol remapping file for -basic-block-section-afdo-profile"),
    cl::value_desc("filename"), cl::init(""));

/// The object is used to identify and match basic blocks given their hashes.
class StaleMatcher {
public:
  /// Initialize stale matcher.
  void init(const std::vector<MachineBasicBlock *> &Blocks,
            const std::vector<BlendedBlockHash> &Hashes) {
    assert(Blocks.size() == Hashes.size() &&
           "incorrect matcher initialization");
    for (size_t I = 0; I < Blocks.size(); I++) {
      MachineBasicBlock *Block = Blocks[I];
      uint16_t OpHash = Hashes[I].getOpcodeHash();
      OpHashToBlocks[OpHash].push_back(std::make_pair(Hashes[I], Block));
    }
  }

  /// Find the most similar block for a given hash.
  MachineBasicBlock *matchBlock(BlendedBlockHash BlendedHash) const {
    auto BlockIt = OpHashToBlocks.find(BlendedHash.getOpcodeHash());
    if (BlockIt == OpHashToBlocks.end()) {
      return nullptr;
    }
    MachineBasicBlock *BestBlock = nullptr;
    uint64_t BestDist = std::numeric_limits<uint64_t>::max();
    for (auto It : BlockIt->second) {
      MachineBasicBlock *Block = It.second;
      BlendedBlockHash Hash = It.first;
      uint64_t Dist = Hash.distance(BlendedHash);
      if (BestBlock == nullptr || Dist < BestDist) {
        BestDist = Dist;
        BestBlock = Block;
      }
    }
    return BestBlock;
  }

private:
  using HashBlockPairType = std::pair<BlendedBlockHash, MachineBasicBlock *>;
  DenseMap<uint16_t, std::vector<HashBlockPairType>> OpHashToBlocks;
};

INITIALIZE_PASS_BEGIN(BasicBlockMatchingAndInference,
                      "machine-block-match-infer",
                      "Machine Block Matching and Inference Analysis", true,
                      true)
INITIALIZE_PASS_DEPENDENCY(MachineBlockHashInfo)
INITIALIZE_PASS_DEPENDENCY(BasicBlockSectionsProfileReaderWrapperPass)
INITIALIZE_PASS_END(BasicBlockMatchingAndInference, "machine-block-match-infer",
                    "Machine Block Matching and Inference Analysis", true, true)

char BasicBlockMatchingAndInference::ID = 0;

/// Looks up the sampled weights of machine instructions in a line-based AFDO
/// profile. This is the lookup part of SampleProfileLoaderBaseImpl without its
/// weight propagation, remarks and coverage tracking, which are not needed to
/// supplement the Propeller profile.
class BasicBlockMatchingAndInference::AFDOProfile {
public:
  explicit AFDOProfile(std::unique_ptr<SampleProfileReader> Reader)
      : Reader(std::move(Reader)) {}

  /// Selects \p MF for the weight lookups below and returns its samples, or
  /// nullptr if the profile has none.
  const FunctionSamples *getSamples(const MachineFunction &MF) {
    LocationCache.clear();
    Samples = Reader->getSamplesFor(MF.getFunction());
    if (Samples && Samples->empty())
      Samples = nullptr;
    return Samples;
  }

  /// Returns the weight of \p MI in the selected function, or an error if it
  /// has no samples.
  ErrorOr<uint64_t> getInstWeight(const MachineInstr &MI) {
    const DILocation *DIL = MI.getDebugLoc();
    if (!DIL)
      return std::error_code();
    // Find the samples of the inlined instance MI comes from.
    auto [It, Inserted] = LocationCache.try_emplace(DIL, nullptr);
    if (Inserted)
      It->second = Samples->findFunctionSamples(DIL, Reader->getRemapper());
    if (!It->second)
      return std::error_code();
    unsigned Discriminator = EnableFSDiscriminator
                                 ? DIL->getDiscriminator()
                                 : DIL->getBaseDiscriminator();
    return It->second->findSamplesAt(FunctionSamples::getOffset(DIL),
                                     Discriminator);
  }

  /// Returns the maximum weight among the non-meta instructions of \p MBB in
  /// the selected function, or an error if none of them has samples.
  ErrorOr<uint64_t> getBlockWeight(const MachineBasicBlock &MBB) {
    uint64_t Max = 0;
    bool HasWeight = false;
    for (const MachineInstr &MI : MBB) {
      if (MI.isMetaInstruction())
        continue;
      if (ErrorOr<uint64_t> W = getInstWeight(MI)) {
        Max = std::max(Max, *W);
        HasWeight = true;
      }
    }
    if (!HasWeight)
      return std::error_code();
    return Max;
  }

private:
  std::unique_ptr<SampleProfileReader> Reader;
  const FunctionSamples *Samples = nullptr;
  DenseMap<const DILocation *, const FunctionSamples *> LocationCache;
};

BasicBlockMatchingAndInference::BasicBlockMatchingAndInference()
    : MachineFunctionPass(ID) {}

BasicBlockMatchingAndInference::~BasicBlockMatchingAndInference() = default;

void BasicBlockMatchingAndInference::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<MachineBlockHashInfo>();
  AU.addRequired<BasicBlockSectionsProfileReaderWrapperPass>();
  AU.setPreservesAll();
  MachineFunctionPass::getAnalysisUsage(AU);
}

std::optional<BasicBlockMatchingAndInference::WeightInfo>
BasicBlockMatchingAndInference::getWeightInfo(StringRef FuncName) const {
  auto It = ProgramWeightInfo.find(FuncName);
  if (It == ProgramWeightInfo.end()) {
    return std::nullopt;
  }
  return It->second;
}

BasicBlockMatchingAndInference::WeightInfo
BasicBlockMatchingAndInference::initWeightInfoByMatching(MachineFunction &MF) {
  std::vector<MachineBasicBlock *> Blocks;
  std::vector<BlendedBlockHash> Hashes;
  auto BSPR = &getAnalysis<BasicBlockSectionsProfileReaderWrapperPass>();
  auto MBHI = &getAnalysis<MachineBlockHashInfo>();
  for (auto &Block : MF) {
    Blocks.push_back(&Block);
    Hashes.push_back(BlendedBlockHash(MBHI->getMBBHash(Block)));
  }
  StaleMatcher Matcher;
  Matcher.init(Blocks, Hashes);
  BasicBlockMatchingAndInference::WeightInfo MatchWeight;
  const CFGProfile *CFG = BSPR->getFunctionCFGProfile(MF.getName());
  if (CFG == nullptr)
    return MatchWeight;
  for (auto &BlockCount : CFG->NodeCounts) {
    if (CFG->BBHashes.count(BlockCount.first.BaseID)) {
      auto Hash = CFG->BBHashes.lookup(BlockCount.first.BaseID);
      MachineBasicBlock *Block = Matcher.matchBlock(BlendedBlockHash(Hash));
      // When a basic block has clone copies, sum their counts.
      if (Block != nullptr)
        MatchWeight.BlockWeights[Block] += BlockCount.second;
    }
  }
  for (auto &PredItem : CFG->EdgeCounts) {
    auto PredID = PredItem.first.BaseID;
    if (!CFG->BBHashes.count(PredID))
      continue;
    auto PredHash = CFG->BBHashes.lookup(PredID);
    MachineBasicBlock *PredBlock =
        Matcher.matchBlock(BlendedBlockHash(PredHash));
    if (PredBlock == nullptr)
      continue;
    for (auto &SuccItem : PredItem.second) {
      auto SuccID = SuccItem.first.BaseID;
      auto EdgeWeight = SuccItem.second;
      if (CFG->BBHashes.count(SuccID)) {
        auto SuccHash = CFG->BBHashes.lookup(SuccID);
        MachineBasicBlock *SuccBlock =
            Matcher.matchBlock(BlendedBlockHash(SuccHash));
        // When an edge has clone copies, sum their counts.
        if (SuccBlock != nullptr)
          MatchWeight.EdgeWeights[std::make_pair(PredBlock, SuccBlock)] +=
              EdgeWeight;
      }
    }
  }
  return MatchWeight;
}

BasicBlockMatchingAndInference::WeightInfo
BasicBlockMatchingAndInference::initWeightInfoByBBID(MachineFunction &MF,
                                                     const CFGProfile &CFG) {
  // Path cloning does not run before matching and inference, so blocks are
  // identified by their base IDs. The counts of clones in the profile are
  // attributed to the original block.
  DenseMap<unsigned, MachineBasicBlock *> BaseIDToMBB;
  for (auto &MBB : MF) {
    unsigned BaseID = MBB.getBBID() ? MBB.getBBID()->BaseID
                                    : static_cast<unsigned>(MBB.getNumber());
    BaseIDToMBB[BaseID] = &MBB;
  }
  auto FindMBB = [&](const UniqueBBID &BBID) {
    return BaseIDToMBB.lookup(BBID.BaseID);
  };

  // A basic block is matched only if it exists in CFG.NodeCounts and all of its
  // edges in the CFG profile are also edges in the MIR. The profile may omit
  // edges without samples, so MIR edges missing from the profile are allowed.
  // Both ends of a profile edge that is not in the MIR are kept as unknown.
  DenseSet<const MachineBasicBlock *> MismatchedMBBs;
  for (const auto &[PredID, Succs] : CFG.EdgeCounts) {
    MachineBasicBlock *PredMBB = FindMBB(PredID);
    for (const auto &SuccItem : Succs) {
      MachineBasicBlock *SuccMBB = FindMBB(SuccItem.first);
      if (PredMBB && SuccMBB && PredMBB->isSuccessor(SuccMBB))
        continue;
      if (PredMBB)
        MismatchedMBBs.insert(PredMBB);
      if (SuccMBB)
        MismatchedMBBs.insert(SuccMBB);
    }
  }
  DenseSet<const MachineBasicBlock *> ProfiledMBBs;
  for (const auto &BlockCount : CFG.NodeCounts)
    if (MachineBasicBlock *MBB = FindMBB(BlockCount.first))
      ProfiledMBBs.insert(MBB);
  DenseSet<const MachineBasicBlock *> MatchedMBBs;
  for (auto &MBB : MF) {
    if (!ProfiledMBBs.contains(&MBB))
      continue;
    if (MismatchedMBBs.contains(&MBB))
      LLVM_DEBUG(dbgs() << MF.getName() << ": bb." << MBB.getNumber()
                        << " has profile edges that are not in the CFG\n");
    else
      MatchedMBBs.insert(&MBB);
  }

  BasicBlockMatchingAndInference::WeightInfo MatchWeight;
  for (const auto &BlockCount : CFG.NodeCounts) {
    MachineBasicBlock *MBB = FindMBB(BlockCount.first);
    if (MBB && MatchedMBBs.count(MBB))
      MatchWeight.BlockWeights[MBB] += BlockCount.second;
  }
  for (const auto &PredItem : CFG.EdgeCounts) {
    MachineBasicBlock *PredMBB = FindMBB(PredItem.first);
    if (!PredMBB || !MatchedMBBs.count(PredMBB))
      continue;
    for (const auto &SuccItem : PredItem.second) {
      MachineBasicBlock *SuccMBB = FindMBB(SuccItem.first);
      if (!SuccMBB || !MatchedMBBs.count(SuccMBB))
        continue;
      MatchWeight.EdgeWeights[std::make_pair(PredMBB, SuccMBB)] +=
          SuccItem.second;
    }
  }
  return MatchWeight;
}

void BasicBlockMatchingAndInference::generateWeightInfoByInference(
    MachineFunction &MF,
    BasicBlockMatchingAndInference::WeightInfo &MatchWeight) {
  BlockEdgeMap Successors;
  for (auto &Block : MF) {
    for (auto *Succ : Block.successors())
      Successors[&Block].push_back(Succ);
  }
  SampleProfileInference<MachineFunction> SPI(
      MF, Successors, MatchWeight.BlockWeights, MatchWeight.EdgeWeights);
  BlockWeightMap BlockWeights;
  EdgeWeightMap EdgeWeights;
  SPI.apply(BlockWeights, EdgeWeights);
  ProgramWeightInfo.try_emplace(
      MF.getName(), BasicBlockMatchingAndInference::WeightInfo{
                        std::move(BlockWeights), std::move(EdgeWeights)});
}

bool BasicBlockMatchingAndInference::doInitialization(Module &M) {
  if (BasicBlockSectionAFDOProfile.empty())
    return false;
  LLVMContext &Ctx = M.getContext();
  auto ReaderOrErr = SampleProfileReader::create(
      BasicBlockSectionAFDOProfile, Ctx, *vfs::getRealFileSystem(),
      FSDiscriminatorPass::Base, BasicBlockSectionAFDORemappingFile);
  if (std::error_code EC = ReaderOrErr.getError()) {
    Ctx.diagnose(DiagnosticInfoSampleProfile(BasicBlockSectionAFDOProfile,
                                             "Could not open AFDO profile: " +
                                                 EC.message()));
    return false;
  }
  std::unique_ptr<SampleProfileReader> Reader = std::move(*ReaderOrErr);
  Reader->setModule(&M);
  if (Reader->read() != sampleprof_error::success) {
    Ctx.diagnose(DiagnosticInfoSampleProfile(BasicBlockSectionAFDOProfile,
                                             "Could not read AFDO profile"));
    return false;
  }
  if (Reader->profileIsProbeBased()) {
    Ctx.diagnose(DiagnosticInfoSampleProfile(
        BasicBlockSectionAFDOProfile,
        "Pseudo probe based AFDO profiles are not supported", DS_Warning));
    return false;
  }
  AFDO = std::make_unique<AFDOProfile>(std::move(Reader));
  return false;
}

void BasicBlockMatchingAndInference::supplementWeightInfoWithAFDO(
    MachineFunction &MF, WeightInfo &MatchWeight) {
  if (!AFDO)
    return;
  const FunctionSamples *Samples = AFDO->getSamples(MF);
  if (!Samples)
    return;

  // Scale the AFDO weights so that the entry block weights of both profiles
  // agree.
  const MachineBasicBlock &EntryBB = MF.front();
  uint64_t PropellerEntryWeight =
      std::max<uint64_t>(MatchWeight.BlockWeights.lookup(&EntryBB), 1);
  uint64_t AFDOEntryWeight = 0;
  if (ErrorOr<uint64_t> W = AFDO->getBlockWeight(EntryBB))
    AFDOEntryWeight = *W;
  if (AFDOEntryWeight == 0)
    AFDOEntryWeight = Samples->getHeadSamplesEstimate();
  AFDOEntryWeight = std::max<uint64_t>(AFDOEntryWeight, 1);

  for (const MachineBasicBlock &MBB : MF) {
    // Only supplement blocks without Propeller samples. Inference treats
    // every block in BlockWeights as known, so blocks without AFDO samples
    // must stay out of the map.
    if (MatchWeight.BlockWeights.lookup(&MBB) != 0)
      continue;
    ErrorOr<uint64_t> AFDOWeight = AFDO->getBlockWeight(MBB);
    if (!AFDOWeight || *AFDOWeight == 0)
      continue;
    MatchWeight.BlockWeights[&MBB] = std::max<uint64_t>(
        1, std::round(static_cast<double>(*AFDOWeight) / AFDOEntryWeight *
                      PropellerEntryWeight));
  }
}

bool BasicBlockMatchingAndInference::doFinalization(Module &M) {
  // The profile is read for each module.
  AFDO.reset();
  return false;
}

bool BasicBlockMatchingAndInference::runOnMachineFunction(MachineFunction &MF) {
  if (MF.empty())
    return false;
  auto *BSPR = &getAnalysis<BasicBlockSectionsProfileReaderWrapperPass>();
  const CFGProfile *CFG = BSPR->getFunctionCFGProfile(MF.getName());
  if (CFG == nullptr || (CFG->NodeCounts.empty() && CFG->EdgeCounts.empty()))
    return false;

  BasicBlockMatchingAndInference::WeightInfo MatchWeight;
  if (PropellerMatchBBID) {
    MatchWeight = initWeightInfoByBBID(MF, *CFG);
  } else {
    MatchWeight = initWeightInfoByMatching(MF);
    // If the ratio of the number of MBBs in matching to the total number of
    // MBBs in the function is less than the threshold value, the processing
    // should be abandoned.
    if (static_cast<float>(MatchWeight.BlockWeights.size()) / MF.size() <
        PropellerInferThreshold) {
      return false;
    }
  }
  supplementWeightInfoWithAFDO(MF, MatchWeight);
  generateWeightInfoByInference(MF, MatchWeight);
  return false;
}

MachineFunctionPass *llvm::createBasicBlockMatchingAndInferencePass() {
  return new BasicBlockMatchingAndInference();
}
