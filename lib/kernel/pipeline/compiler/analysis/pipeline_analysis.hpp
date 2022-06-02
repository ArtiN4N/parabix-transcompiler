#ifndef PIPELINE_KERNEL_ANALYSIS_HPP
#define PIPELINE_KERNEL_ANALYSIS_HPP

#include <algorithm>
#include <queue>
#include <z3.h>
#include <assert.h>

#include <kernel/core/kernel.h>
#include <kernel/core/relationship.h>
#include <kernel/core/streamset.h>
#include <kernel/core/kernel_builder.h>

#include <llvm/Support/Format.h>
#include "../pipeline_compiler.hpp"

namespace kernel {

struct PipelineAnalysis : public PipelineCommonGraphFunctions {

    using KernelPartitionIds = flat_map<ProgramGraph::vertex_descriptor, unsigned>;

    static PipelineAnalysis analyze(BuilderRef b, PipelineKernel * const pipelineKernel) {

        PipelineAnalysis P(pipelineKernel);

//        std::random_device rd;
//        xoroshiro128 rng(rd);

        pipeline_random_engine rng;

//        const auto graphSeed = 2081280305;
//        P.generateRandomPipelineGraph(b, graphSeed, 50, 70, 10);

        P.generateInitialPipelineGraph(b);


        // Initially, we gather information about our partition to determine what kernels
        // are within each partition in a topological order
        auto initialGraph = P.initialPartitioningPass();
        P.computeIntraPartitionRepetitionVectors(initialGraph);
        P.estimateInterPartitionDataflow(initialGraph, rng);
        auto partitionGraph = P.postDataflowAnalysisPartitioningPass(initialGraph);

        P.schedulePartitionedProgram(partitionGraph, rng);
        // Construct the Stream and Scalar graphs
        P.transcribeRelationshipGraph(partitionGraph);

        P.generateInitialBufferGraph();

        P.identifyKernelsOnHybridThread();

        P.identifyOutputNodeIds();

        P.identifyInterPartitionSymbolicRates();

        P.markInterPartitionStreamSetsAsGloballyShared();

        P.identifyTerminationChecks();

        P.determinePartitionJumpIndices();

        P.annotateBufferGraphWithAddAttributes();

        // Finish annotating the buffer graph
        P.identifyOwnedBuffers();
        P.identifyLinearBuffers();
        P.identifyZeroExtendedStreamSets();

        P.determineBufferSize(b);
        P.determineBufferLayout(b, rng);

        P.identifyCrossHybridThreadStreamSets();
        P.identifyPortsThatModifySegmentLength();

        P.makeConsumerGraph();

        P.calculatePartialSumStepFactors();

        P.makeTerminationPropagationGraph();

        // Finish the buffer graph
        // P.identifyDirectUpdatesToStateObjects();
        P.addStreamSetsToBufferGraph(b);

        P.gatherInfo();

        if (codegen::DebugOptionIsSet(codegen::PrintPipelineGraph)) {
            P.printBufferGraph(errs());
        }

        return P;
    }

private:

    // constructor

    PipelineAnalysis(PipelineKernel * const pipelineKernel)
    : PipelineCommonGraphFunctions(mStreamGraph, mBufferGraph)
    , mPipelineKernel(pipelineKernel)
    , mNumOfThreads(pipelineKernel->getNumOfThreads())
    , mLengthAssertions(pipelineKernel->getLengthAssertions())
    , mTraceProcessedProducedItemCounts(codegen::DebugOptionIsSet(codegen::TraceCounts))
    , mTraceDynamicBuffers(codegen::DebugOptionIsSet(codegen::TraceDynamicBuffers))
    , mTraceIndividualConsumedItemCounts(mTraceProcessedProducedItemCounts || mTraceDynamicBuffers) {

    }

    // pipeline analysis functions

    void generateInitialPipelineGraph(BuilderRef b);

    #ifdef ENABLE_GRAPH_TESTING_FUNCTIONS
    void generateRandomPipelineGraph(BuilderRef b, const uint64_t seed,
                                     const unsigned desiredKernels, const unsigned desiredStreamSets,
                                     const unsigned desiredPartitions);
    #endif

    using KernelVertexVec = SmallVector<ProgramGraph::Vertex, 64>;

    void addRegionSelectorKernels(BuilderRef b, Kernels & partition, KernelVertexVec & vertex, ProgramGraph & G);

    void addPopCountKernels(BuilderRef b, Kernels & partition, KernelVertexVec & vertex, ProgramGraph & G);

    void combineDuplicateKernels(BuilderRef b, const Kernels & partition, ProgramGraph & G);

    void removeUnusedKernels(const unsigned p_in, const unsigned p_out, const Kernels & partition, ProgramGraph & G);

    void identifyPipelineInputs();

    void transcribeRelationshipGraph(const PartitionGraph & partitionGraph);

    void gatherInfo() {        
        MaxNumOfInputPorts = in_degree(PipelineOutput, mBufferGraph);
        MaxNumOfOutputPorts = out_degree(PipelineInput, mBufferGraph);
        for (auto i = FirstKernel; i <= LastKernel; ++i) {
            MaxNumOfInputPorts = std::max<unsigned>(MaxNumOfInputPorts, in_degree(i, mBufferGraph));
            MaxNumOfOutputPorts = std::max<unsigned>(MaxNumOfOutputPorts, out_degree(i, mBufferGraph));
        }
    }

    static void addKernelRelationshipsInReferenceOrdering(const unsigned kernel, const RelationshipGraph & G,
                                                          std::function<void(PortType, unsigned, unsigned)> insertionFunction);


