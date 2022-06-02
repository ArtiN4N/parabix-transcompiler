#ifndef PIPELINE_GRAPH_PRINTER_HPP
#define PIPELINE_GRAPH_PRINTER_HPP

#include "pipeline_analysis.hpp"
#include <boost/algorithm/string/replace.hpp>
#include <boost/graph/strong_components.hpp>

#define USE_SIMPLE_BUFFER_GRAPH

namespace kernel {

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief printGraph
 ** ------------------------------------------------------------------------------------------------------------- */
template <typename Graph>
void printGraph(const Graph & G, raw_ostream & out, const StringRef name = "G") {

    out << "digraph \"" << name << "\" {\n";
    for (auto v : make_iterator_range(vertices(G))) {
        out << "v" << v << " [label=\"" << v << "\"];\n";
    }
    for (auto e : make_iterator_range(edges(G))) {
        const auto s = source(e, G);
        const auto t = target(e, G);
        out << "v" << s << " -> v" << t << ";\n";
    }

    out << "}\n\n";
    out.flush();
}


/** ------------------------------------------------------------------------------------------------------------- *
 * @brief printRelationshipGraph
 ** ------------------------------------------------------------------------------------------------------------- */
void PipelineAnalysis::printRelationshipGraph(const RelationshipGraph & G, raw_ostream & out, const StringRef name) {


    auto write = [](const Rational & v, llvm::raw_ostream & out)  {
        if (LLVM_LIKELY(v.denominator() == 1)) {
            out << v.numerator();
        } else {
            out << '(' << v.numerator() << '/' << v.denominator() << ')';
        }
    };

    std::vector<unsigned> component(num_vertices(G));
    strong_components(G, component.data());

    out << "digraph " << name << " {\n";
    for (const auto v : make_iterator_range(vertices(G))) {
        out << "v" << v << " [label=\"" << v << ' ';
        const RelationshipNode & rn = G[v];
        switch (rn.Type) {
            case RelationshipNode::IsNil:
                out << "<nil>";
                break;
            case RelationshipNode::IsKernel:
                out << "Kernel:";
                if (rn.Kernel) {
                    out << rn.Kernel->getName();
                }
                break;
            case RelationshipNode::IsBinding: {
                    const Binding & binding = rn.Binding;
                    out << "Binding:";
                    using KindId = ProcessingRate::KindId;
                    const ProcessingRate & rate = binding.getRate();
                    switch (rate.getKind()) {
                        case KindId::Fixed:
                            out << 'F';
                            write(rate.getLowerBound(), out);
                            break;
                        case KindId::Greedy:
                            out << 'G';
                            write(rate.getLowerBound(), out);
                            break;
                        case KindId::Bounded:
                            out << 'B';
                            write(rate.getLowerBound(), out);
                            out << '-';
                            write(rate.getUpperBound(), out);
                            break;
                        case KindId::Unknown:
                            out << 'U';
                            write(rate.getLowerBound(), out);
                            break;
                        case KindId::PopCount:
                            out << "Pop";
                            break;
                        case KindId::NegatedPopCount:
                            out << "Neg";
                            break;
                        case KindId::Relative:
                            out << 'R';
                            break;
                        case KindId::PartialSum:
                            out << 'P';
                            break;
                        case KindId::__Count: llvm_unreachable("");
                    }
                }
                break;
            case RelationshipNode::IsCallee:
                assert (&rn.Callee);
                out << "Callee:" << rn.Callee.get().Name;
                break;
            case RelationshipNode::IsRelationship:
                assert (rn.Relationship);
                if (isa<StreamSet>(rn.Relationship)) {
                    out << "StreamSet: ";
                } else if (isa<ScalarConstant>(rn.Relationship)) {
                    out << "Constant: ";
                } else if (isa<Scalar>(rn.Relationship)) {
                    out << "Scalar: ";
                } else {
                    out << "<Unknown Relationship>: ";
                }
                rn.Relationship->getType()->print(errs());

//                out << " ";
//                out.write_hex(reinterpret_cast<uintptr_t>(rn.Relationship));
        }
        out << "\"];\n";
        out.flush();
    }

    for (const auto e : make_iterator_range(edges(G))) {
        const auto s = source(e, G);
        const auto t = target(e, G);
        out << "v" << s << " -> v" << t << " ";
        char joiner = '[';
        const RelationshipType & rt = G[e];
        if (rt.Reason != ReasonType::OrderingConstraint) {
            out  << joiner << "label=\"";
            joiner = ',';
            switch (rt.Type) {
                case PortType::Input:
                    out << 'I';
                    break;
                case PortType::Output:
                    out << 'O';
                    break;
            }
            out << ':' << rt.Number;
            switch (rt.Reason) {
                case ReasonType::Explicit:
                    break;
                case ReasonType::ImplicitPopCount:
                    out << " (popcount)";
                    break;
                case ReasonType::ImplicitRegionSelector:
                    out << " (region)";
                    break;
                case ReasonType::Reference:
                    out << " (ref)";
                    break;
                default:
                    llvm_unreachable("invalid or unhandled reason type!");
                    break;
            }
            out << "\"";
        }

        if (LLVM_UNLIKELY(component[s] == component[t])) {
            out << joiner << "penwidth=3";
            joiner = ',';
        }

        switch (rt.Reason) {
            case ReasonType::None:
            case ReasonType::Explicit:
                break;
            case ReasonType::ImplicitPopCount:
            case ReasonType::ImplicitRegionSelector:
                out << joiner << "color=blue";
                break;
            case ReasonType::Reference:
                out << joiner << "color=gray";
                break;
            case ReasonType::OrderingConstraint:
                out << joiner << "color=red";
                break;
        }
        out << "];\n";
        out.flush();
    }
    out << "}\n\n";
    out.flush();
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief printBufferGraph
 ** ------------------------------------------------------------------------------------------------------------- */
void PipelineAnalysis::printBufferGraph(raw_ostream & out) const {

    using BufferId = StreamSetBuffer::BufferKind;

    auto print_rational = [&out](const Rational & r) -> raw_ostream & {
        if (r.denominator() > 1) {
            const auto n = r.numerator() / r.denominator();
            const auto p = r.numerator() % r.denominator();
            out << n << '+' << p << '/' << r.denominator();
        } else {
            out << r.numerator();
        }
        return out;
    };

    auto rate_range = [&out, print_rational](const Rational & a, const Rational & b) -> raw_ostream & {
        print_rational(a);
        out << ",";
        print_rational(b);
        return out;
    };

    auto printStreamSet = [&](const unsigned streamSet) {
        out << "v" << streamSet << " [shape=record,";
        const BufferNode & bn = mBufferGraph[streamSet];
        switch (bn.Locality) {
            case BufferLocality::GloballyShared:
            case BufferLocality::PartitionLocal:
                out << "style=bold,";
            default:
                break;
        }

        switch (bn.Locality) {
            case BufferLocality::ThreadLocal:
            case BufferLocality::PartitionLocal:
                out << "color=blue,";
            default:
                break;
        }

        const StreamSetBuffer * const buffer = bn.Buffer;

        out << "label=\"" << streamSet;
        out << " |{";


        if (bn.isExternal()) {
            out << 'X';
        }
        if (buffer == nullptr) {
            out << '?';
        } else {
            switch (buffer->getBufferKind()) {
                case BufferId::StaticBuffer:
                    out << 'S'; break;
                case BufferId::DynamicBuffer:
                    out << 'D'; break;
                case BufferId::ExternalBuffer:
                    out << 'E'; break;
                default: llvm_unreachable("unknown streamset type");
            }
        }
        if (bn.isUnowned()) {
            out << 'U';
        }
        if (bn.IsLinear) {
            out << 'L';
        }
        if (bn.isShared()) {
            out << '*';
        }

        if (buffer) {
            Type * ty = buffer->getBaseType();
            out << ':'
                << ty->getArrayNumElements() << 'x';
            ty = ty->getArrayElementType();
            ty = cast<IDISA::FixedVectorType>(ty)->getElementType();
            out << ty->getIntegerBitWidth();
        }

        #ifndef USE_SIMPLE_BUFFER_GRAPH
        if (bn.Locality == BufferLocality::ThreadLocal) {
            out << " [0x";
            out.write_hex(bn.BufferStart);
            out << "]";
        }
        #endif
        out << "|{";

        if (buffer && buffer->getBufferKind() != BufferId::ExternalBuffer) {
            switch (buffer->getBufferKind()) {
                case BufferId::StaticBuffer:
                    out << cast<StaticBuffer>(buffer)->getCapacity();
                    break;
                case BufferId::DynamicBuffer:
                    out << cast<DynamicBuffer>(buffer)->getInitialCapacity();
                    break;
                default: llvm_unreachable("unknown buffer type");
            }
        }

        #ifndef USE_SIMPLE_BUFFER_GRAPH
        if (bn.CopyBack) {
            out << "|CB:" << bn.CopyBack;
        }
        if (bn.CopyForwards) {
            out << "|CF:" << bn.CopyForwards;
        }
        if (bn.LookBehind) {
            out << "|LB:" << bn.LookBehind;
        }
        if (bn.MaxAdd) {
            out << "|+" << bn.MaxAdd;
        }
        if (bn.CrossesHybridThreadBarrier) {
            out << "|Hybrid";
        }
        #endif

        out << "}}\"];\n";

    };

    auto currentPartition = PartitionCount;
    bool closePartition = false;

    std::vector<unsigned> firstKernelOfPartition(PartitionCount);

    auto checkClosePartitionLabel = [&]() {
        if (closePartition) {
            out << "}\n";
            closePartition = false;
        }
    };

    bool firstKernelInPartition = true;

    auto checkOpenPartitionLabel = [&](const unsigned kernel, const bool ignorePartition) {
        const auto partitionId = KernelPartitionId[kernel];
        if (partitionId != currentPartition || ignorePartition) {
            checkClosePartitionLabel();
            firstKernelInPartition = true;
            if (LLVM_LIKELY(!ignorePartition)) {
                out << "subgraph cluster" << partitionId << " {\n"
                       "label=\"Partition #" << partitionId  << "\";"
                       "fontcolor=\"red\";"
                       "style=\"rounded,dashed,bold\";";
                if (PartitionOnHybridThread.test(partitionId)) {
                    out << "color=\"blue\";";
                } else {
                    out << "color=\"red\";";
                }
                out <<  "\n";
                closePartition = true;
                currentPartition = partitionId;
                firstKernelOfPartition[partitionId] = kernel;
            }
        }        
    };



    auto printKernel = [&](const unsigned kernel, const StringRef name,
                           const bool ignorePartition, const bool hideKernel) {
        checkOpenPartitionLabel(kernel, ignorePartition);

        if (hideKernel) {
            return;
        }

        const Kernel * const kernelObj = getKernel(kernel);

        assert (kernelObj);

        const auto nonLinear = mayHaveNonLinearIO(kernel);

        const auto borders = nonLinear ? '2' : '1';
        out << "v" << kernel << " [label=\"[" <<
                kernel << "] " << name << "\\n";
        if (MinimumNumOfStrides.size() > 0) {
            out << " Expected: [";
            if (MaximumNumOfStrides.size() > 0) {
                print_rational(MinimumNumOfStrides[kernel]) << ',';
                print_rational(MaximumNumOfStrides[kernel]);
            } else {
                print_rational(MinimumNumOfStrides[kernel]) << ",?";
            }
            out << "]";
            if (StrideStepLength.size() > 0) {
                out << " (x" << StrideStepLength[kernel] << ")";
            }
            out << "\\n";
        }
        if (isKernelStateFree(kernel)) {
            out << "<StateFree>\\n";
        }
        if (kernelObj->hasAttribute(AttrId::InternallySynchronized)) {
            out << "<InternallySynchronized>\\n";
        }
        if (kernelObj->canSetTerminateSignal()) {
            out << "<CanTerminateEarly>\\n";
        }

        out << "\" shape=rect,style=rounded,peripheries=" << borders;
        #ifndef USE_SIMPLE_BUFFER_GRAPH
        if (kernelObj->requiresExplicitPartialFinalStride()) {
            out << ",color=\"blue\"";
        }
        #endif
        out << "];\n";

        for (const auto e : make_iterator_range(out_edges(kernel, mBufferGraph))) {
            const auto streamSet = target(e, mBufferGraph);
            printStreamSet(streamSet);
        }

        firstKernelInPartition = false;
    };

    out << "digraph \"" << StringRef(mPipelineKernel->getName()) << "\" {\n"
           "rankdir=tb;"
           "nodesep=0.5;"
           "ranksep=0.5;"
           "newrank=true;"
           // "compound=true;"
           "\n";

    printKernel(PipelineInput, "P_{in}", true, out_degree(PipelineInput, mBufferGraph) == 0);
    firstKernelOfPartition[0] = PipelineInput;
    for (unsigned i = FirstKernel; i <= LastKernel; ++i) {
        const Kernel * const kernel = getKernel(i); assert (kernel);
        auto name = kernel->getName();
        boost::replace_all(name, "\"", "\\\"");
        printKernel(i, name, false, false);
    }

    bool hidePipelineOutput = in_degree(PipelineOutput, mBufferGraph) == 0;
    for (auto i = KernelPartitionId[FirstKernel]; i < KernelPartitionId[LastKernel]; ++i) {
        if (mPartitionJumpIndex[i] == KernelPartitionId[PipelineOutput]) {
            hidePipelineOutput = false;
            break;
        }
    }

    printKernel(PipelineOutput, "P_{out}", true, hidePipelineOutput);
    firstKernelOfPartition[PartitionCount - 1] = PipelineOutput;

    for (auto e : make_iterator_range(edges(mBufferGraph))) {
        const auto s = source(e, mBufferGraph);
        const auto t = target(e, mBufferGraph);

        const BufferPort & port = mBufferGraph[e];

        bool isLocal = true;
        // is this edge from a buffer to a kernel?
        if (s >= FirstStreamSet) {
            const auto p = parent(s, mBufferGraph);
            const auto pId = KernelPartitionId[p];
            const auto tId = KernelPartitionId[t];
            // does this use of the buffer cross a partition boundary?
            isLocal = (pId == tId);
        }

        out << "v" << s << " -> v" << t <<
               " [";
        out << "label=\"#" << port.Port.Number << ": ";
        const Binding & binding = port.Binding;
        const ProcessingRate & rate = binding.getRate();
        switch (rate.getKind()) {
            case RateId::Fixed:
                out << "F(";
                print_rational(port.Minimum);
                out << ")";
                break;
            case RateId::Bounded:
                out << "B(";
                rate_range(port.Minimum, port.Maximum);
                out << ")";
                break;
            case RateId::Greedy:
                out << "G(";
                print_rational(rate.getLowerBound());
                out << ",*)";
                break;
            case RateId::PartialSum:
                out << "P(";
                print_rational(rate.getUpperBound());
                out << ")";
                break;
            default: llvm_unreachable("unknown or unhandled rate type in buffer graph");
        }
        // out << " {G" << pd.GlobalPortId << ",L" << pd.LocalPortId << '}';

        #ifndef USE_SIMPLE_BUFFER_GRAPH

        out << " {" << port.SymbolicRateId << '}';

        if (port.IsPrincipal) {
            out << " [P]";
        }
        if (port.IsShared) {
            out << " [S]";
        }
        if (port.TransitiveAdd) {
            out << " +" << port.TransitiveAdd;
        }
        if (binding.hasAttribute(AttrId::ZeroExtended)) {
            if (port.IsZeroExtended) {
                out << " [Z]";
            } else {
                out << " [z&#x336;]";
            }
        }
        #endif

        using DistId = ProcessingRateProbabilityDistribution::DistributionTypeId;



        const auto & D = binding.getDistribution();
        switch (D.getTypeId()) {
            case DistId::Uniform: break;
            case DistId::Normal:
                out << " [Dist: " << "Normal "
                    << llvm::format("%0.2f", D.getMean())
                    << ","
                    << llvm::format("%0.2f", D.getStdDev()) << "]";
                break;
            case DistId::Gamma:
                out << " [Dist: " << "Gamma "
                    << llvm::format("%0.2f", D.getAlpha())
                    << ","
                    << llvm::format("%0.2f", D.getBeta()) << "]";
                break;
            case DistId::Maximum:
                out << "[Dist: " << "Max" << "]";
                break;
        }

        if (port.LookBehind) {
            out << " [LB:" << port.LookBehind << ']';
        }
        if (port.LookAhead) {
            out << " [LA:" << port.LookAhead << ']';
        }
        if (port.Delay) {
            out << " [Delay:" << port.Delay << ']';
        }
        std::string name = binding.getName();
        boost::replace_all(name, "\"", "\\\"");
        out << "\\n" << name << "\"";
        if (isLocal) {
            out << " style=dashed";
        } else if (port.CanModifySegmentLength) {
            out << " style=bold";
        }
        out << "];\n";
    }

    for (unsigned i = 0; i < PartitionCount; ++i) {
        const auto a = i;
        const auto b = mPartitionJumpIndex[i];
        if (b > (a + 1)) {
            const auto s = firstKernelOfPartition[a];
            const auto t = firstKernelOfPartition[b];
            out << "v" << s << " -> v" << t <<
                   " ["
                      "style=\"dotted,bold\","
                      "color=\"red\""
                   "];\n";
        }
    }

    #ifndef USE_SIMPLE_BUFFER_GRAPH
    for (const auto e : make_iterator_range(edges(mConsumerGraph))) {

        const auto a = source(e, mConsumerGraph);
        const auto b = target(e, mConsumerGraph);
        const ConsumerEdge & c = mConsumerGraph[e];

        if (c.Flags & ConsumerEdge::WriteConsumedCount) {
            out << "v" << a << " -> v" << b <<
                   " [style=dotted,color=\"blue\",dir=none];\n";
        }


    }
    #endif

    out << "}\n\n";
    out.flush();
}

}

#endif // PIPELINE_GRAPH_PRINTER_HPP