    // partitioning analysis
    PartitionGraph initialPartitioningPass();
    PartitionGraph postDataflowAnalysisPartitioningPass(PartitionGraph & initial);

    PartitionGraph identifyKernelPartitions();

    void determinePartitionJumpIndices();

    void makePartitionJumpTree();

    // scheduling analysis

    void schedulePartitionedProgram(PartitionGraph & P, pipeline_random_engine & rng);

    void analyzeDataflowWithinPartitions(PartitionGraph & P, pipeline_random_engine & rng) const;

    SchedulingGraph makeIntraPartitionSchedulingGraph(const PartitionGraph & P, const unsigned currentPartitionId) const;

    PartitionDependencyGraph makePartitionDependencyGraph(const unsigned numOfKernels, const SchedulingGraph & S) const;

    OrderingDAWG scheduleProgramGraph(const PartitionGraph & P, pipeline_random_engine & rng) const;

    OrderingDAWG assembleFullSchedule(const PartitionGraph & P, const OrderingDAWG & partial) const;

    std::vector<unsigned> selectScheduleFromDAWG(const OrderingDAWG & schedule) const;

    void addSchedulingConstraints(const std::vector<unsigned> & program);

    static bool isNonSynchronousRate(const Binding & binding);

    // buffer management analysis functions

    void addStreamSetsToBufferGraph(BuilderRef b);
    void generateInitialBufferGraph();

    void determineBufferSize(BuilderRef b);

    void determineBufferLayout(BuilderRef b, pipeline_random_engine & rng);

    void identifyOwnedBuffers();

    void identifyLinearBuffers();
    void markInterPartitionStreamSetsAsGloballyShared();

    void identifyOutputNodeIds();

    void identifyPortsThatModifySegmentLength();

    // void identifyDirectUpdatesToStateObjects();

    // consumer analysis functions

    void identifyCrossHybridThreadStreamSets();
    void makeConsumerGraph();

    // dataflow analysis functions
    void computeIntraPartitionRepetitionVectors(PartitionGraph & P);
    void estimateInterPartitionDataflow(PartitionGraph & P, pipeline_random_engine & rng);

    void computeMinimumExpectedDataflow(PartitionGraph & P);

    void identifyInterPartitionSymbolicRates();

    void calculatePartialSumStepFactors();

    // zero extension analysis function

    void identifyZeroExtendedStreamSets();

    // termination analysis functions

    void identifyTerminationChecks();
    void makeTerminationPropagationGraph();

    // add(k) analysis functions

    void annotateBufferGraphWithAddAttributes();

    // IO analysis functions

    void identifyKernelsOnHybridThread();

    // Input truncation analysis functions

    void makeInputTruncationGraph();




public:

    // Debug functions
    void printBufferGraph(raw_ostream & out) const;
    static void printRelationshipGraph(const RelationshipGraph & G, raw_ostream & out, const StringRef name = "G");

private:

    PipelineKernel * const          mPipelineKernel;
    const unsigned					mNumOfThreads;
    const LengthAssertions &        mLengthAssertions;
    ProgramGraph                    Relationships;
    KernelPartitionIds              PartitionIds;

public:

    const bool                      mTraceProcessedProducedItemCounts;
    const bool                      mTraceDynamicBuffers;
    const bool                      mTraceIndividualConsumedItemCounts;

    static const unsigned           PipelineInput = 0U;
    static const unsigned           FirstKernel = 1U;
    unsigned                        LastKernel = 0;
    unsigned                        PipelineOutput = 0;
    unsigned                        FirstStreamSet = 0;
    unsigned                        LastStreamSet = 0;
    unsigned                        FirstBinding = 0;
    unsigned                        LastBinding = 0;
    unsigned                        FirstCall = 0;
    unsigned                        LastCall = 0;
    unsigned                        FirstScalar = 0;
    unsigned                        LastScalar = 0;
    unsigned                        PartitionCount = 0;
    bool                            HasZeroExtendedStream = false;

    size_t                          RequiredThreadLocalStreamSetMemory = 0;

    unsigned                        MaxNumOfInputPorts = 0;
    unsigned                        MaxNumOfOutputPorts = 0;

    RelationshipGraph               mStreamGraph;
    RelationshipGraph               mScalarGraph;

    KernelIdVector                  KernelPartitionId;

    std::vector<unsigned>           MinimumNumOfStrides;
    std::vector<unsigned>           MaximumNumOfStrides;
    std::vector<unsigned>           StrideStepLength;

    BufferGraph                     mBufferGraph;

    BitVector                       KernelOnHybridThread;
    BitVector                       PartitionOnHybridThread;

    std::vector<unsigned>           mPartitionJumpIndex;

    ConsumerGraph                   mConsumerGraph;
    PartialSumStepFactorGraph       mPartialSumStepFactorGraph;

    TerminationChecks               mTerminationCheck;

    TerminationPropagationGraph     mTerminationPropagationGraph;

    OwningVector<Kernel>            mInternalKernels;
    OwningVector<Binding>           mInternalBindings;
    OwningVector<StreamSetBuffer>   mInternalBuffers;
};

}

#include "pipeline_graph_printers.hpp"
#include "evolutionary_algorithm.hpp"
#include "relationship_analysis.hpp"
#include "buffer_analysis.hpp"
#include "buffer_size_analysis.hpp"
#include "consumer_analysis.hpp"
#include "dataflow_analysis.hpp"
#include "variable_rate_analysis.hpp"
#include "partitioning_analysis.hpp"
#include "scheduling_analysis.hpp"
#include "termination_analysis.hpp"
#include "zero_extend_analysis.hpp"
#include "add_analysis.hpp"

#endif
