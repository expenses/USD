//
// Copyright 2016 Pixar
//
// Licensed under the Apache License, Version 2.0 (the "Apache License")
// with the following modification; you may not use this file except in
// compliance with the Apache License and the following modification to it:
// Section 6. Trademarks. is deleted and replaced with:
//
// 6. Trademarks. This License does not grant permission to use the trade
//    names, trademarks, service marks, or product names of the Licensor
//    and its affiliates, except as required to comply with Section 4(c) of
//    the License and to reproduce the content of the NOTICE file.
//
// You may obtain a copy of the Apache License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the Apache License with the above modification is
// distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied. See the Apache License for the specific
// language governing permissions and limitations under the Apache License.
//

#include "pxr/pxr.h"
#include "pxr/usd/pcp/primIndex.h"
#include "pxr/usd/pcp/arc.h"
#include "pxr/usd/pcp/cache.h"
#include "pxr/usd/pcp/dynamicFileFormatContext.h"
#include "pxr/usd/pcp/composeSite.h"
#include "pxr/usd/pcp/debugCodes.h"
#include "pxr/usd/pcp/dependencies.h"
#include "pxr/usd/pcp/diagnostic.h"
#include "pxr/usd/pcp/dynamicFileFormatInterface.h"
#include "pxr/usd/pcp/expressionVariables.h"
#include "pxr/usd/pcp/instancing.h"
#include "pxr/usd/pcp/layerStack.h"
#include "pxr/usd/pcp/layerStackRegistry.h"
#include "pxr/usd/pcp/node_Iterator.h"
#include "pxr/usd/pcp/primIndex_Graph.h"
#include "pxr/usd/pcp/primIndex_StackFrame.h"
#include "pxr/usd/pcp/statistics.h"
#include "pxr/usd/pcp/strengthOrdering.h"
#include "pxr/usd/pcp/types.h"
#include "pxr/usd/pcp/utils.h"
#include "pxr/usd/ar/resolver.h"
#include "pxr/usd/ar/resolverContextBinder.h"
#include "pxr/usd/sdf/fileFormat.h"
#include "pxr/usd/sdf/layer.h"
#include "pxr/base/trace/trace.h"
#include "pxr/base/tf/debug.h"
#include "pxr/base/tf/enum.h"
#include "pxr/base/tf/hash.h"
#include "pxr/base/tf/pxrTslRobinMap/robin_set.h"
#include "pxr/base/tf/diagnostic.h"
#include "pxr/base/tf/envSetting.h"
#include "pxr/base/tf/mallocTag.h"

#include <algorithm>
#include <functional>
#include <iostream>
#include <vector>

// Un-comment for extra runtime validation.
// #define PCP_DIAGNOSTIC_VALIDATION 1

using std::string;
using std::vector;

PXR_NAMESPACE_OPEN_SCOPE

TF_DEFINE_ENV_SETTING(
    MENV30_ENABLE_NEW_DEFAULT_STANDIN_BEHAVIOR, true,
    "If enabled then standin preference is weakest opinion.");

static inline PcpPrimIndex const *
_GetOriginatingIndex(PcpPrimIndex_StackFrame *previousFrame,
                     PcpPrimIndexOutputs *outputs) {
    return ARCH_UNLIKELY(previousFrame) ?
        previousFrame->originatingIndex : &outputs->primIndex;
}

bool
PcpIsNewDefaultStandinBehaviorEnabled()
{
    return TfGetEnvSetting(MENV30_ENABLE_NEW_DEFAULT_STANDIN_BEHAVIOR);
}

////////////////////////////////////////////////////////////////////////

PcpPrimIndex::PcpPrimIndex()
{
}

PcpNodeRef
PcpPrimIndex::GetRootNode() const
{
    return _graph ? _graph->GetRootNode() : PcpNodeRef();
}

const SdfPath&
PcpPrimIndex::GetPath() const
{
    return _graph ? _graph->GetRootNode().GetPath() : SdfPath::EmptyPath();
}

bool
PcpPrimIndex::HasSpecs() const
{
    // Prim stacks are not cached in Usd mode
    if (!IsUsd()) {
        return !_primStack.empty();
    }

    for (const auto& node : GetNodeRange()) {
        if (node.HasSpecs()) {
            return true;
        }
    }

    return false;
}

bool
PcpPrimIndex::HasAnyPayloads() const
{
    return _graph && _graph->HasPayloads();
}

bool
PcpPrimIndex::IsUsd() const
{
    return _graph && _graph->IsUsd();
}

bool
PcpPrimIndex::IsInstanceable() const
{
    return _graph && _graph->IsInstanceable();
}

PcpPrimIndex::PcpPrimIndex(const PcpPrimIndex &rhs)
{
    _graph = rhs._graph;
    _primStack = rhs._primStack;

    if (rhs._localErrors) {
        _localErrors.reset(new PcpErrorVector(*rhs._localErrors.get()));
    }
}

void 
PcpPrimIndex::Swap(PcpPrimIndex& rhs)
{ 
    _graph.swap(rhs._graph);
    _primStack.swap(rhs._primStack);
    _localErrors.swap(rhs._localErrors);
}

void
PcpPrimIndex::PrintStatistics() const
{
    Pcp_PrintPrimIndexStatistics(*this, std::cout);
}

std::string PcpPrimIndex::DumpToString(
    bool includeInheritOriginInfo,
    bool includeMaps) const
{
    return PcpDump(
        *this, includeInheritOriginInfo, includeMaps);
}

void PcpPrimIndex::DumpToDotGraph(
    const std::string& filename,
    bool includeInheritOriginInfo,
    bool includeMaps) const
{
    PcpDumpDotGraph(
        *this, filename.c_str(), includeInheritOriginInfo, includeMaps);
}

PcpNodeRange
PcpPrimIndex::GetNodeRange(PcpRangeType rangeType) const
{
    if (!_graph) {
        return PcpNodeRange();
    }

    const std::pair<size_t, size_t> range = 
        _graph->GetNodeIndexesForRange(rangeType);
    return PcpNodeRange(
        PcpNodeIterator(get_pointer(_graph), range.first),
        PcpNodeIterator(get_pointer(_graph), range.second));
}

PcpNodeIterator 
PcpPrimIndex::GetNodeIteratorAtNode(const PcpNodeRef &node) const
{
    if (!_graph) {
        return PcpNodeIterator();
    }
    return PcpNodeIterator(
        get_pointer(_graph), _graph->GetNodeIndexForNode(node));
}

PcpPrimRange 
PcpPrimIndex::GetPrimRange(PcpRangeType rangeType) const
{
    if (!_graph) {
        return PcpPrimRange();
    }

    // Early out for common case of retrieving entire prim range.
    if (rangeType == PcpRangeTypeAll) {
        return PcpPrimRange(
            PcpPrimIterator(this, 0),
            PcpPrimIterator(this, _primStack.size()));
    }

    const std::pair<size_t, size_t> range = 
        _graph->GetNodeIndexesForRange(rangeType);
    const size_t startNodeIdx = range.first;
    const size_t endNodeIdx = range.second;

    for (size_t startPrimIdx = 0; 
         startPrimIdx < _primStack.size(); ++startPrimIdx) {

        const Pcp_CompressedSdSite& startPrim = _primStack[startPrimIdx];
        if (startPrim.nodeIndex >= startNodeIdx && 
            startPrim.nodeIndex < endNodeIdx) {

            size_t endPrimIdx = startPrimIdx + 1;
            for (; endPrimIdx < _primStack.size(); ++endPrimIdx) {
                const Pcp_CompressedSdSite& endPrim = _primStack[endPrimIdx];
                if (endPrim.nodeIndex >= endNodeIdx) {
                    break;
                }
            }

            return PcpPrimRange(
                PcpPrimIterator(this, startPrimIdx),
                PcpPrimIterator(this, endPrimIdx));
        }
    }

    return PcpPrimRange(PcpPrimIterator(this, _primStack.size()),
                        PcpPrimIterator(this, _primStack.size()));
}

PcpPrimRange 
PcpPrimIndex::GetPrimRangeForNode(const PcpNodeRef& node) const
{
    PcpPrimIterator firstIt(this, 0);
    PcpPrimIterator endIt(this, _primStack.size());

    // XXX: optimization
    // This is slow, but the prim index doesn't provide us any faster
    // way to associate a node with prims in the prim stack. We may need
    // to store indices into the prim stack with each node, similar to
    // Csd_NamespaceExcerpt and Csd_PrimCache.
    while (firstIt != endIt && firstIt.GetNode() != node) {
        ++firstIt;
    }

    if (firstIt == endIt) {
        return PcpPrimRange();
    }

    PcpPrimIterator lastIt = firstIt;
    while (++lastIt != endIt && lastIt.GetNode() == node) {
        // Do nothing
    }

    return PcpPrimRange(firstIt, lastIt);
}

PcpNodeRef
PcpPrimIndex::GetNodeProvidingSpec(const SdfPrimSpecHandle& primSpec) const
{
    return GetNodeProvidingSpec(primSpec->GetLayer(), primSpec->GetPath());
}

PcpNodeRef
PcpPrimIndex::GetNodeProvidingSpec(
    const SdfLayerHandle& layer, const SdfPath& path) const
{
    for (const PcpNodeRef &node: GetNodeRange()) {
        // If the site has the given path and contributes specs then
        // search for the layer.
        if (node.CanContributeSpecs() && 
            node.GetPath() == path    && 
            node.GetLayerStack()->HasLayer(layer)) {
            return node;
        }
    }

    return PcpNodeRef();
}

SdfVariantSelectionMap
PcpPrimIndex::ComposeAuthoredVariantSelections() const
{
    TRACE_FUNCTION();

    // Collect the selections according to the prim stack.
    SdfVariantSelectionMap result;
    const TfToken field = SdfFieldKeys->VariantSelection;
    TF_FOR_ALL(i, GetPrimRange()) {
        const Pcp_SdSiteRef site = i.base()._GetSiteRef();

        SdfVariantSelectionMap vselMap;
        if (!site.layer->HasField(site.path, field, &vselMap)) {
            continue;
        }

        for (auto it = vselMap.begin(); it != vselMap.end(); ) {
            std::string& vsel = it->second;
            if (Pcp_IsVariableExpression(vsel)) {
                const PcpLayerStackRefPtr& layerStack =
                    i.base().GetNode().GetLayerStack();

                PcpErrorVector exprErrors;
                vsel = Pcp_EvaluateVariableExpression(
                    vsel, layerStack->GetExpressionVariables(),
                    "variant", site.layer, site.path, nullptr, &exprErrors);

                // If an error occurred evaluating this expression, we ignore
                // this variant selection and look for the next weakest opinion.
                // We don't emit any errors here since they would have already
                // been captured as composition errors during prim indexing.
                // See PcpComposeSiteVariantSelection.
                if (!exprErrors.empty()) {
                    it = vselMap.erase(it);
                    continue;
                }
            }
            ++it;
        }

        result.insert(vselMap.begin(), vselMap.end());
    }
    return result;
}

std::string
PcpPrimIndex::GetSelectionAppliedForVariantSet(
    const std::string &variantSet) const
{
    for (const PcpNodeRef &node: GetNodeRange()) {
        if (node.GetPath().IsPrimVariantSelectionPath()) {
            std::pair<std::string, std::string> vsel =
                node.GetPath().GetVariantSelection();
            if (vsel.first == variantSet)
                return vsel.second;
        }
    }
    return std::string();
}

////////////////////////////////////////////////////////////////////////

template <class T>
static bool 
_CheckIfEquivalent(const T* lhsPtr, const T* rhsPtr)
{
    if (lhsPtr == rhsPtr) {
        return true;
    }

    static const T empty;
    const T& lhs = (lhsPtr ? *lhsPtr : empty);
    const T& rhs = (rhsPtr ? *rhsPtr : empty);
    return lhs == rhs;
}

bool 
PcpPrimIndexInputs::IsEquivalentTo(const PcpPrimIndexInputs& inputs) const
{
    // Don't consider the PcpCache when determining equivalence, as
    // prim index computation is independent of the cache.
    return 
        _CheckIfEquivalent(variantFallbacks, inputs.variantFallbacks) && 
        _CheckIfEquivalent(includedPayloads, inputs.includedPayloads) && 
        cull == inputs.cull;
}

////////////////////////////////////////////////////////////////////////

PcpNodeRef 
PcpPrimIndexOutputs::Append(PcpPrimIndexOutputs&& childOutputs, 
                            const PcpArc& arcToParent,
                            PcpErrorBasePtr *error)
{
    PcpNodeRef parent = arcToParent.parent;
    PcpNodeRef newNode = parent.InsertChildSubgraph(
        childOutputs.primIndex.GetGraph(), arcToParent, error);
    if (!newNode) {
        return newNode;
    }

    if (childOutputs.primIndex.GetGraph()->HasPayloads()) {
        parent.GetOwningGraph()->SetHasPayloads(true);
    }

    dynamicFileFormatDependency.AppendDependencyData(
        std::move(childOutputs.dynamicFileFormatDependency));

    expressionVariablesDependency.AppendDependencyData(
        std::move(childOutputs.expressionVariablesDependency));

    culledDependencies.insert(
        culledDependencies.end(),
        std::make_move_iterator(childOutputs.culledDependencies.begin()),
        std::make_move_iterator(childOutputs.culledDependencies.end()));

    allErrors.insert(
        allErrors.end(), 
        childOutputs.allErrors.begin(), childOutputs.allErrors.end());

    if (childOutputs.payloadState == NoPayload) {
        // Do nothing, keep our payloadState.
    }
    else if (payloadState == NoPayload) {
        // Take the child's payloadState.
        payloadState = childOutputs.payloadState;
    }
    else if (childOutputs.payloadState != payloadState) {
        // Inconsistent payload state -- issue a warning.
        TF_WARN("Inconsistent payload states for primIndex <%s> -- "
                "parent=%d vs child=%d; taking parent=%d\n",
                primIndex.GetPath().GetText(),
                payloadState, childOutputs.payloadState, payloadState);
    }
                
    return newNode;
}

////////////////////////////////////////////////////////////////////////

static void
Pcp_BuildPrimIndex(
    const PcpLayerStackSite & site,
    const PcpLayerStackSite & rootSite,
    int ancestorRecursionDepth,
    bool evaluateImpliedSpecializes,
    bool evaluateVariants,
    bool rootNodeShouldContributeSpecs,
    PcpPrimIndex_StackFrame *previousFrame,
    const PcpPrimIndexInputs& inputs,
    PcpPrimIndexOutputs* outputs);

static inline bool
_NodeCanBeCulled(const PcpNodeRef& node, 
                 const PcpLayerStackSite& rootLayerStack);

static void
_GatherNodesRecursively(const PcpNodeRef& node,
                        std::vector<PcpNodeRef> *result);

static bool
_HasSpecializesChild(const PcpNodeRef & parent)
{
    TF_FOR_ALL(child, Pcp_GetChildrenRange(parent)) {
        if (PcpIsSpecializeArc((*child).GetArcType()))
            return true;
    }
    return false;
}

// The implied specializes algorithm wants to start at the
// most ancestral parent of the given node that is a specializes
// arc, if such a node exists.
static PcpNodeRef
_FindStartingNodeForImpliedSpecializes(const PcpNodeRef& node)
{
    PcpNodeRef specializesNode;
    for (PcpNodeRef n = node, e = n.GetRootNode(); n != e; 
         n = n.GetParentNode()) {
        if (PcpIsSpecializeArc(n.GetArcType())) {
            specializesNode = n;
        }
    }
    return specializesNode;
}

static bool
_HasClassBasedChild(const PcpNodeRef & parent)
{
    TF_FOR_ALL(child, Pcp_GetChildrenRange(parent)) {
        if (PcpIsClassBasedArc((*child).GetArcType()))
            return true;
    }
    return false;
}

// Given class-based node n, returns the 'starting' node where implied class
// processing should begin in order to correctly propagate n through the
// graph.
//
// The starting node will generally be the starting node of the class hierarchy
// that n is a part of. For instance, in the simple case:
//
//    inh     inh     inh
//  I ---> C1 ---> C2 ---> C3 ...
//
// Given any of { C1, C2, C3, ... }, the starting node would be I 
// (See Pcp_FindStartingNodeOfClassHierarchy). This causes the entire class
// hierarchy to be propagated as a unit. If we were to propagate each class
// individually, it would be as if I inherited directly from C1, C2, and C3,
// which is incorrect.
// 
// This gets more complicated when ancestral classes are involved. Basically,
// when a class-based node is added, we have to take into account the location
// of that node's site relative to the ancestral class to determine where to
// start from.
//
// Consider the prim /M/I/A in the following example:
//
//          reference
// M --------------------------> R
// |                             |
// +- CA <----+ implied inh.     +- CA <----+ inherit
// |          |                  |          |
// +- C1 <----|--+ implied inh.  +- C1 <----|--+ inherit
// |  |       |  |               |  |       |  |
// |  +- A ---+  |               |  +- A ---+  | 
// |             |               |             |
// +- I ---------+               +- I ---------+
//    |                             |
//    +- A                          +- A
// 
// /M/I/A inherits opinions from /M/C1/A due to the ancestral inherit arc
// between /M/I and /M/C1. Then, /M/C1/A inherits opinions from /M/CA. 
// However, /M/I/A does NOT explicitly inherit opinions from /M/CA. If it did,
// opinions from /M/CA would show up twice. 
//
// To ensure /M/I/A does not explicitly inherit from /M/CA, when /R/CA is added
// the chain of inherit nodes:        inh          inh
//                             /R/I/A ---> /R/C1/A ---> /R/CA
//
// Must be propagated as a single unit, even though it does not form a single 
// class hierarchy. So, the starting node would be /R/I/A.
//
// Contrast that with this case:
//
//          reference
// M --------------------------> R
// |                             |
// +- C1 <------------+ implied  +- C1 <------------+ inherit
// |  |               | inh.     |  |               |
// |  +- CA <-+ impl. |          |  +- CA <-+ inh.  |
// |  |       | inh.  |          |  |       |       |
// |  +- A ---+       |          |  +- A ---+       | 
// |                  |          |                  |
// +- I --------------+          +- I --------------+
//    |                             |
//    +- CA <-+                     +- CA <-+
//    |       | implied inh.        |       | implied inh.
//    +- A ---+                     +- A ---+
//
// In this case, we do expect /M/I/A to explicitly inherit from /M/I/CA.
// When /R/C1/CA is added, the chain:         inh          inh
//                                     /R/I/A ---> /R/C1/A ---> /R/C1/CA
//
// Must be propagated as a single unit (Note that this *is* a class hierarchy).
// So, the starting node would be /R/I/A.
//
// This (deceivingly simple) function accounts for all this.
// These variations are captured in the TrickyNestedClasses museum cases.
static PcpNodeRef
_FindStartingNodeForImpliedClasses(const PcpNodeRef& n)
{
    TF_VERIFY(PcpIsClassBasedArc(n.GetArcType()));

    PcpNodeRef startNode = n;

    while (PcpIsClassBasedArc(startNode.GetArcType())) {
        const std::pair<PcpNodeRef, PcpNodeRef> instanceAndClass = 
            Pcp_FindStartingNodeOfClassHierarchy(startNode);

        const PcpNodeRef& instanceNode = instanceAndClass.first;
        const PcpNodeRef& classNode = instanceAndClass.second;

        startNode = instanceNode;

        // If the instance that inherits the class hierarchy is itself
        // a class-based node, there must be an ancestral inherit arc which 
        // we need to consider. If the class being inherited from is a
        // namespace child of the ancestral class (the second case shown
        // above), we're done. Otherwise, we'll iterate again to find the
        // start of the ancestral class hierarchy.
        if (PcpIsClassBasedArc(instanceNode.GetArcType())) {
            const SdfPath ancestralClassPath = 
                instanceNode.GetPathAtIntroduction();
            const bool classHierarchyIsChildOfAncestralHierarchy = 
                classNode.GetPath().HasPrefix(ancestralClassPath);

            if (classHierarchyIsChildOfAncestralHierarchy) {
                break;
            }
        }
    }

    return startNode;
}

// This is a convenience function to create a map expression
// that maps a given source path to a target node, composing in
// relocations and layer offsets if any exist.
static PcpMapExpression
_CreateMapExpressionForArc(const SdfPath &sourcePath, 
                           const PcpNodeRef &targetNode,
                           const PcpPrimIndexInputs &inputs,
                           const SdfLayerOffset &offset = SdfLayerOffset())
{
    const SdfPath targetPath = targetNode.GetPath().StripAllVariantSelections();

    PcpMapFunction::PathMap sourceToTargetMap;
    sourceToTargetMap[sourcePath] = targetPath;
    PcpMapExpression arcExpr = PcpMapExpression::Constant(
        PcpMapFunction::Create( sourceToTargetMap, offset ) );

    // Apply relocations that affect namespace at and below this site.
    if (!inputs.usd) {
        arcExpr = targetNode.GetLayerStack()
            ->GetExpressionForRelocatesAtPath(targetPath)
            .Compose(arcExpr);
    }

    return arcExpr;
}

// Bitfield of composition arc types
enum _ArcFlags {
    _ArcFlagInherits    = 1<<0,
    _ArcFlagVariants    = 1<<1,
    _ArcFlagReferences  = 1<<2,
    _ArcFlagPayloads    = 1<<3,
    _ArcFlagSpecializes = 1<<4
};

// Scan a node's specs for presence of fields describing composition arcs.
// This is used as a preflight check to confirm presence of these arcs
// before performing additional work to evaluate them.
// Return a bitmask of the arc types found.
inline static size_t
_ScanArcs(PcpNodeRef const& node)
{
    size_t arcs = 0;
    SdfPath const& path = node.GetPath();
    for (SdfLayerRefPtr const& layer: node.GetLayerStack()->GetLayers()) {
        if (!layer->HasSpec(path)) {
            continue;
        }
        if (layer->HasField(path, SdfFieldKeys->InheritPaths)) {
            arcs |= _ArcFlagInherits;
        }
        if (layer->HasField(path, SdfFieldKeys->VariantSetNames)) {
            arcs |= _ArcFlagVariants;
        }
        if (layer->HasField(path, SdfFieldKeys->References)) {
            arcs |= _ArcFlagReferences;
        }
        if (layer->HasField(path, SdfFieldKeys->Payload)) {
            arcs |= _ArcFlagPayloads;
        }
        if (layer->HasField(path, SdfFieldKeys->Specializes)) {
            arcs |= _ArcFlagSpecializes;
        }
    }
    return arcs;
}

////////////////////////////////////////////////////////////////////////

namespace {

/// A task to perform on a particular node.
struct Task {
    /// This enum must be in evaluation priority order.
    enum Type {
        EvalNodeRelocations,
        EvalImpliedRelocations,
        EvalNodeReferences,
        EvalNodePayload,
        EvalNodeInherits,
        EvalImpliedClasses,
        EvalNodeSpecializes,
        EvalImpliedSpecializes,
        EvalNodeVariantSets,
        EvalNodeVariantAuthored,
        EvalNodeVariantFallback,
        EvalNodeVariantNoneFound,
        None
    };

    // This sorts tasks in priority order from lowest priority to highest
    // priority, so highest priority tasks come last.
    struct PriorityOrder {
        inline bool operator()(const Task& a, const Task& b) const {
            if (a.type != b.type) {
                return a.type > b.type;
            }
            // Node strength order is costly to compute, so avoid it for
            // arcs with order-independent results.
            switch (a.type) {
            case EvalNodePayload:
                // Payloads can have dynamic file format arguments that depend 
                // on non-local information, so we must process these in 
                // strength order.
                return PcpCompareNodeStrength(a.node, b.node) == 1;
            case EvalNodeVariantAuthored:
            case EvalNodeVariantFallback:
                // Variant selections can depend on non-local information
                // so we must visit them in strength order.
                if (a.node != b.node) {
                    return PcpCompareNodeStrength(a.node, b.node) == 1;
                } else {
                    // Lower-number vsets have strength priority.
                    return a.vsetNum > b.vsetNum;
                }
            case EvalNodeVariantNoneFound:
                // In the none-found case, we only need to ensure a consistent
                // and distinct order for distinct tasks, the specific order can
                // be arbitrary.
                if (a.node != b.node) {
                    return a.node > b.node;
                } else {
                    return a.vsetNum > b.vsetNum;
                }
            case EvalImpliedClasses:
                // When multiple implied classes tasks are queued for different
                // nodes, ordering matters in that ancestor nodes must be 
                // processed after their descendants. This minimally guarantees
                // that by relying on an undocumented implementation detail 
                // of the less than operator, which we use for performance
                // rather than doing a more expensive graph traversal.
                //
                // The less than operator compares the nodes' index in
                // the node graph. Each node's index is assigned incrementally
                // as its added to its parent in the graph so b.node having a 
                // greater index than a.node guarantees that b.node is not an 
                // ancestor of a.node.
                // 
                // Note that while the composition cases where this order 
                // matters are extremely rare, they do come up. The museum case
                // ImpliedAndAncestralInherits_ComplexEvaluation details the
                // minimal (though still complex) case that requires this 
                // ordering be correct and should be referred to if a detailed
                // explanation is desired.
                return b.node > a.node;
            default:
                // Arbitrary order
                return a.node > b.node;
            }
        }
    };

    explicit Task(Type type, const PcpNodeRef& node = PcpNodeRef())
        : type(type)
        , vsetNum(0)
        , node(node)
    { }

    Task(Type type, const PcpNodeRef& node,
         std::string &&vsetName, int vsetNum)
        : type(type)
        , vsetNum(vsetNum)
        , node(node)
        , vsetName(std::move(vsetName))
    { }

    Task(Type type, const PcpNodeRef& node,
         std::string const &vsetName, int vsetNum)
        : type(type)
        , vsetNum(vsetNum)
        , node(node)
        , vsetName(vsetName)
    { }

    // TfHash support.
    template <class HashState>
    friend void TfHashAppend(HashState &h, Task const &task) {
        h.Append(task.type, task.node, task.vsetNum, task.vsetName);
    }

    inline bool operator==(Task const &rhs) const {
        return type == rhs.type && node == rhs.node &&
            vsetName == rhs.vsetName && vsetNum == rhs.vsetNum;
    }

    inline bool operator!=(Task const &rhs) const { return !(*this == rhs); }

    friend void swap(Task &lhs, Task &rhs) {
        std::swap(lhs.type, rhs.type);
        std::swap(lhs.node, rhs.node);
        lhs.vsetName.swap(rhs.vsetName);
        std::swap(lhs.vsetNum, rhs.vsetNum);
    }

    // Stream insertion operator for debugging.
    friend std::ostream &operator<<(std::ostream &os, Task const &task) {
        unsigned char buf[sizeof(PcpNodeRef)] = { 0 };
        memcpy(buf, &task.node, sizeof(task.node));
        std::string bytes;
        for (char b: buf) {
            bytes += TfStringPrintf("%x", int(b));
        }
        os << TfStringPrintf(
            "Task(type=%s, node=%s, nodePath=<%s>, nodeSite=<%s>",
            TfEnum::GetName(task.type).c_str(),
            bytes.c_str(),
            task.node.GetPath().GetText(),
            TfStringify(task.node.GetSite()).c_str());
        if (!task.vsetName.empty()) {
            os << TfStringPrintf(", vsetName=%s, vsetNum=%d",
                                 task.vsetName.c_str(), task.vsetNum);
        }
        return os << ")";
    }        
    
    Type type;
    int vsetNum; // << only for variant tasks.
    PcpNodeRef node;
    std::string vsetName; // << only for variant tasks.
};

}

TF_REGISTRY_FUNCTION(TfEnum) {
    TF_ADD_ENUM_NAME(Task::EvalNodeRelocations);
    TF_ADD_ENUM_NAME(Task::EvalImpliedRelocations);
    TF_ADD_ENUM_NAME(Task::EvalNodeReferences);
    TF_ADD_ENUM_NAME(Task::EvalNodePayload);
    TF_ADD_ENUM_NAME(Task::EvalNodeInherits);
    TF_ADD_ENUM_NAME(Task::EvalImpliedClasses);
    TF_ADD_ENUM_NAME(Task::EvalNodeSpecializes);
    TF_ADD_ENUM_NAME(Task::EvalImpliedSpecializes);
    TF_ADD_ENUM_NAME(Task::EvalNodeVariantSets);
    TF_ADD_ENUM_NAME(Task::EvalNodeVariantAuthored);
    TF_ADD_ENUM_NAME(Task::EvalNodeVariantFallback);
    TF_ADD_ENUM_NAME(Task::EvalNodeVariantNoneFound);
    TF_ADD_ENUM_NAME(Task::None);
}

// Pcp_PrimIndexer is used during prim cache population to track which
// tasks remain to finish building the graph.  As new nodes are added,
// we add task entries to this structure, which ensures that we
// process them in an appropriate order.
//
// This is the high-level control logic for the population algorithm.
// At each step, it determines what will happen next.
//
// Notes on the algorithm:
//
// - We can process inherits, and implied inherits in any order
//   any order, as long as we finish them before moving on to
//   deciding references and variants.  This is because evaluating any
//   arcs of the former group does not affect how we evaluate other arcs
//   of that group -- but they do affect how we evaluate references,
//   variants and payloads.  Specifically, they may introduce information
//   needed to evaluate references, opinions with variants selections, 
//   or overrides to the payload target path.
//
//   It is important to complete evaluation of the former group
//   before proceeding to references/variants/payloads so that we gather
//   as much information as available before deciding those arcs.
//
// - We only want to process a payload when there is nothing else
//   left to do.  Again, this is to ensure that we have discovered
//   any opinions which may affect the payload arc, including
//   those inside variants.
//
// - At each step, we may introduce a new node that returns us
//   to an earlier stage of the algorithm.  For example, a payload
//   may introduce nodes that contain references, inherits, etc.
//   We need to process them to completion before we return to
//   check variants, and so on.
//
struct Pcp_PrimIndexer
{
    // The root site for the prim indexing process.
    const PcpLayerStackSite rootSite;

    // Total depth of ancestral recursion.
    const int ancestorRecursionDepth;

    // Context for the prim index we are building.
    const PcpPrimIndexInputs &inputs;
    PcpPrimIndexOutputs* const outputs;

    // The previousFrame tracks information across recursive invocations
    // of Pcp_BuildPrimIndex() so that recursive indexes can query
    // outer indexes.  This is used for cycle detection as well as
    // composing the variant selection.
    PcpPrimIndex_StackFrame* const previousFrame;

    // Open tasks, maintained as a max-heap (via push_heap, pop_heap, etc) using
    // Task::PriorityOrder().
    using _TaskQueue = std::vector<Task>;
    _TaskQueue tasks;

    // A set for uniquing implied inherits & specializes tasks.
    using _TaskUniq = pxr_tsl::robin_set<Task, TfHash>;
    _TaskUniq taskUniq;

    const bool evaluateImpliedSpecializes;
    const bool evaluateVariants;

#ifdef PCP_DIAGNOSTIC_VALIDATION
    /// Diagnostic helper to make sure we don't revisit sites.
    PcpNodeRefHashSet seen;
#endif // PCP_DIAGNOSTIC_VALIDATION

    Pcp_PrimIndexer(PcpPrimIndexInputs const &inputs_,
                    PcpPrimIndexOutputs *outputs_,
                    PcpLayerStackSite rootSite_,
                    int ancestorRecursionDepth_,
                    PcpPrimIndex_StackFrame *previousFrame_=nullptr,
                    bool evaluateImpliedSpecializes_=true,
                    bool evaluateVariants_=true)
        : rootSite(rootSite_)
        , ancestorRecursionDepth(ancestorRecursionDepth_)
        , inputs(inputs_)
        , outputs(outputs_)
        , previousFrame(previousFrame_)
        , evaluateImpliedSpecializes(evaluateImpliedSpecializes_)
        , evaluateVariants(evaluateVariants_)
    {
    }

    inline PcpPrimIndex const *GetOriginatingIndex() const {
        return _GetOriginatingIndex(previousFrame, outputs);
    }

    static inline bool _IsImpliedTaskType(Task::Type taskType) {
        // Bitwise-or to avoid branches. 
        return (taskType == Task::Type::EvalImpliedClasses) |
            (taskType == Task::Type::EvalImpliedSpecializes);
    }

    void AddTask(Task &&task) {
        if (tasks.empty()) {
            tasks.reserve(8); // Typically we have about this many tasks, and
                              // this results in a single 256 byte allocation.
        }
        // For the EvalImplied{Classes,Specializes} tasks, we must check and
        // skip dupes.  We can get dupes for these due to the way that implied
        // inherits and specializes are propagated back.
        if (!_IsImpliedTaskType(task.type) || taskUniq.insert(task).second) {
            tasks.push_back(std::move(task));
            push_heap(tasks.begin(), tasks.end(), Task::PriorityOrder());
        }
    }

    // Select the next task to perform.
    Task PopTask() {
        Task task(Task::Type::None);
        if (!tasks.empty()) {
            pop_heap(tasks.begin(), tasks.end(), Task::PriorityOrder());
            task = std::move(tasks.back());
            tasks.pop_back();
            if (_IsImpliedTaskType(task.type)) {
                taskUniq.erase(task);
            }
        }
        return task;
    }

    // Add this node and its children to the task queues.  
    inline void
    _AddTasksForNodeRecursively(
        const PcpNodeRef& n, 
        bool skipTasksForExpressedArcs,
        bool skipCompletedNodesForImpliedSpecializes,
        bool isUsd) 
    {
#ifdef PCP_DIAGNOSTIC_VALIDATION
        TF_VERIFY(seen.count(n) == 0, "Already processed <%s>",
                  n.GetPath().GetText());
        seen.insert(n);
#endif // PCP_DIAGNOSTIC_VALIDATION

        TF_FOR_ALL(child, Pcp_GetChildrenRange(n)) {
            _AddTasksForNodeRecursively(
                *child, 
                skipTasksForExpressedArcs, 
                skipCompletedNodesForImpliedSpecializes, isUsd);
        }

        // If the node does not have specs or cannot contribute specs,
        // we can avoid even enqueueing certain kinds of tasks that will
        // end up being no-ops.
        const bool contributesSpecs = n.HasSpecs() && n.CanContributeSpecs();

        // Preflight scan for arc types that are present in specs.
        // This reduces pressure on the task queue, and enables more
        // data access locality, since we avoid interleaving tasks that
        // re-visit sites later only to determine there is no work to do.
        const size_t arcMask = contributesSpecs ? _ScanArcs(n) : 0;

        // If the caller tells us the new node and its children were already
        // indexed, we do not need to re-scan them for certain arcs based on
        // what was already completed.
        if (skipCompletedNodesForImpliedSpecializes) {
            // In this case, we only need to add tasks that come after 
            // implied specializes.
            if (evaluateVariants && (arcMask & _ArcFlagVariants)) {
                AddTask(Task(Task::Type::EvalNodeVariantSets, n));
            }
        } else {
            // Payloads and variants have expensive
            // sorting semantics, so do a preflight check
            // to see if there is any work to do.
            if (evaluateVariants && (arcMask & _ArcFlagVariants)) {
                AddTask(Task(Task::Type::EvalNodeVariantSets, n));
            }
            if (!skipTasksForExpressedArcs) {
                // In some cases, we don't want to add the tasks for expressed 
                // arcs because we're adding nodes from an already composed 
                // subtree that has already processed these arcs. 
                // 
                // These cases include adding a subtree that was recursively 
                // prim indexed for ancestral opinions or propagating a 
                // specializes subtree back down to its origin node.
                if (arcMask & _ArcFlagSpecializes) {
                    AddTask(Task(Task::Type::EvalNodeSpecializes, n));
                }
                if (arcMask & _ArcFlagInherits) {
                    AddTask(Task(Task::Type::EvalNodeInherits, n));
                }
                if (arcMask & _ArcFlagPayloads) {
                    AddTask(Task(Task::Type::EvalNodePayload, n));
                }
                if (arcMask & _ArcFlagReferences) {
                    AddTask(Task(Task::Type::EvalNodeReferences, n));
                }
                if (!isUsd) {
                    AddTask(Task(Task::Type::EvalNodeRelocations, n));
                }
            }
            if (!isUsd && n.GetArcType() == PcpArcTypeRelocate) {
                AddTask(Task(Task::Type::EvalImpliedRelocations, n));
            }
        }
    }

    void AddTasksForRootNode(const PcpNodeRef& rootNode) {
        return _AddTasksForNodeRecursively(
            rootNode, 
            /*skipTasksForExpressedArcs=*/false,
            /*skipCompletedNodesForImpliedSpecializes=*/false,
            /*isUsd=*/inputs.usd);
    }

    void AddTasksForNode(
        const PcpNodeRef& n, 
        bool skipTasksForExpressedArcs,
        bool skipCompletedNodesForImpliedSpecializes) {

        // Any time we add an edge to the graph, we may need to update
        // implied class edges.
        if (!skipCompletedNodesForImpliedSpecializes) {
            if (PcpIsClassBasedArc(n.GetArcType())) {
                // The new node is itself class-based.  Find the starting
                // prim of the chain of classes the node is a part of, and 
                // propagate the entire chain as a single unit.
                if (PcpNodeRef base = _FindStartingNodeForImpliedClasses(n)) {
                    AddTask(Task(Task::Type::EvalImpliedClasses, base));
                }
            } else if (_HasClassBasedChild(n)) {
                // The new node is not class-based -- but it has class-based
                // children.  Such children represent inherits found during the
                // recursive computation of the node's subgraph.  We need to
                // pick them up and continue propagating them now that we are
                // merging the subgraph into the parent graph.
                AddTask(Task(Task::Type::EvalImpliedClasses, n));
            }
            if (evaluateImpliedSpecializes) {
                if (PcpNodeRef base = 
                    _FindStartingNodeForImpliedSpecializes(n)) {
                    // We're adding a new specializes node or a node beneath
                    // a specializes node.  Add a task to propagate the subgraph
                    // beneath this node to the appropriate location.
                    AddTask(Task(Task::Type::EvalImpliedSpecializes, base));
                }
                else if (_HasSpecializesChild(n)) {
                    // The new node is not a specializes node or beneath a
                    // specializes node, but has specializes children.
                    // Such children represent arcs found during the recursive 
                    // computation of the node's subgraph.  We need to pick them 
                    // up and continue propagating them now that we are
                    // merging the subgraph into the parent graph.
                    AddTask(Task(Task::Type::EvalImpliedSpecializes, n));
                }
            }
        }

        // Recurse over all of the rest of the nodes.  (We assume that any
        // embedded class hierarchies have already been propagated to
        // the top node n, letting us avoid redundant work.)
        _AddTasksForNodeRecursively(
            n, skipTasksForExpressedArcs, 
            skipCompletedNodesForImpliedSpecializes, inputs.usd);

        _DebugPrintTasks("After AddTasksForNode");
    }

    inline void _DebugPrintTasks(char const *label) const {
#if 0
        printf("-- %s ----------------\n", label);
        _TaskQueue tq(tasks);
        sort_heap(tq.begin(), tq.end(), Task::PriorityOrder());
        for (auto iter = tq.rbegin(); iter != tq.rend(); ++iter) {
            printf("%s\n", TfStringify(*iter).c_str());
        }
        printf("----------------\n");
#endif
    }

    // Retry any variant sets that previously failed to find an authored
    // selection to take into account newly-discovered opinions.
    // EvalNodeVariantNoneFound is a placeholder representing variants
    // that were previously visited and yielded no variant; it exists
    // solely for this function to be able to find and retry them.
    void RetryVariantTasks() {

        // Scan for fallback / none-found variant tasks and promote to authored.
        // This increases priority, so heap sift-up any modified tasks.
        for (auto i = tasks.begin(), e = tasks.end(); i != e; ++i) {
            Task &t = *i;
            if (t.type == Task::Type::EvalNodeVariantFallback ||
                t.type == Task::Type::EvalNodeVariantNoneFound) {
                // Promote the type and re-heap this task.
                t.type = Task::Type::EvalNodeVariantAuthored;
                push_heap(tasks.begin(), i + 1, Task::PriorityOrder());
            }
        }

        _DebugPrintTasks("After RetryVariantTasks");
    }

    // Convenience function to record an error both in this primIndex's
    // local errors vector and the allErrors vector.
    void RecordError(const PcpErrorBasePtr &err) {
        RecordError(err, &outputs->primIndex, &outputs->allErrors);
    }

    // Convenience function to record an error both in this primIndex's
    // local errors vector and the allErrors vector.
    static void RecordError(const PcpErrorBasePtr &err,
                            PcpPrimIndex *primIndex,
                            PcpErrorVector *allErrors) {
        // Capacity errors are reported at most once.
        if (err->errorType == PcpErrorType_IndexCapacityExceeded ||
            err->errorType == PcpErrorType_ArcCapacityExceeded ||
            err->errorType == PcpErrorType_ArcNamespaceDepthCapacityExceeded) {

            for (PcpErrorBasePtr const& e: *allErrors) {
                if (e->errorType == err->errorType) {
                    // Already reported.
                    return;
                }
            }
        }

        allErrors->push_back(err);
        if (!primIndex->_localErrors) {
            primIndex->_localErrors.reset(new PcpErrorVector);
        }
        primIndex->_localErrors->push_back(err);
    }
};

// Returns true if there is a prim spec associated with the specified node
// or any of its descendants.
static bool
_PrimSpecExistsUnderNode(
    const PcpNodeRef &node,
    Pcp_PrimIndexer *indexer) 
{
    // Check for prim specs at this node's site.
    if (node.HasSpecs())
        return true;
    
    // Recursively check this node's children.
    TF_FOR_ALL(child, Pcp_GetChildrenRange(node)) {
        if (_PrimSpecExistsUnderNode(*child, indexer))
            return true;
    }
    return false;
}

// Mark an entire subtree of nodes as inert.
static void
_InertSubtree(
    PcpNodeRef node)
{
    node.SetInert(true);
    TF_FOR_ALL(child, Pcp_GetChildrenRange(node)) {
        _InertSubtree(*child);
    }
}

inline static bool
_HasAncestorCycle(
    const PcpLayerStackSite& parentNodeSite,
    const PcpLayerStackSite& childNodeSite )
{
    // For example, a cycle exists if in the same layer stack 
    // the prim at /A/B adds a child arc to /A or the prim at 
    // /A adds a child arc to /A/B.
    return parentNodeSite.layerStack == childNodeSite.layerStack
        && (parentNodeSite.path.HasPrefix(childNodeSite.path)
            || childNodeSite.path.HasPrefix(parentNodeSite.path));
}

inline static bool
_FindAncestorCycleInParentGraph(const PcpNodeRef &parentNode, 
                                const PcpLayerStackSite& childNodeSite)
{
    // We compare the targeted site to each previously-visited site: 
    for (PcpNodeRef node = parentNode; node; node = node.GetParentNode()) {
        if (_HasAncestorCycle(node.GetSite(), childNodeSite)) {
            return true;
        }
    }
    return false;
}

static bool
_IsImpliedClassBasedArc(
    PcpArcType arcType,
    const PcpNodeRef &parent,
    const PcpNodeRef &origin)
{
    return PcpIsClassBasedArc(arcType) && parent != origin;
}

static bool
_IsImpliedClassBasedArc(const PcpNodeRef& node)
{
    return _IsImpliedClassBasedArc(
        node.GetArcType(), node.GetParentNode(), node.GetOriginNode());
}

// Check that no cycles are being introduced by adding this arc.
static PcpErrorArcCyclePtr
_CheckForCycle(
    const PcpNodeRef &parent, 
    const PcpNodeRef &origin,
    PcpArcType arcType,
    const PcpLayerStackSite &childSite,
    PcpPrimIndex_StackFrame *previousFrame )
{
    // XXX:RelocatesSourceNodes: Don't check for cycles in placeholder
    // implied class nodes under relocates. These children of Relocates
    // nodes can yield invalid sites, because the arc will include
    // the effect of relocations but the Relocates node is the source
    // path. In this case, we won't be adding opinions anyway, so we
    // don't need to check for cycles.
    if (_IsImpliedClassBasedArc(arcType, parent, origin)) {
        // Skip across parent class arcs.
        PcpPrimIndex_StackFrameIterator j(parent, previousFrame);
        while (j.node 
               && _IsImpliedClassBasedArc(j.GetArcType(), parent, origin)) {
            j.Next();
        }
        if (j.node && j.GetArcType() == PcpArcTypeRelocate) {
            // This is a class arc under a relocate.
            // Do not count this as a cycle.
            return PcpErrorArcCyclePtr();
        }
    }

    // Don't check for cycles for variant arcs, since these just
    // represent the selection of a particular branch of scene
    // description. For example, adding a variant selection child
    // /A{v=sel} to parent /A is not a cycle, even though the child
    // path is prefixed by the parent.
    if (arcType == PcpArcTypeVariant) {
        return PcpErrorArcCyclePtr();
    }

    bool foundCycle = false;

    // If the the current graph is a subgraph that is being recursively built
    // for another node, we have to crawl up the parent graph as well to check
    // for cycles.
    PcpLayerStackSite childSiteInStackFrame = childSite;
    for (PcpPrimIndex_StackFrameIterator it(parent, previousFrame);
         it.node; it.NextFrame()) {

        // Check for a cycle in the parent's current graph.
        if (_FindAncestorCycleInParentGraph(it.node, childSiteInStackFrame)) {
            foundCycle = true;
            break;
        }

        // In some cases we need to convert the child site's path into the 
        // path it will have when its owning subgraph is added to the parent
        // graph in order to correctly check for cycles. This is best 
        // explained with a simple example:
        //
        //    /A
        //    /A/B
        //    /A/C (ref = /D/B)
        //
        //    /D (ref = /A)
        //
        // If you compute the prim index /D/C it will have a reference arc 
        // to /A/C because /D references /A. When the index then goes to
        // to add the reference arc to /D/B from /A/C it initiates a 
        // recursive subgraph computation of /D/B. 
        // 
        // When we build the subgraph prim index for /D/B, the first step
        // is to compute its namespace ancestor which builds an index for
        // /D. When the index for /D tries to add its reference arc to /A,
        // we end up here in this function to check for cycles.
        // 
        // If we just checked for cycles using the child site's current 
        // path, /A, we'd find an ancestor cycle when we go up to the parent
        // graph for the node /A/C. However, the requested subgraph is for 
        // /D/B not /D, so the child site will actually be /A/B instead of 
        // /A when the subgraph reference arc is actually added for node 
        // /A/C. Adding a node /A/B does not introduce any cycles.
        if (it.previousFrame) {
            const SdfPath& requestedPathForCurrentGraph = 
                it.previousFrame->requestedSite.path;
            const SdfPath& currentPathForCurrentGraph = 
                it.node.GetRootNode().GetPath();

            childSiteInStackFrame.path =
                currentPathForCurrentGraph == childSiteInStackFrame.path ?
                requestedPathForCurrentGraph :
                requestedPathForCurrentGraph.ReplacePrefix(
                    currentPathForCurrentGraph,
                    childSiteInStackFrame.path);
        }
    }

    if (foundCycle) {
        PcpErrorArcCyclePtr err = PcpErrorArcCycle::New();
        // Traverse the parent chain to build a list of participating arcs.
        PcpSiteTrackerSegment seg;
        for (PcpPrimIndex_StackFrameIterator i(parent, previousFrame); 
             i.node; i.Next()) {
            seg.site = i.node.GetSite();
            seg.arcType = i.GetArcType();
            err->cycle.push_back(seg);
        }
        // Reverse the list to order arcs from root to leaf.
        std::reverse(err->cycle.begin(), err->cycle.end());
        // Retain the root site.
        err->rootSite = err->cycle.front().site;
        // There is no node for the last site in the chain, so report it
        // directly.
        seg.site = childSite;
        seg.arcType = arcType;
        err->cycle.push_back(seg);
        return err;
    }

    return PcpErrorArcCyclePtr();
}

static void
_AddCulledDependencies(
    const PcpNodeRef& node,
    std::vector<PcpCulledDependency>* culledDeps)
{
    if (node.IsCulled()) {
        Pcp_AddCulledDependency(node, culledDeps);
    }

    TF_FOR_ALL(child, Pcp_GetChildrenRange(node)) {
        _AddCulledDependencies(*child, culledDeps);
    }
};

// Add an arc of the given type from the parent node to the child site,
// and track any new tasks that result.  Return the new node.
//
// If includeAncestralOpinions is specified, recursively build and
// include the ancestral opinions that would affect the new site.
//
static PcpNodeRef
_AddArc(
    const PcpArcType arcType,
    PcpNodeRef parent,
    PcpNodeRef origin,
    const PcpLayerStackSite & site,
    PcpMapExpression mapExpr,
    int arcSiblingNum,
    int namespaceDepth,
    bool directNodeShouldContributeSpecs,
    bool includeAncestralOpinions,
    bool skipDuplicateNodes,
    bool skipImpliedSpecializesCompletedNodes,
    bool skipTasksForExpressedArcs,
    Pcp_PrimIndexer *indexer )
{
    PCP_INDEXING_PHASE(
        indexer,
        parent, 
        "Adding new %s arc to %s from %s", 
        TfEnum::GetDisplayName(arcType).c_str(),
        Pcp_FormatSite(site).c_str(),
        Pcp_FormatSite(parent.GetSite()).c_str());

    PCP_INDEXING_MSG(
        indexer,
        parent, 
        "origin: %s\n"
        "arcSiblingNum: %d\n"
        "namespaceDepth: %d\n"
        "directNodeShouldContributeSpecs: %s\n"
        "includeAncestralOpinions: %s\n"
        "skipDuplicateNodes: %s\n"
        "skipImpliedSpecializesCompletedNodes: %s\n\n",
        origin ? Pcp_FormatSite(origin.GetSite()).c_str() : "<None>",
        arcSiblingNum,
        namespaceDepth,
        directNodeShouldContributeSpecs ? "true" : "false",
        includeAncestralOpinions ? "true" : "false",
        skipDuplicateNodes ? "true" : "false",
        skipImpliedSpecializesCompletedNodes ? "true" : "false");

    if (!TF_VERIFY(!mapExpr.IsNull())) {
        return PcpNodeRef();
    }

    // Check for cycles.  If found, report an error and bail.
    if (PcpErrorArcCyclePtr err =
        _CheckForCycle(parent, origin, arcType, site, indexer->previousFrame)) {
        indexer->RecordError(err);
        return PcpNodeRef();
    }

    // We (may) want to determine whether adding this arc would cause the 
    // final prim index to have nodes with the same site. If so, we need to 
    // skip over it, as adding the arc would cause duplicate opinions in the
    // final prim index.
    //
    // This is tricky -- we need to search the current graph being built as
    // well as those in the previous recursive calls to Pcp_BuildPrimIndex. 
    if (indexer->previousFrame) {
        skipDuplicateNodes |= indexer->previousFrame->skipDuplicateNodes;
    }

    if (skipDuplicateNodes) {
        PcpLayerStackSite siteToAddInCurrentGraph = site;

        bool foundDuplicateNode = false;
        for (PcpPrimIndex_StackFrameIterator it(parent, indexer->previousFrame);
             it.node; it.NextFrame()) {

            PcpPrimIndex_Graph *currentGraph = it.node.GetOwningGraph();
            if (currentGraph->GetNodeUsingSite(siteToAddInCurrentGraph)) {
                foundDuplicateNode = true;
                break;
            }
            
            // The graph in the previous stack frame may be at a different
            // level of namespace than the current graph. In order to search
            // it for this new node's site, we have to figure out what this
            // node's site would be once it was added to the previous graph.
            // Let's say we're in a recursive call to Pcp_BuildPrimIndex for 
            // prim /A/B, and that we're processing ancestral opinions for /A.
            // In doing so, we're adding an arc to site /C. That would be:
            //
            //   - requestedPathForCurrentGraph = /A/B
            //     currentPathForCurrentGraph = /A
            //     siteToAddInCurrentGraph.path = /C
            //
            // When the recursive call to Pcp_BuildPrimIndex is all done,
            // the arc to site /C will have become /C/B. This is the path
            // we need to use to search the graph in the previous frame. We
            // compute this path using a simple prefix replacement.
            if (it.previousFrame) {
                const SdfPath& requestedPathForCurrentGraph = 
                    it.previousFrame->requestedSite.path;
                const SdfPath& currentPathForCurrentGraph = 
                    currentGraph->GetRootNode().GetPath();

                siteToAddInCurrentGraph.path = 
                    requestedPathForCurrentGraph.ReplacePrefix(
                        currentPathForCurrentGraph,
                        siteToAddInCurrentGraph.path);
            }
        }

        if (foundDuplicateNode) {
            return PcpNodeRef();
        }
    }

    // Local opinions are not allowed at the source of a relocation (or below). 
    // This is colloquially known as the "salted earth" policy. We enforce 
    // this policy here to ensure we examine all arcs as they're being added.
    // Optimizations:
    // - We only need to do this for non-root prims because root prims can't
    //   be relocated. This is indicated by the includeAncestralOpinions flag.
    if (directNodeShouldContributeSpecs && includeAncestralOpinions) {
        const SdfRelocatesMap & layerStackRelocates =
            site.layerStack->GetRelocatesSourceToTarget();
        SdfRelocatesMap::const_iterator
            i = layerStackRelocates.lower_bound( site.path );
        if (i != layerStackRelocates.end() && i->first.HasPrefix(site.path)) {
            directNodeShouldContributeSpecs = false;
        }
    }

    // Set up the arc.
    PcpArc newArc;
    newArc.type = arcType;
    newArc.mapToParent = mapExpr;
    newArc.parent = parent;
    newArc.origin = origin;
    newArc.namespaceDepth = namespaceDepth;
    newArc.siblingNumAtOrigin = arcSiblingNum;

    // Create the new node.
    PcpNodeRef newNode;
    PcpErrorBasePtr newNodeError;
    if (!includeAncestralOpinions) {
        // No ancestral opinions.  Just add the single new site.
        newNode = parent.InsertChild(site, newArc, &newNodeError);
        if (newNode) {
            newNode.SetInert(!directNodeShouldContributeSpecs);

            // Compose the existence of primSpecs and update the HasSpecs field 
            // accordingly.
            newNode.SetHasSpecs(PcpComposeSiteHasPrimSpecs(newNode));

            if (!newNode.IsInert() && newNode.HasSpecs()) {
                if (!indexer->inputs.usd) {
                    // Determine whether opinions from this site can be accessed
                    // from other sites in the graph.
                    newNode.SetPermission(
                        PcpComposeSitePermission(site.layerStack, site.path));

                    // Determine whether this node has any symmetry information.
                    newNode.SetHasSymmetry(
                        PcpComposeSiteHasSymmetry(site.layerStack, site.path));
                }
            }

            PCP_INDEXING_UPDATE(
                indexer, newNode, 
                "Added new node for site %s to graph",
                TfStringify(site).c_str());
        }

    } else {
        // Ancestral opinions are those above the source site in namespace.
        // We only need to account for them if the site is not a root prim
        // (since root prims have no ancestors with scene description, only
        // the pseudo-root).
        //
        // Account for ancestral opinions by building out the graph for
        // that site and incorporating its root node as the new child.
        PCP_INDEXING_MSG(
            indexer, parent, 
            "Need to build index for %s source at %s to "
            "pick up ancestral opinions",
            TfEnum::GetDisplayName(arcType).c_str(),
            Pcp_FormatSite(site).c_str());

        // We don't want to evaluate implied specializes immediately when
        // building the index for this source site. Instead, we'll add
        // tasks to do this after we have merged the source index into 
        // the final index. This allows any specializes arcs in the source
        // index to be propagated to the root of the graph for the correct
        // strength ordering.
        const bool evaluateImpliedSpecializes = false;

        // We don't want to evaluate variants immediately when building
        // the index for the source site. This is because Pcp_BuildPrimIndex,
        // won't know anything about opinions outside of the source site,
        // which could cause stronger variant selections to be ignored.
        // (For instance, if a referencing layer stack had a stronger
        // opinion for the selection than what was authored at the source.
        //
        // So, tell Pcp_BuildPrimIndex to skip variants; we'll add tasks
        // for that after inserting the source index into our index. That
        // way, the variant evaluation process will have enough context
        // to decide what the strongest variant selection is.
        const bool evaluateVariants = false;

        // Provide a linkage across recursive calls to the indexer.
        PcpPrimIndex_StackFrame
            frame(site, parent, &newArc, indexer->previousFrame,
                  indexer->GetOriginatingIndex(), skipDuplicateNodes);

        PcpPrimIndexOutputs childOutputs;
        Pcp_BuildPrimIndex( site,
                            indexer->rootSite,
                            indexer->ancestorRecursionDepth,
                            evaluateImpliedSpecializes,
                            evaluateVariants,
                            directNodeShouldContributeSpecs,
                            &frame,
                            indexer->inputs,
                            &childOutputs );

        // Combine the child output with our current output.
        newNode = indexer->outputs->Append(std::move(childOutputs), newArc,
                                           &newNodeError);
        if (newNode) {
            // Record any culled nodes from this subtree that introduced
            // ancestral dependencies. These nodes may be removed from the prim
            // index when Finalize() is called, so they must be saved separately
            // for later use. Only do this in the top-level call to _AddArc
            // to avoid running over the same subtree multiple times if there
            // were multiple levels of recursive prim indexing.
            if (!indexer->previousFrame) {
                _AddCulledDependencies(
                    newNode, &indexer->outputs->culledDependencies);
            }

            PCP_INDEXING_UPDATE(
                indexer, newNode, 
                "Added subtree for site %s to graph",
                TfStringify(site).c_str());
        }
    }

    // Handle errors.
    if (newNodeError) {
        // Provide rootSite as context.
        newNodeError->rootSite = indexer->rootSite;
        indexer->RecordError(newNodeError);
    }
    if (!newNode) {
        TF_VERIFY(newNodeError, "Failed to create a node, but did not "
                  "specify the error.");
        return PcpNodeRef();
    }

    // If culling is enabled, check whether the entire subtree rooted
    // at the new node can be culled. This doesn't have to recurse down
    // the new subtree; instead, it just needs to check the new node only. 
    // This is because computing the source prim index above will have culled
    // everything it can *except* for the subtree's root node. 
    if (indexer->inputs.cull) {
        if (_NodeCanBeCulled(newNode, indexer->rootSite)) {
            newNode.SetCulled(true);
        }
        else {
            // Ancestor nodes that were previously marked as culled must
            // be updated because they now have a subtree that isn't culled.
            // This can happen during the propagation of implied inherits from
            // a class hierarchy. For instance, consider the graph:
            //
            //   root.menva       ref.menva
            //   Model_1 (ref)--> Model (inh)--> ModelClass (inh)--> CharClass.
            // 
            // Let's say there were specs for /CharClass but NOT for /ModelClass
            // in the root layer stack. In that case, propagating ModelClass to
            // the root layer stack would result in a culled node. However, when
            // we then propagate CharClass, we wind up with an unculled node 
            // beneath a culled node, which violates the culling invariant. So,
            // we would need to fix up /ModelClass to indicate that it can no
            // longer be culled.
            for (PcpNodeRef p = parent; 
                 p && p.IsCulled(); p = p.GetParentNode()) {
                p.SetCulled(false);
            }
        }
    }

    // Enqueue tasks to evaluate the new nodes.
    //
    // If we evaluated ancestral opinions, it it means the nested
    // call to Pcp_BuildPrimIndex() has already evaluated refs, payloads,
    // and inherits on this subgraph, so we can skip those tasks in this case 
    // too.
    skipTasksForExpressedArcs = 
        skipTasksForExpressedArcs || includeAncestralOpinions;
    indexer->AddTasksForNode(
        newNode, skipTasksForExpressedArcs, 
        skipImpliedSpecializesCompletedNodes);

    // If the arc targets a site that is itself private, issue an error.
    if (newNode.GetPermission() == SdfPermissionPrivate) {
        PcpErrorArcPermissionDeniedPtr err = PcpErrorArcPermissionDenied::New();
        err->rootSite = PcpSite(parent.GetRootNode().GetSite());
        err->site = PcpSite(parent.GetSite());
        err->privateSite = PcpSite(newNode.GetSite());
        err->arcType = arcType;
        indexer->RecordError(err);

        // Mark the new child subtree as inert so that it does not
        // contribute specs, but keep the node(s) to track the
        // dependencies in order to support processing later changes
        // that relax the permissions.
        //
        // Note, this is a complementary form of permissions enforcement
        // to that done by _EnforcePermissions().  That function enforces
        // the constraint that once something is made private via an
        // ancestral arc, overrides are prohibited.  This enforces the
        // equivalent constraint on direct arcs: you cannot employ an
        // arc directly to a private site.
        _InertSubtree(newNode);
    }

    // If the new node's path is the pseudo root, this is a special dependency
    // placeholder for unresolved default-target references/payloads.
    // Mark the node inert to node contribute opinions, but retain the
    // nodes to represent the dependency.
    if (newNode.GetPath() == SdfPath::AbsoluteRootPath()) {
        _InertSubtree(newNode);
    }        

    return newNode;
}

static PcpNodeRef
_AddArc(
    const PcpArcType arcType,
    PcpNodeRef parent,
    PcpNodeRef origin,
    const PcpLayerStackSite & site,
    PcpMapExpression mapExpr,
    int arcSiblingNum,
    bool directNodeShouldContributeSpecs,
    bool includeAncestralOpinions,
    bool skipDuplicateNodes,
    Pcp_PrimIndexer *indexer )
{
    // Strip variant selections when determining namespace depth.
    // Variant selections are (unfortunately) represented as path
    // components, but do not represent additional levels of namespace,
    // just alternate storage locations for data.
    const int namespaceDepth =
        PcpNode_GetNonVariantPathElementCount( parent.GetPath() );

    return _AddArc(
        arcType, parent, origin, site, mapExpr, 
        arcSiblingNum, namespaceDepth,
        directNodeShouldContributeSpecs,
        includeAncestralOpinions,
        skipDuplicateNodes,
        /* skipImpliedSpecializes = */ false,
        /* skipTasksForExpressedArcs = */ false,
        indexer);
}

////////////////////////////////////////////////////////////////////////
// References

static SdfPath
_GetDefaultPrimPath(SdfLayerHandle const &layer)
{
    TfToken target = layer->GetDefaultPrim();
    return SdfPath::IsValidIdentifier(target) ?
        SdfPath::AbsoluteRootPath().AppendChild(target) : SdfPath();
}

// Declare helper function for creating PcpDynamicFileFormatContext, 
// implemented in dynamicFileFormatContext.cpp
PcpDynamicFileFormatContext
Pcp_CreateDynamicFileFormatContext(
    const PcpNodeRef &, PcpPrimIndex_StackFrame *, TfToken::Set *, TfToken::Set *);

// Generates dynamic file format arguments for a payload's asset path if the 
// asset's file format supports it.
static void
_ComposeFieldsForFileFormatArguments(const PcpNodeRef &node,
                                     const Pcp_PrimIndexer &indexer,
                                     const SdfPayload &payload,
                                     SdfLayer::FileFormatArguments *args)
{
    SdfFileFormatConstPtr fileFormat = SdfFileFormat::FindByExtension(
        SdfFileFormat::GetFileExtension(payload.GetAssetPath()),
        indexer.inputs.fileFormatTarget);
    if (!fileFormat) {
        return;
    }
    if (const PcpDynamicFileFormatInterface *dynamicFileFormat = 
            dynamic_cast<const PcpDynamicFileFormatInterface *>(
                get_pointer(fileFormat))) {
        // Create the context for composing the prim fields from the current 
        // state of the index. This context will also populate a list of the
        // fields that it composed for dependency tracking
        TfToken::Set composedFieldNames;
        TfToken::Set composedAttributeNames;
        PcpDynamicFileFormatContext context = Pcp_CreateDynamicFileFormatContext(
            node, indexer.previousFrame, &composedFieldNames, &composedAttributeNames);
        // Ask the file format to generate dynamic file format arguments for 
        // the asset in this context.
        VtValue dependencyContextData;
        dynamicFileFormat->ComposeFieldsForFileFormatArguments(
            payload.GetAssetPath(), context, args, &dependencyContextData);

        // Add this dependency context to dynamic file format dependency object.
        indexer.outputs->dynamicFileFormatDependency.AddDependencyContext(
            dynamicFileFormat, std::move(dependencyContextData), 
            std::move(composedFieldNames), std::move(composedAttributeNames));
    }
}

static bool
_ComposeFieldsForFileFormatArguments(const PcpNodeRef &node,
                                     const Pcp_PrimIndexer &indexer,
                                     const SdfReference &ref,
                                     SdfLayer::FileFormatArguments *args)
{
    // References don't support dynamic file format arguments.
    return false;
}

// Reference and payload arcs are composed in essentially the same way. 
template <class RefOrPayloadType, PcpArcType ARC_TYPE>
static void
_EvalRefOrPayloadArcs(PcpNodeRef node, 
                      Pcp_PrimIndexer *indexer,
                      const std::vector<RefOrPayloadType> &arcs,
                      const PcpSourceArcInfoVector &infoVec)
{
    // This loop will be adding arcs and therefore can grow the node
    // storage vector, so we need to avoid holding any references
    // into that storage outside the loop.
    for (size_t arcNum=0; arcNum < arcs.size(); ++arcNum) {
        const RefOrPayloadType & refOrPayload = arcs[arcNum];
        const PcpSourceArcInfo& info = infoVec[arcNum];
        const SdfLayerHandle & srcLayer = info.layer;
        SdfLayerOffset layerOffset = refOrPayload.GetLayerOffset();

        PCP_INDEXING_MSG(
            indexer, node, "Found %s to @%s@<%s>", 
            ARC_TYPE == PcpArcTypePayload ? "payload" : "reference",
            info.authoredAssetPath.c_str(), 
            refOrPayload.GetPrimPath().GetText());

        bool fail = false;

        // Verify that the reference or payload targets either the default 
        // reference/payload target, or a prim with an absolute path.
        if (!refOrPayload.GetPrimPath().IsEmpty() &&
            !(refOrPayload.GetPrimPath().IsAbsolutePath() && 
              refOrPayload.GetPrimPath().IsPrimPath() &&
              !refOrPayload.GetPrimPath().ContainsPrimVariantSelection())) {
            PcpErrorInvalidPrimPathPtr err = PcpErrorInvalidPrimPath::New();
            err->rootSite = PcpSite(node.GetRootNode().GetSite());
            err->site = PcpSite(node.GetSite());
            err->primPath = refOrPayload.GetPrimPath();
            err->sourceLayer = srcLayer;
            err->arcType = ARC_TYPE;
            indexer->RecordError(err);
            fail = true;
        }

        // Validate layer offset in original reference or payload.
        if (!layerOffset.IsValid() ||
            !layerOffset.GetInverse().IsValid()) {
            PcpErrorInvalidReferenceOffsetPtr err =
                PcpErrorInvalidReferenceOffset::New();
            err->rootSite = PcpSite(node.GetRootNode().GetSite());
            err->sourceLayer = srcLayer;
            err->sourcePath = node.GetPath();
            err->assetPath = info.authoredAssetPath;
            err->targetPath = refOrPayload.GetPrimPath();
            err->offset = layerOffset;
            err->arcType = ARC_TYPE;
            indexer->RecordError(err);

            // Don't set fail, just reset the offset.
            layerOffset = SdfLayerOffset();
        } else {
            // Apply the layer stack offset for the introducing layer to the 
            // reference or payload's layer offset.
            layerOffset = info.layerStackOffset * layerOffset;
        }

        // Go no further if we've found any problems.
        if (fail) {
            continue;
        }

        // Compute the reference or payload layer stack
        // See Pcp_NeedToRecomputeDueToAssetPathChange
        SdfLayerRefPtr layer;
        PcpLayerStackRefPtr layerStack;

        const bool isInternal = refOrPayload.GetAssetPath().empty();
        if (isInternal) {
            layer = node.GetLayerStack()->GetIdentifier().rootLayer;
            layerStack = node.GetLayerStack();
        }
        else {
            std::string canonicalMutedLayerId;
            if (indexer->inputs.cache->IsLayerMuted(
                    srcLayer, info.authoredAssetPath, &canonicalMutedLayerId)) {
                PcpErrorMutedAssetPathPtr err = PcpErrorMutedAssetPath::New();
                err->rootSite = PcpSite(node.GetRootNode().GetSite());
                err->site = PcpSite(node.GetSite());
                err->targetPath = refOrPayload.GetPrimPath();
                err->assetPath = info.authoredAssetPath;
                err->resolvedAssetPath = canonicalMutedLayerId;
                err->arcType = ARC_TYPE;
                err->sourceLayer = srcLayer;
                indexer->RecordError(err);
                continue;
            }

            SdfLayer::FileFormatArguments args;
            // Compose any file format arguments that may come from the asset
            // file format if it's dynamic.
            _ComposeFieldsForFileFormatArguments(
                node, *indexer, refOrPayload, &args);
            Pcp_GetArgumentsForFileFormatTarget(
                refOrPayload.GetAssetPath(), 
                indexer->inputs.fileFormatTarget, &args);

            TfErrorMark m;

            // Relative asset paths will already have been anchored to their 
            // source layers in PcpComposeSiteReferences, so we can just call
            // SdfLayer::FindOrOpen instead of FindOrOpenRelativeToLayer.
            layer = SdfLayer::FindOrOpen(refOrPayload.GetAssetPath(), args);

            if (!layer) {
                PcpErrorInvalidAssetPathPtr err = 
                    PcpErrorInvalidAssetPath::New();
                err->rootSite = PcpSite(node.GetRootNode().GetSite());
                err->site = PcpSite(node.GetSite());
                err->targetPath = refOrPayload.GetPrimPath();
                err->assetPath = info.authoredAssetPath;
                err->resolvedAssetPath = refOrPayload.GetAssetPath();
                err->arcType = ARC_TYPE;
                err->sourceLayer = srcLayer;
                if (!m.IsClean()) {
                    vector<string> commentary;
                    for (auto const &err: m) {
                        commentary.push_back(err.GetCommentary());
                    }
                    m.Clear();
                    err->messages = TfStringJoin(commentary.begin(),
                                                 commentary.end(), "; ");
                }
                indexer->RecordError(err);
                continue;
            }

            const ArResolverContext& pathResolverContext =
                node.GetLayerStack()->GetIdentifier().pathResolverContext;
            
            // We want to use the expression variables composed up to node's
            // layer stack to compose over the variables in the referenced layer
            // stack.
            // 
            // Note that we specify the source of this node's layer stack's
            // PcpExpressionVariables object as the "expression variable
            // override source" in the referenced layer stack. This allows us to
            // share layer stacks across prim indexes when expression variables
            // are sparsely authored (which is the expected use case).
            //
            // For example, consider two prim indexes /A and /B:
            //
            //                    ref              ref 
            // /A: @root.sdf@</A> ---> @a.sdf@</A> ---> @model.sdf@</Model>
            //
            //                    ref              ref 
            // /B: @root.sdf@</B> ---> @b.sdf@</B> ---> @model.sdf@</Model>
            //
            // If expression variables are only authored on root.sdf, the
            // override source for all downstream layer stacks will be
            // root.sdf. This means the model.sdf layer stack in /A and /B are
            // the same object.
            // 
            // If we instead used the layer stack identifier of this node as the
            // expression variable override source, the identifiers for the
            // model.sdf layer stack in /A and /B would differ, even though they
            // would be equivalent since they'd have the same layers and
            // composed expression variables.
            //
            // The approach we take maximizes sharing but requires that change
            // processing triggers resyncs when an override source changes.  For
            // example, if expression variables are additionally authored on
            // a.sdf, change processing needs to determine that that layer stack
            // now provides the variable overrides instead of root.sdf, which
            // means that /A needs to be resynced so that the reference to
            // model.sdf is recomputed. At that point, the model.sdf layer
            // stacks in /A and /B are no longer equivalent and become two
            // different objects since they have different composed expression
            // variables. If the variables in a.sdf were then removed, change
            // processing should again resync /A, at which point the model.sdf
            // layer stacks in /A and /B would be the same object once more.
            const PcpLayerStackIdentifier layerStackIdentifier(
                layer, SdfLayerHandle(), pathResolverContext,
                node.GetLayerStack()->GetExpressionVariables().GetSource());

            layerStack = indexer->inputs.cache->ComputeLayerStack( 
                layerStackIdentifier, &indexer->outputs->allErrors);

            if (!PcpIsTimeScalingForLayerTimeCodesPerSecondDisabled()) {
                // If the referenced or payloaded layer has a different TCPS
                // than the source layer that introduces it, we apply the time
                // scale between these TCPS values to the layer offset.
                // Note that if the introducing layer is a layer stack sublayer,
                // any TCPS scaling from the layer stack will already have been
                // applied to the layer offset for the reference/payload.
                const double srcTimeCodesPerSecond = 
                    srcLayer->GetTimeCodesPerSecond();
                const double destTimeCodesPerSecond =
                    layerStack->GetTimeCodesPerSecond();
                if (srcTimeCodesPerSecond != destTimeCodesPerSecond) {
                    layerOffset.SetScale(layerOffset.GetScale() * 
                        srcTimeCodesPerSecond / destTimeCodesPerSecond);
                }
            }
        }

        bool directNodeShouldContributeSpecs = true;

        // Determine the prim path.  This is either the one explicitly 
        // specified in the SdfReference or SdfPayload, or if that's empty, then
        // the one specified by DefaultPrim in the referenced layer.
        SdfPath defaultPrimPath;
        if (refOrPayload.GetPrimPath().IsEmpty()) {
            // Check the layer for a defaultPrim, and use
            // that if present.
            defaultPrimPath = _GetDefaultPrimPath(layer);
            if (defaultPrimPath.IsEmpty()) {
                PcpErrorUnresolvedPrimPathPtr err =
                    PcpErrorUnresolvedPrimPath::New();
                err->rootSite = PcpSite(node.GetRootNode().GetSite());
                err->site = PcpSite(node.GetSite());
                // Use a relative path with the field key for a hint.
                err->targetLayer = layer;
                err->unresolvedPath = SdfPath::ReflexiveRelativePath().
                    AppendChild(SdfFieldKeys->DefaultPrim);
                err->sourceLayer = srcLayer;
                err->arcType = ARC_TYPE;
                indexer->RecordError(err);

                // Set the prim path to the pseudo-root path.  We'll still add 
                // an arc to it as a special dependency placeholder, so we
                // correctly invalidate if/when the default target metadata gets
                // authored in the target layer.
                defaultPrimPath = SdfPath::AbsoluteRootPath();
                directNodeShouldContributeSpecs = false;
            }
        }

        // Final prim path to use.
        SdfPath const &primPath = defaultPrimPath.IsEmpty() ? 
            refOrPayload.GetPrimPath() : defaultPrimPath;

        // The mapping for a reference (or payload) arc makes the source
        // and target map to each other.  Paths outside these will not map,
        // except for the case of internal references.
        PcpMapExpression mapExpr = 
            _CreateMapExpressionForArc(
                /* source */ primPath, /* targetNode */ node, 
                indexer->inputs, layerOffset);
        if (isInternal) {
            // Internal references maintain full namespace visibility
            // outside the source & target.
            mapExpr = mapExpr.AddRootIdentity();
        }

        // Only need to include ancestral opinions if the prim path is
        // not a root prim.
        const bool includeAncestralOpinions = !primPath.IsRootPrimPath();

        const PcpNodeRef newNode = _AddArc( ARC_TYPE,
                 /* parent = */ node,
                 /* origin = */ node,
                 PcpLayerStackSite( layerStack, primPath ),
                 mapExpr,
                 /* arcSiblingNum = */ arcNum,
                 directNodeShouldContributeSpecs,
                 includeAncestralOpinions,
                 /* skipDuplicateNodes = */ false,
                 indexer );
                 
        // Reference and payload arcs must target a prim that exists in the 
        // referenced layer stack. If there isn't, we report an error. Note that
        // the node representing this arc was already added to the graph for
        // dependency tracking purposes.
        if (newNode && !_PrimSpecExistsUnderNode(newNode, indexer)) {
            PcpErrorUnresolvedPrimPathPtr err = PcpErrorUnresolvedPrimPath::New();
            err->rootSite = PcpSite(node.GetRootNode().GetSite());
            err->site = PcpSite(node.GetSite());
            err->targetLayer = layer;
            err->unresolvedPath = newNode.GetSite().path;
            err->sourceLayer = srcLayer;
            err->arcType = ARC_TYPE;
            indexer->RecordError(err);
        }
    }
}

static void
_EvalNodeReferences(
    PcpPrimIndex *index, 
    PcpNodeRef node, 
    Pcp_PrimIndexer *indexer)    
{
    PCP_INDEXING_PHASE(
        indexer, node,
        "Evaluating references at %s", 
        Pcp_FormatSite(node.GetSite()).c_str());

    if (!node.CanContributeSpecs())
        return;

    // Compose value for local references.
    SdfReferenceVector refArcs;
    PcpSourceArcInfoVector refInfo;
    std::unordered_set<std::string> exprVarDependencies;
    PcpErrorVector errors;
    PcpComposeSiteReferences(
        node, &refArcs, &refInfo, &exprVarDependencies, &errors);

    if (!exprVarDependencies.empty()) {
        indexer->outputs->expressionVariablesDependency.AddDependencies(
            node.GetLayerStack(), std::move(exprVarDependencies));
    }

    if (!errors.empty()) {
        for (const PcpErrorBasePtr& err : errors) {
            indexer->RecordError(err);
        }
    }

    // Add each reference arc.
    _EvalRefOrPayloadArcs<SdfReference, PcpArcTypeReference>(
        node, indexer, refArcs, refInfo);
}

////////////////////////////////////////////////////////////////////////
// Payload

static void
_EvalNodePayloads(
    PcpPrimIndex *index, 
    const PcpNodeRef& node, 
    Pcp_PrimIndexer *indexer)
{
    PCP_INDEXING_PHASE(
        indexer, node, "Evaluating payload for %s", 
        Pcp_FormatSite(node.GetSite()).c_str());

    if (!node.CanContributeSpecs()) {
        return;
    }

    // Compose value for local payloads.
    SdfPayloadVector payloadArcs;
    PcpSourceArcInfoVector payloadInfo;
    std::unordered_set<std::string> exprVarDependencies;
    PcpErrorVector errors;
    PcpComposeSitePayloads(
        node, &payloadArcs, &payloadInfo, &exprVarDependencies, &errors);

    if (!exprVarDependencies.empty()) {
        indexer->outputs->expressionVariablesDependency.AddDependencies(
            node.GetLayerStack(), std::move(exprVarDependencies));
    }

    if (!errors.empty()) {
        for (const PcpErrorBasePtr& err : errors) {
            indexer->RecordError(err);
        }
    }

    if (payloadArcs.empty()) {
        return;
    }

    PCP_INDEXING_MSG(
        indexer, node, "Found payload for node %s", node.GetPath().GetText());

    // Mark that this prim index contains a payload.
    // However, only process the payload if it's been requested.
    index->GetGraph()->SetHasPayloads(true);

    // First thing we check is if this payload arc is being composed because it
    // will be an ancestral payload arc for a subgraph being being built for a 
    // subroot reference or payload.
    // The prim index stack frame tells us whether we're building a subgraph 
    // for a reference or payload and we can compare the stack frame
    // arc's requested site against the site we're building to check if this 
    // we're building an ancestor of the actual target site.
    const bool isAncestralPayloadOfSubrootReference =
        indexer->previousFrame &&
        (indexer->previousFrame->arcToParent->type == PcpArcTypePayload ||
         indexer->previousFrame->arcToParent->type == PcpArcTypeReference) &&
        index->GetRootNode().GetSite() != indexer->previousFrame->requestedSite;

    // If this payload arc is an ancestral arc of the target of a subroot 
    // reference/payload, then we always compose this payload. This is because 
    // this ancestral prim index is not necessarily a one that would be 
    // present on its own in the PcpCache and there may be no explicit way to 
    // include it. So our policy is to always include the payload in this 
    // context.
    //
    // Example:
    // Prim </A> in layer1 has a payload to another prim </B> in layer2
    // Prim </B> has a child prim </B/C>
    // Prim </B/C> has a payload to another prim </D> in layer3 
    // Prim </E> on the root layer has a subroot reference to </A/C> in layer1
    //
    // When composing the reference arc for prim </E> we build a prim index for
    // </A/C> which builds the ancestral prim index for </A> first. In order for
    // </A/C> to exist, the ancestral payload for </A> to </B> must be included.
    // Because it will be an ancestral arc of a subroot reference subgraph, the
    // payload will always be included.
    // 
    // However when we continue to compose </A/C> -> </B/C> and we encounter the
    // payload to </D>, this payload is NOT automatically included as it is a 
    // direct arc from the subroot reference arc and can be included or excluded
    // via including/excluding </E>
    if (!isAncestralPayloadOfSubrootReference) {
        const PcpPrimIndexInputs::PayloadSet* includedPayloads = 
            indexer->inputs.includedPayloads;

        // If includedPayloads is nullptr, we never include payloads.  Otherwise if
        // it does not have this path, we invoke the predicate.  If the predicate
        // returns true we set the output bit includedDiscoveredPayload and we
        // compose it.
        if (!includedPayloads) {
            PCP_INDEXING_MSG(indexer, node, "Payload was not included, skipping");
            return;
        }
        SdfPath const &path = indexer->rootSite.path;

        // If there's a payload predicate, we invoke that to decide whether or not
        // this payload should be included.
        bool composePayload = false;
        if (auto const &pred = indexer->inputs.includePayloadPredicate) {
            composePayload = pred(path);
            indexer->outputs->payloadState = composePayload ?
                PcpPrimIndexOutputs::IncludedByPredicate : 
                PcpPrimIndexOutputs::ExcludedByPredicate;
        }
        else {
            tbb::spin_rw_mutex::scoped_lock lock;
            auto *mutex = indexer->inputs.includedPayloadsMutex;
            if (mutex) { lock.acquire(*mutex, /*write=*/false); }
            composePayload = includedPayloads->count(path);
            indexer->outputs->payloadState = composePayload ?
                PcpPrimIndexOutputs::IncludedByIncludeSet : 
                PcpPrimIndexOutputs::ExcludedByIncludeSet;
        }
         
        if (!composePayload) {
            PCP_INDEXING_MSG(indexer, node,
                             "Payload <%s> was not included, skipping",
                             path.GetText());
            return;
        }
    }

    _EvalRefOrPayloadArcs<SdfPayload, PcpArcTypePayload>(
        node, indexer, payloadArcs, payloadInfo);
}

////////////////////////////////////////////////////////////////////////
// Relocations

static void
_ElideSubtree(
    const Pcp_PrimIndexer& indexer,
    PcpNodeRef node)
{
    if (indexer.inputs.cull) {
        node.SetCulled(true);
    }
    else {
        node.SetInert(true);
    }

    TF_FOR_ALL(child, Pcp_GetChildrenRange(node)) {
        _ElideSubtree(indexer, *child);
    }
}

static void
_ElideRelocatedSubtrees(
    const Pcp_PrimIndexer& indexer,
    PcpNodeRef node)
{
    TF_FOR_ALL(it, Pcp_GetChildrenRange(node)) {
        const PcpNodeRef& childNode = *it;

        // We can cut off the traversal if this is a relocate node, since we
        // would have done this work when the node was originally added to
        // the graph.
        if (childNode.GetArcType() == PcpArcTypeRelocate) {
            continue;
        }

        // Elide the subtree rooted at this node if there's a relocate 
        // statement that would move its opinions to a different prim.
        if (childNode.CanContributeSpecs()) {
            const PcpLayerStackRefPtr& layerStack = childNode.GetLayerStack();
            const SdfRelocatesMap& relocatesSrcToTarget = 
                layerStack->GetIncrementalRelocatesSourceToTarget();
            if (relocatesSrcToTarget.find(childNode.GetPath()) !=
                relocatesSrcToTarget.end()) {
                _ElideSubtree(indexer, childNode);
                continue;
            }
        }

        _ElideRelocatedSubtrees(indexer, childNode);
    }
}

// Account for relocations that affect existing nodes in the graph.
// This method is how we handle the effects of relocations, as we walk
// down namespace.  For each prim, we start by using the parent's graph,
// then applying relocations here.  For every relocation, we introduce a
// new graph node for the relocation source, and recursively populate that
// source via _AddArc().
static void
_EvalNodeRelocations(
    PcpPrimIndex *index, 
    const PcpNodeRef &node, 
    Pcp_PrimIndexer *indexer )
{
    PCP_INDEXING_PHASE(
        indexer, node, 
        "Evaluating relocations under %s", 
        Pcp_FormatSite(node.GetSite()).c_str());

    // Unlike other tasks, we skip processing if this node can't contribute 
    // specs, but only if this node was introduced at this level at namespace.
    // This additional check is needed because a descendant node might not
    // have any specs and thus be marked as culled, but still have relocates
    // that affect that node.
    if (!node.CanContributeSpecs() && node.GetDepthBelowIntroduction() == 0) {
        return;
    }

    // Determine if this node was relocated, and from what source path.
    //
    // We need to use the incremental relocates map instead of the 
    // fully-combined map to ensure we examine all sources of opinions
    // in the case where there are multiple relocations nested in different
    // levels of namespace that affect the same prim. The fully-combined 
    // map collapses these relocations into a single entry, which would
    // cause us to skip looking at any intermediate sites.
    const SdfRelocatesMap & relocatesTargetToSource = 
        node.GetLayerStack()->GetIncrementalRelocatesTargetToSource();
    SdfRelocatesMap::const_iterator i =
        relocatesTargetToSource.find(node.GetPath());
    if (i == relocatesTargetToSource.end()) {
        // This node was not relocated.
        return;
    }

    // This node was relocated.  Add a relocation arc back to the source.
    const SdfPath & relocSource = i->second;
    const SdfPath & relocTarget = i->first;

    PCP_INDEXING_MSG(
        indexer, node, "<%s> was relocated from source <%s>",
        relocTarget.GetText(), relocSource.GetText());

    // Determine how the opinions from the relocation source will compose
    // with opinions from ancestral arcs on the relocation target. 
    // For certain nodes, we recursively mark their contributes as 
    // shouldContributeSpecs=false to indicate that they should not 
    // contribute opinions.
    //
    // TODO: We do not remove them entirely, because the
    // nodes there may be used as the 'origin' of an implied inherit
    // for purposes of determining relative strength. Perhaps we can
    // remove all nodes that aren't used as an origin?
    //
    // TODO: We may also want to use these nodes as a basis
    // to check for an issue errors about opinions at relocation
    // sources across references. Today, Csd silently ignores these,
    // but it seems like we should check for opinion collisions,
    // and either report the current relocation arc as invalid, or
    // choose between the opinions somehow.
    //
    TF_FOR_ALL(childIt, Pcp_GetChildrenRange(node)) {
        const PcpNodeRef& child = *childIt;
        switch (child.GetArcType()) {
            // Ancestral arcs of these types should contribute opinions.
        case PcpArcTypeVariant:
            // Variants are allowed to provide overrides of relocated prims. 
            continue;
        case PcpArcTypeRoot:
        case PcpNumArcTypes:
            // Cases we should never encounter.
            TF_VERIFY(false, "Unexpected child node encountered");
            continue;            

            // Nodes of these types should NOT contribute opinions.
        case PcpArcTypeRelocate:
            // Ancestral relocation arcs are superceded by this relocation,
            // which is 'closer' to the actual prim we're trying to index.
            // So, contributions from the ancestral subtree should be ignored
            // in favor of the ones from the relocation arc we're about to
            // add. See TrickyMultipleRelocations for an example.
        case PcpArcTypeReference:
        case PcpArcTypePayload:
        case PcpArcTypeInherit:
        case PcpArcTypeSpecialize:
            // Ancestral opinions at a relocation target across a reference
            // or inherit are silently ignored. See TrickyRelocationSquatter
            // for an example.
            //
            // XXX: Since inherits are stronger than relocations, I wonder
            //      if you could make the argument that classes should be
            //      able to override relocated prims, just like variants.
            break;
        };

        _ElideSubtree(*indexer, child);

        PCP_INDEXING_UPDATE(
            indexer, child, 
            "Elided subtree that will be superceded by relocation source <%s>",
            relocSource.GetText());
    }

    // The mapping for a relocation source node is identity.
    //
    // The reason is that relocation mappings are applied across the
    // specific arcs whose target path is affected by relocations.
    // In this approach, relocates source nodes do not need to apply
    // relocation mappings since they would be redundant.
    //
    // Instead of representing the namespace mappings for relocations,
    // Relocation source nodes are primarily placeholders used to
    // incorporate the ancestral arcs from the relocation sources (spooky
    // ancestors).  Using actual nodes for this lets us easily
    // incorporate spooky ancestral opinions, spooky implied inherits
    // etc. without needed special accommodation.  However, it does
    // have some other ramifications; see XXX:RelocatesSourceNodes.
    //
    // XXX: It could be that a better design would be to only use
    // Relocates Source nodes during the temporary recursive indexing
    // of relocation sources, and then immediately transfer all of its
    // children to the relocates parent directly. To do this we would
    // need to decide how to resolve the relative arc strength of the
    // relocation target vs. source child nodes.
    const PcpMapExpression identityMapExpr = PcpMapExpression::Identity();

    // A prim can only be relocated from a single place -- our
    // expression of relocates as a map only allows for a single
    // entry -- so the arc number is always zero.
    const int arcSiblingNum = 0;

    PcpNodeRef newNode =
        _AddArc( PcpArcTypeRelocate,
                 /* parent = */ node,
                 /* origin = */ node,
                 PcpLayerStackSite( node.GetLayerStack(), relocSource ),
                 identityMapExpr,
                 arcSiblingNum,
                 /* The direct site of a relocation source is not allowed to
                    contribute opinions.  However, note that it usually
                    has node-children that do contribute opinions via
                    ancestral arcs. */
                 /* directNodeShouldContributeSpecs = */ false,
                 /* includeAncestralOpinions = */ true,
                 /* skipDuplicateNodes = */ false,
                 indexer );

    if (newNode) {
        // Check for the existence of opinions at the relocation
        // source, and issue errors for any that are found.
        //
        // XXX: It's a little misleading to do this only here, as this won't
        //      report relocation source errors for namespace children beneath
        //      this site. (See the error message for /Group/Model_Renamed/B
        //      in ErrorArcCycle for example; it cites invalid opinions at
        //      /Group/Model, but doesn't cite invalid opinions at 
        //      /Group/Model/B.
        SdfSiteVector sites;
        PcpComposeSitePrimSites(newNode, &sites);
        TF_FOR_ALL(site, sites) {
            PcpErrorOpinionAtRelocationSourcePtr err =
                PcpErrorOpinionAtRelocationSource::New();
            err->rootSite = PcpSite(node.GetRootNode().GetSite());
            err->layer = site->layer;
            err->path  = site->path;
            indexer->RecordError(err);
        }

        // Scan the added subtree to see it contains any opinions that would
        // be moved to a different prim by other relocate statements. If so,
        // we need to elide those opinions, or else we'll wind up with multiple
        // prims with opinions from the same site. 
        //
        // See RelocatePrimsWithSameName test case for an example of this.
        _ElideRelocatedSubtrees(*indexer, newNode);
    }
}

static void
_EvalImpliedRelocations(
    PcpPrimIndex *index, 
    const PcpNodeRef &node, 
    Pcp_PrimIndexer *indexer )
{
    if (node.GetArcType() != PcpArcTypeRelocate || node.IsDueToAncestor()) {
        return;
    }

    PCP_INDEXING_PHASE(
        indexer, node,
        "Evaluating relocations implied by %s", 
        Pcp_FormatSite(node.GetSite()).c_str());

    if (const PcpNodeRef parent = node.GetParentNode()) {
        if (const PcpNodeRef gp = parent.GetParentNode()) {

            // Determine the path of the relocation source prim in the parent's
            // layer stack. Note that this mapping may fail in some cases. For
            // example, if prim /A/B was relocated to /A/C, and then in another
            // layer stack prim /D sub-root referenced /A/C, there would be no
            // corresponding prim for the source /A/B in that layer stack.
            // See SubrootReferenceAndRelocates for a concrete example.
            const SdfPath gpRelocSource =
                parent.GetMapToParent().MapSourceToTarget(node.GetPath());
            if (gpRelocSource.IsEmpty()) {
                PCP_INDEXING_PHASE(
                    indexer, node,
                    "No implied site for relocation source -- skipping");
                return;
            }

            PCP_INDEXING_PHASE(
                indexer, node,
                "Propagating relocate from %s to %s", 
                Pcp_FormatSite(node.GetSite()).c_str(),
                gpRelocSource.GetText());

            // Check if this has already been propagated.
            TF_FOR_ALL(gpChildIt, Pcp_GetChildrenRange(gp)) {
                const PcpNodeRef& gpChild = *gpChildIt;
                if (gpChild.GetPath() == gpRelocSource &&
                    gpChild.GetArcType() == PcpArcTypeRelocate) {
                    PCP_INDEXING_PHASE(
                        indexer, node,
                        "Relocate already exists -- skipping");
                    return;
                }
            }

            _AddArc( PcpArcTypeRelocate,
                     /* parent = */ gp,
                     /* origin = */ node,
                     PcpLayerStackSite( gp.GetLayerStack(),
                                        gpRelocSource ),
                     PcpMapExpression::Identity(),
                     /* arcSiblingNum = */ 0,
                     /* directNodeShouldContributeSpecs = */ false,
                     /* includeAncestralOpinions = */ false,
                     /* skipDuplicateNodes = */ false,
                     indexer );
        }
    }
}

////////////////////////////////////////////////////////////////////////
// Class-based Arcs

// Walk over the child nodes of parent, looking for an existing inherit
// node.
static PcpNodeRef
_FindMatchingChild(const PcpNodeRef& parent,
                   const PcpArcType parentArcType,
                   const PcpLayerStackSite& site,
                   const PcpArcType arcType,
                   const PcpMapExpression & mapToParent,
                   int depthBelowIntroduction)
{
    // Arbitrary-order traversal.
    TF_FOR_ALL(childIt, Pcp_GetChildrenRange(parent)) {
        const PcpNodeRef& child = *childIt;

        // XXX:RelocatesSourceNodes: This somewhat arcane way of comparing
        // inherits arc "identity" is necessary to handle the way implied
        // inherits map across relocation source nodes.  In particular,
        // comparing only the sites there would give us a collision, because
        // the sites for implied inherits under relocates sources are
        // not necessarily meaningful.
        if (parentArcType == PcpArcTypeRelocate) {
            if (child.GetArcType() == arcType &&
                child.GetMapToParent().Evaluate() == mapToParent.Evaluate() &&
                child.GetOriginNode().GetDepthBelowIntroduction()
                == depthBelowIntroduction) {
                return child;
            }
        }
        else {
            if (child.GetSite() == site) {
                return child;
            }
        }
    }
    return PcpNodeRef();
}

static SdfPath
_FindContainingVariantSelection(SdfPath p)
{
    while (!p.IsEmpty() && !p.IsPrimVariantSelectionPath()) {
        p = p.GetParentPath();
    }
    return p;
}

// Use the mapping function to figure out the path of the site to
// inherit, by mapping the parent's site back to the source.
static SdfPath
_DetermineInheritPath(
    const SdfPath & parentPath,
    const PcpMapExpression & inheritMap )
{
    // For example, given an inherit map like this:
    //    source: /Class
    //    target: /Model
    //
    // Say we are adding this inherit arc to </Model>; we'll map
    // the target path back to </Class>.
    //
    // Why don't we just use the source path directly?
    // The reason we use a mapping function to represent the arc,
    // rather than simply passing around the path of the class itself,
    // is to let us account for relocations that happened along the
    // way.  See TrickySpookyInheritsInSymmetricRig for an example
    // where we reparent a rig's LArm/Anim scope out to the anim
    // interface, and we need to account for the "spooky inherit"
    // back to SymArm/Anim from the new location.  The PcpMapFunction
    // lets us account for any relocations needed.
    //
    // We also have to handle variants here.  PcpLayerStackSites for variant
    // arcs may contain variant selections.  These variant selections
    // are purely to address appropriate section of opinion storage
    // in the layer, however; variant selections are *not* an aspect
    // of composed scene namespace, and must never appear in the paths
    // used in mapping functions.  Therefore, to add a class arc to a
    // variant-selection site, we take additional measures to strip out
    // the variant selections before mapping the path and then re-add
    // them afterwards.
    //
    if (!parentPath.ContainsPrimVariantSelection()) {
        // Easy case: Just map the site back across the inherit.
        return inheritMap.MapTargetToSource(parentPath);
    } else {
        // Harder case: The site path has variant selections.
        // We want to map the site's namespace back across the
        // inherit, but retain the embedded variant selections.

        // Find the nearest containing variant selection.
        SdfPath varPath = _FindContainingVariantSelection(parentPath);
        TF_VERIFY(!varPath.IsEmpty());

        // Strip the variant selections from the site path, apply the
        // inherit mapping, then re-add the variant selections.
        return inheritMap.MapTargetToSource(
                parentPath.StripAllVariantSelections() )
                .ReplacePrefix( varPath.StripAllVariantSelections(), varPath );
    }
}

// A helper that adds a single class-based arc below the given parent,
// returning the new node.  If the arc already exists, this
// returns the existing node.
static PcpNodeRef
_AddClassBasedArc(
    PcpArcType arcType,
    PcpNodeRef parent,
    PcpNodeRef origin,
    const PcpMapExpression & inheritMap,
    const int inheritArcNum,
    const PcpLayerStackSite & ignoreIfSameAsSite,
    Pcp_PrimIndexer *indexer )
{
    PCP_INDEXING_PHASE(
        indexer, parent, "Preparing to add %s arc to %s", 
        TfEnum::GetDisplayName(arcType).c_str(),
        Pcp_FormatSite(parent.GetSite()).c_str());

    PCP_INDEXING_MSG(
        indexer, parent,
        "origin: %s\n"
        "inheritArcNum: %d\n"
        "ignoreIfSameAsSite: %s\n",
        Pcp_FormatSite(origin.GetSite()).c_str(),
        inheritArcNum,
        ignoreIfSameAsSite == PcpLayerStackSite() ? 
            "<none>" : Pcp_FormatSite(ignoreIfSameAsSite).c_str());

    // Use the inherit map to figure out the site path to inherit.
    SdfPath inheritPath = 
        _DetermineInheritPath( parent.GetPath(), inheritMap );

    // We need to check the parent node's arc type in a few places
    // below. PcpNode::GetArcType is insufficient because we could be in a
    // recursive prim indexing call. In that case, we need to know what
    // the arc type will be once this node is incorporated into the parent
    // prim index. We can use the PcpPrimIndex_StackFrameIterator to 
    // determine that.
    const PcpArcType parentArcType = 
        PcpPrimIndex_StackFrameIterator(parent, indexer->previousFrame)
        .GetArcType();

    if (!inheritPath.IsEmpty()) {
        PCP_INDEXING_MSG(indexer, parent,
                         "Inheriting from path <%s>", inheritPath.GetText());
    }
    else {
        // The parentNode site is outside the co-domain of the inherit.
        // This means there is no appropriate site for the parent
        // to inherit opinions along this inherit arc.
        //
        // For example, this could be an inherit that reaches outside
        // a referenced root to another subroot class, which cannot
        // be mapped across that reference.  Or it could be a root class
        // inherit in the context of a variant: variants cannot contain
        // opinions about root classes.
        //
        // This is not an error; it just means the class arc is not
        // meaningful from this site.
        PCP_INDEXING_MSG(indexer, parent,
                         "No appropriate site for inheriting opinions");
        return PcpNodeRef();
    }

    PcpLayerStackSite inheritSite( parent.GetLayerStack(), inheritPath );

    // Check if there are multiple inherits with the same site.
    // For example, this might be an implied inherit that was also
    // broken down explicitly.
    if (PcpNodeRef child = _FindMatchingChild(
            parent, parentArcType, inheritSite, arcType, inheritMap,
            origin.GetDepthBelowIntroduction())) {

        PCP_INDEXING_MSG(
            indexer, parent, child,
            TfEnum::GetDisplayName(arcType).c_str(),
            "A %s arc to <%s> already exists. Skipping.",
            inheritPath.GetText());

        // TODO Need some policy to resolve multiple arcs.  Existing Csd
        //      prefers the weaker of the two.  Currently, this just
        //      leaves the one that happened to get populated first
        //      in place, which is too loosey-goosey.
        return child;
    }

    // The class-based arc may map this path un-changed. For example,
    // consider an implied inherit being propagated from under a
    // reference node, that is in turn a child of a relocation node:
    //
    //   root -> relocation -> reference -> inherit
    //                    :
    //                    +--> implied inherit
    //
    // The reference node's mapToParent will apply the effect of the
    // relocations, because it is bringing opinions into a namespace
    // where relocations have been applied.  As a result, as soon as
    // the inherit is transferred to become the implied inherit, the
    // implied inherit map function also also includes the relocations.
    //
    // When we use it to _DetermineInheritPath() from the relocation node,
    // the relocation source site will end up hitting the identity
    // mapping (/ -> /) that every inherit has, and yield the same
    // path unchanged.
    //
    // We need to add these nodes to the graph to represent the logical
    // presence of the class arc, and to ensure that it continues to
    // be propagated further up the graph.  However, we do not want to
    // contribute redundant opinions, so we mark the newly added node
    // with shouldContributeSpecs=false.
    //
    // XXX: This situation is a pretty subtle implication of the way
    // we use PcpNodes to represent (and propagate) inherits. Overall,
    // it seems like an opportunity to find a cleaner representation.
    //
    const bool shouldContributeSpecs =
        (inheritPath != parent.GetPath()) &&
        (inheritSite != ignoreIfSameAsSite);

    // If we hit the cases described above, we need to ensure the placeholder
    // duplicate nodes are added to the graph to ensure the continued 
    // propagation of implied classes. Otherwise, duplicate nodes should
    // be skipped over to ensure we don't introduce different paths
    // to the same site.
    const bool skipDuplicateNodes = shouldContributeSpecs;

    // Only subroot prim classes need to compute ancestral opinions.
    const bool includeAncestralOpinions =
        shouldContributeSpecs && !inheritPath.IsRootPrimPath();

    PcpNodeRef newNode =
        _AddArc( arcType, parent, origin,
                 inheritSite, inheritMap, inheritArcNum,
                 /* directNodeShouldContributeSpecs = */ shouldContributeSpecs,
                 includeAncestralOpinions,
                 skipDuplicateNodes,
                 indexer );

    return newNode;
}

// Helper function for adding a list of class-based arcs under the given
// node in the given prim index.
static void
_AddClassBasedArcs(
    PcpPrimIndex* index,
    const PcpNodeRef& node,
    const SdfPathVector& classArcs,
    PcpArcType arcType,
    Pcp_PrimIndexer* indexer)
{
    for (size_t arcNum=0; arcNum < classArcs.size(); ++arcNum) {
        SdfPath const& arcPath = classArcs[arcNum];

        PCP_INDEXING_MSG(indexer, node, "Found %s to <%s>", 
            TfEnum::GetDisplayName(arcType).c_str(),
            arcPath.GetText());

        // Verify that the class-based arc (i.e., inherit or specialize)
        // targets a prim path, with no variant selection.
        if (!arcPath.IsEmpty() &&
            !(arcPath.IsPrimPath() &&
              !arcPath.ContainsPrimVariantSelection())) {
            PcpErrorInvalidPrimPathPtr err = PcpErrorInvalidPrimPath::New();
            err->rootSite = PcpSite(node.GetRootNode().GetSite());
            err->site = PcpSite(node.GetSite());
            err->primPath = arcPath;
            err->arcType = arcType;
            indexer->RecordError(err);
            continue;
        }

        // The mapping for a class arc maps the class to the instance.
        // Every other path maps to itself.
        PcpMapExpression mapExpr = 
            _CreateMapExpressionForArc(
                /* source */ arcPath, /* targetNode */ node,
                indexer->inputs)
            .AddRootIdentity();

        _AddClassBasedArc(arcType,
            /* parent = */ node,
            /* origin = */ node,
            mapExpr,
            arcNum,
            /* ignoreIfSameAsSite = */ PcpLayerStackSite(),
            indexer);
    }
}

/// Build the effective map function for an implied class arc.
///
/// \p classArc is the original class arc
/// \p transfer is the function that maps the parent of the arc
///    to the destination parent
///
/// Here is an example:
///
/// Say Sullivan_1 references Sullivan, and has a child rig scope Rig
/// that inherits a child class _class_Rig:
///
///   Sullivan_1 -----reference----->  Sullivan
///       |                                |
///       +---Rig                          +---Rig
///       |     :                          |     |
///       |     implicit inherit           |     inherits
///       |     :                          |     |
///       |     V                          |     V
///       +---_class_Rig                   +---_class_Rig
///
/// The mapping for the inherit in Sullivan is
///
///    source: /Sullivan/_class_Rig
///    target: /Sullivan/Rig
///
/// The mapping for the reference is:
///
///    source: /Sullivan
///    target: /Sullivan_1
///
/// The implied classes are determined by applying \p transfer to
/// \p classArc. In the same way we apply MapFunctions to individual
/// paths to move them between namespaces, we apply functions to other
/// functions to move them as well, via PcpMapFunction::Compose(). In
/// this example, we use the reference mapping as the function to
/// figure out the equivalent implicit class mapping on the left side.
/// This ends up giving us the implicit class result:
///
///    source: /Sullivan_1/_class_Rig
///    target: /Sullivan_1/Rig
///
/// In more elaborate cases where relocations are at play, transferFunc
/// accounts for the effect of the relocations, and the implied class
/// function we return here will also reflect those relocations.
///
static PcpMapExpression
_GetImpliedClass( const PcpMapExpression & transfer,
                  const PcpMapExpression & classArc )
{
    if (transfer.IsConstantIdentity()) {
        return classArc;
    }

    return transfer.Compose( classArc.Compose( transfer.Inverse() ))
        .AddRootIdentity();
}

// Check the given node for class-based children, and add corresponding
// implied classes to the parent node.
static void
_EvalImpliedClassTree(
    PcpPrimIndex *index, 
    PcpNodeRef destNode,
    PcpNodeRef srcNode,
    const PcpMapExpression & transferFunc,
    bool srcNodeIsStartOfTree,
    Pcp_PrimIndexer *indexer)
{
    // XXX:RelocatesSourceNodes: Avoid propagating implied classes to
    // relocates nodes here. Classes on relocate nodes only exist as
    // placeholders so that they can continue to be propagated after
    // the relocation source tree is added to the prim index in _AddArc.
    // We don't need to propagate classes to relocate nodes here because
    // we don't need them to serve as placeholders; instead, we can just
    // propagate them directly to the relocate node's parent.
    //
    // Doing this avoids having to work around path translation subtleties
    // in _AddClassBasedArc.
    if (destNode.GetArcType() == PcpArcTypeRelocate) {
        // Create a transfer function for the relocate node's parent by
        // composing the relocate node's mapToParent with the given transfer
        // function. See _EvalImpliedClasses for more details.
        const PcpMapExpression newTransferFunc = 
            destNode.GetMapToParent().AddRootIdentity().Compose(transferFunc);
        _EvalImpliedClassTree(
            index, destNode.GetParentNode(), srcNode, newTransferFunc, 
            srcNodeIsStartOfTree, indexer);

        // Ensure that any ancestral class hierarchies beginning under 
        // destNode are propagated. This normally occurs naturally when
        // a new implied class arc is added under destNode. However,
        // since we're adding implied class arcs to destNode's parent
        // instead, we have to explicitly add a task to ensure this occurs.
        // See TrickyInheritsAndRelocates5 for a test case where this is
        // important.
        indexer->AddTask(Task(Task::Type::EvalImpliedClasses, destNode));
        return;
    }

    // Visit all class arcs under srcNode, in arbitrary order.
    // Walk over the tree below srcNode, pushing to the parent.
    //
    // NOTE: We need to grab a copy of the child list and not just
    //       a reference. The recursive call may cause more nodes to
    //       be added to the graph's node pool, which would invalidate
    //       the reference.
    for (const PcpNodeRef& srcChild : Pcp_GetChildren(srcNode)) {
        // Skip everything that isn't a class-based arc.
        if (!PcpIsClassBasedArc(srcChild.GetArcType()))
            continue;

        PCP_INDEXING_MSG(
            indexer, srcChild, destNode, 
            "Attempting to propagate %s of %s to %s.", 
            TfEnum::GetDisplayName(srcChild.GetArcType()).c_str(),
            Pcp_FormatSite(srcChild.GetSite()).c_str(),
            Pcp_FormatSite(destNode.GetSite()).c_str());

        // Now, the purpose of this entire function is to propagate an
        // entire class hierarchy below one node, to its parent:
        //
        //    destNode ---> srcNode
        //                   : :
        //                  :   :
        //                 :     :
        //                :       :
        //             (...classes...)
        //
        // However, consider what happens when destNode inherits
        // srcNode, which also inherits some otherNode:
        //
        //              i            i
        //    destNode ---> srcNode ---> otherNode
        //
        // As we are processing the class-based children of srcNode,
        // we need to somehow distinguish the true children (i.e.
        // namespace descendants) from the arc that continues
        // the destNode --> srcNode --> otherNode chain.
        // We do NOT want to add an implied class arc directly
        // from otherNode to destNode.
        //
        if (srcNodeIsStartOfTree
            && PcpIsClassBasedArc(srcNode.GetArcType())
            && srcNode .GetDepthBelowIntroduction() ==
               srcChild.GetDepthBelowIntroduction()) {

            PCP_INDEXING_MSG(indexer, srcChild, destNode,
                             "Skipping ancestral class");
            continue;
        }

        // Determine the equivalent class mapping under destNode.
        PcpMapExpression destClassFunc =
            _GetImpliedClass(transferFunc, srcChild.GetMapToParent());

        PCP_INDEXING_MSG(
            indexer, srcChild, destNode, 
            "Transfer function:\n%s", transferFunc.GetString().c_str());
        PCP_INDEXING_MSG(
            indexer, srcChild, destNode, 
            "Implied class:\n%s", destClassFunc.GetString().c_str());

        PcpNodeRef destChild;

        // Check to see if an implied class for srcChild has already been
        // propagated to destNode by examining origin nodes. If we find a 
        // a child node whose origin matches srcChild, that node must be
        // the implied class for srcChild, so we don't don't need to redo 
        // the work to process it.
        TF_FOR_ALL(destChildIt, Pcp_GetChildrenRange(destNode)) {
            if (destChildIt->GetOriginNode() == srcChild && 
                destChildIt->GetMapToParent().Evaluate() 
                    == destClassFunc.Evaluate()) {
                destChild = *destChildIt;

                PCP_INDEXING_MSG(
                    indexer, srcChild, destChild,
                    "Found previously added implied inherit node");
                break;
            }
        }

        // Try to add this implied class.
        //
        // This may fail if there's no equivalent site to inherit, due to
        // the namespace domains of the mappings involved.  Or it may
        // return an existing node if destNode already inherits the site.
        //
        // We use the same origin and sibling number information
        // as the srcChild in order to properly account for the
        // effective strength of this implied class.  For example,
        // there may be multiple class arcs from srcNode that
        // we are pushing to destNode, and we need to preserve
        // their relative strength.  destNode may also end up
        // receiving implied classes from multiple different
        // sources; we rely on their distinct origins to reconcile
        // their strength.
        //
        // It is also possible that the newly added class arc would
        // represent a redundant arc in the scene, due to relocations
        // or variants.  For example, this might be an inherit of
        // a class outside the scope of the relocation or variant.
        // We do not want to contribute redundant opinions to the
        // scene, but we still want to continue propagating the
        // inherit arc up the graph.  To handle this, we provide
        // the ignoreIfSameAsSite (the inherit site we are propagating)
        // so that _AddClassBasedArc() can determine if this would be
        // a redundant inherit.
        //
        if (!destChild) {
            destChild = _AddClassBasedArc(
                srcChild.GetArcType(),
                /* parent = */ destNode,
                /* origin = */ srcChild,
                destClassFunc,
                srcChild.GetSiblingNumAtOrigin(),
                /* ignoreIfSameAsSite = */ srcChild.GetSite(),
                indexer);
        }

        // If we successfully added the arc (or found it already existed)
        // recurse on nested classes.  This will build up the full
        // class hierarchy that we are inheriting.
        // Optimization: Recursion requires some cost to set up
        // childTransferFunc, below.  Before we do that work,
        // check if there are any nested inherits.
        if (destChild && _HasClassBasedChild(srcChild)) {
            // Determine the transferFunc to use for the nested child,
            // by composing the functions to walk up from the srcChild,
            // across the transferFunc, and down to the destChild.
            // (Since we are walking down to destChild, we use the
            // inverse of its mapToParent.)
            //
            // This gives us a childTransferFunc that will map the
            // srcChild namespace to the destChild namespace, so
            // that can continue propagating implied classes from there.
            //
            PcpMapExpression childTransferFunc =
                destClassFunc.Inverse()
                .Compose(transferFunc.Compose(srcChild.GetMapToParent()));

            _EvalImpliedClassTree(index, destChild, srcChild,
                                  childTransferFunc, 
                                  /* srcNodeIsStartOfTree = */ false,
                                  indexer);
        }
    }
}

static bool
_IsPropagatedSpecializesNode(
    const PcpNodeRef& node);

static void
_EvalImpliedClasses(
    PcpPrimIndex *index, 
    PcpNodeRef node,
    Pcp_PrimIndexer *indexer)
{
    PCP_INDEXING_PHASE(
        indexer, node,
        "Evaluating implied classes at %s", 
        Pcp_FormatSite(node.GetSite()).c_str());

    // If this is the root node, there is no need to propagate classes.
    if (!node.GetParentNode())
        return;

    // Do not allow inherits to propagate from beneath propagated
    // specializes arcs.  These inherits need to be propagated from
    // the origin of these specializes arcs -- this ensures the origin
    // nodes of the propagated inherits have a consistent strength 
    // ordering.  This is handled with the implied specializes task.
    if (_IsPropagatedSpecializesNode(node)) {
        return;
    }

    // Optimization: early-out if there are no class arcs to propagate.
    if (!_HasClassBasedChild(node)) {
        return;
    }

    // Grab the mapping to the parent node.
    // We will use it to map ("transfer") the class to the parent.
    // The mapping to the parent may have a restricted domain, such as
    // for a reference arc, which only maps the reference root prim.
    // To map root classes across such a mapping, we need to add
    // an identity (/->/) entry.  This is not a violation of reference
    // namespace encapsulation: classes deliberately work this way.
    PcpMapExpression transferFunc = node.GetMapToParent().AddRootIdentity();

    _EvalImpliedClassTree( index, node.GetParentNode(), node,
                           transferFunc, 
                           /* srcNodeIsStartOfTree = */ true, 
                           indexer );
}

////////////////////////////////////////////////////////////////////////
// Inherits

// Evaluate any inherit arcs expressed directly at node.
static void
_EvalNodeInherits(
    PcpPrimIndex *index, 
    PcpNodeRef node, 
    Pcp_PrimIndexer *indexer)
{
    PCP_INDEXING_PHASE(
        indexer, node,
        "Evaluating inherits at %s", 
        Pcp_FormatSite(node.GetSite()).c_str());

    if (!node.CanContributeSpecs())
        return;

    // Compose value for local inherits.
    SdfPathVector inhArcs;
    PcpComposeSiteInherits(node, &inhArcs);

    // Add inherits arcs.
    _AddClassBasedArcs(
        index, node, inhArcs,
        PcpArcTypeInherit,
        indexer);
}

////////////////////////////////////////////////////////////////////////
// Specializes

// Evaluate any specializes arcs expressed directly at node.
static void
_EvalNodeSpecializes(
    PcpPrimIndex* index,
    const PcpNodeRef& node,
    Pcp_PrimIndexer* indexer)
{
    PCP_INDEXING_PHASE(
        indexer, node,
        "Evaluating specializes at %s", 
        Pcp_FormatSite(node.GetSite()).c_str());

    if (!node.CanContributeSpecs())
        return;

    // Compose value for local specializes.
    SdfPathVector specArcs;
    PcpComposeSiteSpecializes(node, &specArcs);

    // Add specializes arcs.
    _AddClassBasedArcs(
        index, node, specArcs,
        PcpArcTypeSpecialize,
        indexer);
}

// Returns true if the given node is a specializes node that
// has been propagated to the root of the graph for strength
// ordering purposes in _EvalImpliedSpecializes.
static bool
_IsPropagatedSpecializesNode(
    const PcpNodeRef& node)
{
    return (PcpIsSpecializeArc(node.GetArcType()) && 
            node.GetParentNode() == node.GetRootNode() && 
            node.GetSite() == node.GetOriginNode().GetSite());
}

static bool
_IsNodeInSubtree(
    const PcpNodeRef& node,
    const PcpNodeRef& subtreeRoot)
{
    for (PcpNodeRef n = node; n; n = n.GetParentNode()) {
        if (n == subtreeRoot) {
            return true;
        }
    }
    return false;
}

static std::pair<PcpNodeRef, bool>
_PropagateNodeToParent(
    PcpNodeRef parentNode,
    PcpNodeRef srcNode,
    bool skipImpliedSpecializes,
    bool skipTasksForExpressedArcs,
    const PcpMapExpression& mapToParent,
    const PcpNodeRef& srcTreeRoot,
    Pcp_PrimIndexer* indexer)
{
    bool createdNewNode = false;

    PcpNodeRef newNode;
    if (srcNode.GetParentNode() == parentNode) {
        newNode = srcNode;
    }
    else {
        newNode = _FindMatchingChild(
            parentNode, parentNode.GetArcType(),
            srcNode.GetSite(), srcNode.GetArcType(),
            mapToParent, srcNode.GetDepthBelowIntroduction());

        if (!newNode) {
            // Only propagate a node if it's a non-implied arc or if it's an
            // implied arc whose origin is outside the subgraph we're 
            // propagating. If this is an implied arc whose origin is
            // within the subgraph, it will be handled when we evaluate
            // implied class arcs on the subgraph being propagated.
            if (!_IsImpliedClassBasedArc(srcNode) ||
                !_IsNodeInSubtree(srcNode.GetOriginNode(), srcTreeRoot)) {

                const int namespaceDepth = 
                    (srcNode == srcTreeRoot ?
                        PcpNode_GetNonVariantPathElementCount(
                            parentNode.GetPath()) :
                        srcNode.GetNamespaceDepth());

                const PcpNodeRef originNode = 
                    (srcNode == srcTreeRoot || _IsImpliedClassBasedArc(srcNode) ?
                        srcNode : parentNode);

                newNode = _AddArc(srcNode.GetArcType(),
                    /* parent = */ parentNode,
                    /* origin = */ originNode,
                    srcNode.GetSite(),
                    mapToParent,
                    srcNode.GetSiblingNumAtOrigin(),
                    namespaceDepth,
                    /* directNodeShouldContributeSpecs = */ !srcNode.IsInert(),
                    /* includeAncestralOpinions = */ false,
                    /* skipDuplicateNodes = */ false,
                    skipImpliedSpecializes,
                    skipTasksForExpressedArcs,
                    indexer);

                createdNewNode = static_cast<bool>(newNode);
            }
        }

        if (newNode) {
            newNode.SetInert(srcNode.IsInert());
            newNode.SetHasSymmetry(srcNode.HasSymmetry());
            newNode.SetPermission(srcNode.GetPermission());
            newNode.SetRestricted(srcNode.IsRestricted());

            srcNode.SetInert(true);
        }
        else {
            _InertSubtree(srcNode);
        }
    }

    return {newNode, createdNewNode};
}

static void
_PropagateSpecializesTreeToRoot(
    PcpPrimIndex* index,
    PcpNodeRef parentNode,
    PcpNodeRef srcNode,
    PcpNodeRef originNode,
    const PcpMapExpression& mapToParent,
    const PcpNodeRef& srcTreeRoot,
    Pcp_PrimIndexer* indexer)
{
    // Make sure to skip implied specializes tasks for the propagated
    // node. Otherwise, we'll wind up propagating this node back to
    // its originating subtree, which will leave it inert. But we still want
    // to queue the expressed arc tasks for the nodes we propagate to the root.
    const bool skipImpliedSpecializes = true;
    const bool skipTasksForExpressedArcs = false;

    std::pair<PcpNodeRef, bool> newNode = _PropagateNodeToParent(
        parentNode, srcNode,
        skipImpliedSpecializes, skipTasksForExpressedArcs,
        mapToParent, srcTreeRoot, indexer);
    if (!newNode.first) {
        return;
    }

    for (PcpNodeRef childNode : Pcp_GetChildren(srcNode)) {
        if (!PcpIsSpecializeArc(childNode.GetArcType())) {
            _PropagateSpecializesTreeToRoot(
                index, newNode.first, childNode, newNode.first, 
                childNode.GetMapToParent(), srcTreeRoot, indexer);
        }
    }
}

static void
_FindSpecializesToPropagateToRoot(
    PcpPrimIndex* index,
    PcpNodeRef node,
    Pcp_PrimIndexer* indexer)
{
    // XXX:RelocatesSourceNodes: This node may be a placeholder 
    // implied arc under a relocation node that is only present 
    // to allow class-based arcs to be implied up the prim index. 
    // These placeholders are not valid sources of opinions, so
    // we can cut off our search for specializes to propagate.
    const PcpNodeRef parentNode = node.GetParentNode();
    const bool nodeIsRelocatesPlaceholder =
        parentNode != node.GetOriginNode() && 
        parentNode.GetArcType() == PcpArcTypeRelocate &&
        parentNode.GetSite() == node.GetSite();
    if (nodeIsRelocatesPlaceholder) {
        return;
    }

    if (PcpIsSpecializeArc(node.GetArcType())) {
        PCP_INDEXING_MSG(
            indexer, node, node.GetRootNode(),
            "Propagating specializes arc %s to root", 
            Pcp_FormatSite(node.GetSite()).c_str());

        // HACK: When we propagate specializes arcs from the root 
        // to their origin in _PropagateArcsToOrigin, we will mark 
        // them as inert=false. However, we will *not* do the same 
        // for any of the implied specializes that originate from 
        // that arc -- they will be left with inert=true.
        // 
        // If we wind up having to propagate these implied specializes
        // back to the root, we will wind up copying the inert=true
        // flag, which isn't what we want. Instead of trying to fix
        // up the implied specializes in _PropagateArcsToOrigin,
        // it's much simpler if we just deal with that here by forcing
        // the specializes node to inert=false.
        node.SetInert(false);

        _PropagateSpecializesTreeToRoot(
            index, index->GetRootNode(), node, node,
            node.GetMapToRoot(), node, indexer);
    }

    for (PcpNodeRef childNode : Pcp_GetChildren(node)) {
        _FindSpecializesToPropagateToRoot(index, childNode, indexer);
    }
}

static void
_PropagateArcsToOrigin(
    PcpPrimIndex* index,
    PcpNodeRef parentNode,
    PcpNodeRef srcNode,
    const PcpMapExpression& mapToParent,
    const PcpNodeRef& srcTreeRoot,
    Pcp_PrimIndexer* indexer)
{
    // Don't skip implied specializes tasks as we propagate arcs back
    // to the origin.  If one of the arcs we propagate back is another
    // specializes arc, we need to ensure that arc is propagated back
    // to the root later on.
    //
    // But we DO want to skip any expressed arc tasks as we propagate back to 
    // the origin so that we can copy the whole subtree (including all direct 
    // and ancestral arcs) without enqueing new tasks for the propagated nodes
    // which could lead to duplicate tasks being queued up for the propagated
    // subtree nodes and failed verifies later on.
    // See SpecializesAndAncestralArcs museum cases.
    const bool skipImpliedSpecializes = false;
    const bool skipTasksForExpressedArcs = true;

    std::pair<PcpNodeRef, bool> newNode = _PropagateNodeToParent(
        parentNode, srcNode, skipImpliedSpecializes, skipTasksForExpressedArcs,
        mapToParent, srcTreeRoot, indexer);
    if (!newNode.first) {
        return;
    }

    for (PcpNodeRef childNode : Pcp_GetChildren(srcNode)) {
        _PropagateArcsToOrigin(
            index, newNode.first, childNode, childNode.GetMapToParent(), 
            srcTreeRoot, indexer);
    }
}

static void
_FindArcsToPropagateToOrigin(
    PcpPrimIndex* index,
    const PcpNodeRef& node,
    Pcp_PrimIndexer* indexer)
{
    TF_VERIFY(PcpIsSpecializeArc(node.GetArcType()));

    for (PcpNodeRef childNode : Pcp_GetChildren(node)) {
        PCP_INDEXING_MSG(
            indexer, childNode, node.GetOriginNode(),
            "Propagating arcs under %s to specializes origin %s", 
            Pcp_FormatSite(childNode.GetSite()).c_str(),
            Pcp_FormatSite(node.GetOriginNode().GetSite()).c_str());

        _PropagateArcsToOrigin(
            index, node.GetOriginNode(), childNode, childNode.GetMapToParent(),
            node, indexer);
    }
}

// Opinions from specializes arcs, including those that are implied across
// other arcs, are always weaker than the target of those arcs.  Conceptually, 
// this means that opinions from all specializes arcs (and any encapsulated
// arcs) come after all other opinions.   
// 
//                                 ref
// For instance,          Model ---------> Ref
// given this example:    |                |
//                        +- Instance      +- Instance
//                        |   :            |   :
//                        |   : implied    |   : specializes
//                        |   v            |   v 
//                        +- Class         +- Class
//
// The intended strength ordering is for /Model/Instance is:
//   [/Model/Instance, /Ref/Instance, /Model/Class, /Ref/Class].
//
// To achieve this, we propagate specializes subgraphs in the prim index
// to the root of the graph.  Strength ordering will then place the
// specializes arcs at the end of the graph, after all other arcs.
//
// We need to reverse this process when we discover additional arcs
// beneath the specializes subgraphs that have been propagated to the
// root.  This can happen if there are namespace children beneath the
// source of a specializes arc with their own arcs.  This can also
// happen if we discover variants after processing implied specializes.
//
// When we encounter this situation, the specializes subgraph is
// propagated back to its origin.  The primary purpose of this is to
// allow any implied arcs to be propagated to the necessary locations
// using the already-existing mechanisms.  Once that's done,
// the subgraph will be propagated back to the root.  
// 
static void
_EvalImpliedSpecializes(
    PcpPrimIndex* index,
    const PcpNodeRef& node,
    Pcp_PrimIndexer* indexer)
{
    PCP_INDEXING_PHASE(
        indexer, node,
        "Evaluating implied specializes at %s", 
        Pcp_FormatSite(node.GetSite()).c_str());

    // If this is the root node, there is no need to propagate specializes.
    if (!node.GetParentNode())
        return;

    if (_IsPropagatedSpecializesNode(node)) {
        _FindArcsToPropagateToOrigin(index, node, indexer);
    }
    else {
        _FindSpecializesToPropagateToRoot(index, node, indexer);
    }
}

////////////////////////////////////////////////////////////////////////
// Variants

static bool
_ComposeVariantSelectionForNode(
    const PcpNodeRef& node,
    const SdfPath& pathInNode,
    const std::string & vset,
    std::string *vsel,
    PcpNodeRef *nodeWithVsel,
    Pcp_PrimIndexer *indexer)
{
    TF_VERIFY(!pathInNode.IsEmpty());

    // We are using path-translation to walk between nodes, so we
    // are working exclusively in namespace paths, which must have
    // no variant selection.
    TF_VERIFY(!pathInNode.ContainsPrimVariantSelection(),
              "Unexpected variant selection in namespace path <%s>",
              pathInNode.GetText());

    // If this node has an authored selection, use that.
    // Note that we use this even if the authored selection is
    // the empty string, which explicitly selects no variant.
    if (node.CanContributeSpecs()) {
        PcpLayerStackSite site(node.GetLayerStack(), pathInNode);
        // pathInNode is a namespace path, not a storage path,
        // so it will contain no variant selection (as verified above).
        // To find the storage site, we need to insert any variant
        // selection for this node.
        if (node.GetArcType() == PcpArcTypeVariant) {
            site.path = pathInNode.ReplacePrefix(
                node.GetPath().StripAllVariantSelections(),
                node.GetPath());
        }

        std::unordered_set<std::string> exprVarDependencies;
        PcpErrorVector errors;

        const bool foundSelection = 
            PcpComposeSiteVariantSelection(
                site.layerStack, site.path, vset, vsel, 
                &exprVarDependencies, &errors);

        if (!exprVarDependencies.empty()) {
            indexer->outputs->expressionVariablesDependency.AddDependencies(
                site.layerStack, std::move(exprVarDependencies));
        }

        if (!errors.empty()) {
            for (const PcpErrorBasePtr& err : errors) {
                indexer->RecordError(err);
            }
        }

        if (foundSelection) {
            *nodeWithVsel = node;
            return true;
        }
    }

    return false;
}

// Check the tree of nodes rooted at the given node for any node
// representing a prior selection for the given variant set for the path.
static bool
_FindPriorVariantSelection(
    const PcpNodeRef& node,
    const SdfPath &pathInRoot,
    int ancestorRecursionDepth,
    const std::string & vset,
    std::string *vsel,
    PcpNodeRef *nodeWithVsel)
{
    // If this node represents a variant selection at the same
    // effective depth of namespace, then check its selection.
    if (node.GetArcType() == PcpArcTypeVariant &&
        node.GetDepthBelowIntroduction() == ancestorRecursionDepth) {
        const SdfPath nodePathAtIntroduction = node.GetPathAtIntroduction();
        const std::pair<std::string, std::string> nodeVsel =
            nodePathAtIntroduction.GetVariantSelection();
        if (nodeVsel.first == vset) {
            // The node has a variant selection for the variant set we're 
            // looking for, but we still have to check that the node actually
            // represents the prim path we're choosing a variant selection for
            // (as opposed to a different prim path that just happens to have
            // a variant set with the same name.
            // 
            // Note that we have to map search prim path back down this node
            // to compare it as it was mapped up to the root of this node's 
            // graph before being passed to this function.
            const SdfPath pathInNode =
                node.GetMapToRoot().MapTargetToSource(pathInRoot);
            // If the path didn't translate to this node, it won't translate
            // to any of the node's children, so we might as well early out
            // here.
            if (pathInNode.IsEmpty()) {
                return false;
            }
            if (nodePathAtIntroduction.GetPrimPath() == pathInNode) {
                *vsel = nodeVsel.second;
                *nodeWithVsel = node;
                return true;
            }
        }
    }
    TF_FOR_ALL(child, Pcp_GetChildrenRange(node)) {
        if (_FindPriorVariantSelection(
                *child, pathInRoot, ancestorRecursionDepth, 
                vset, vsel, nodeWithVsel)) {
            return true;
        }
    }
    return false;
}

typedef std::pair<PcpPrimIndex_StackFrame*, PcpNodeRef> _StackFrameAndChildNode;
typedef std::vector<_StackFrameAndChildNode> _StackFrameAndChildNodeVector;

static bool
_ComposeVariantSelectionAcrossStackFrames(
    const PcpNodeRef& node,
    const SdfPath& pathInNode,
    const std::string & vset,
    std::string *vsel,
    _StackFrameAndChildNodeVector *stackFrames,
    PcpNodeRef *nodeWithVsel,
    Pcp_PrimIndexer *indexer)
{
    // Compose variant selection in strong-to-weak order.
    if (_ComposeVariantSelectionForNode(
            node, pathInNode, vset, vsel, nodeWithVsel, indexer)) {
        return true;
    }

    // If we're in recursive prim index construction and hit the end
    // of a graph produced by the current stack frame, we need to look 
    // at the next stack frame to continue the traversal to the next
    // part of the graph.
    //
    // XXX: See XXX comment in _ComposeVariantSelection. This probably has
    //      the same bug. The real fix would be to figure out where the
    //      graph for the next stack frame would be inserted into the
    //      current node's children in the below for loop and deal with it
    //      there.
    const bool atEndOfStack = 
        (!stackFrames->empty() &&
         node == stackFrames->back().first->parentNode);
    if (atEndOfStack) {
        const _StackFrameAndChildNode nextFrame = stackFrames->back();
        stackFrames->pop_back();

        const PcpNodeRef& childNode = nextFrame.second;
        const SdfPath pathInChildNode = 
            nextFrame.first->arcToParent->mapToParent
            .MapTargetToSource(pathInNode);

        if (!pathInChildNode.IsEmpty()) {
            return _ComposeVariantSelectionAcrossStackFrames(
                childNode, pathInChildNode, vset, vsel, stackFrames, 
                nodeWithVsel, indexer);
        }

        return false;
    }

    TF_FOR_ALL(child, Pcp_GetChildrenRange(node)) {
        const PcpNodeRef& childNode = *child;
        const SdfPath pathInChildNode =
            childNode.GetMapToParent().MapTargetToSource(pathInNode);

        if (!pathInChildNode.IsEmpty() &&
            _ComposeVariantSelectionAcrossStackFrames(
                *child, pathInChildNode, vset, vsel, stackFrames,
                nodeWithVsel, indexer)) {
            return true;
        }
    }

    return false;
}

// Convert from the given node and the given path at the node to the
// root node and the path mapped to the root node by traversing up the 
// parent nodes. 
static bool
_ConvertToRootNodeAndPath(PcpNodeRef *node, SdfPath *path)
{
    // This function assumes the given path is not empty to begin with so 
    // return true if this is already the root node.
    if (!node->GetParentNode()) {
        return true;
    }
    *path = node->GetMapToRoot().MapSourceToTarget(*path);
    *node = node->GetRootNode();
    // Return whether the path translates fully up to the root node.
    return !path->IsEmpty();
}

static void
_ComposeVariantSelection(
    const PcpNodeRef &node,
    const SdfPath &pathInNode,
    Pcp_PrimIndexer *indexer,
    const std::string &vset,
    std::string *vsel,
    PcpNodeRef *nodeWithVsel)
{
    TRACE_FUNCTION();
    TF_VERIFY(!pathInNode.IsEmpty());
    TF_VERIFY(!pathInNode.ContainsPrimVariantSelection(),
              "%s", pathInNode.GetText());

    // We want to look for variant selections in all nodes that have been 
    // added up to this point.  Note that Pcp may pick up variant
    // selections from weaker locations than the node for which
    // we are evaluating variants.
    //
    // See bug 106950 and TrickyVariantWeakerSelection for more details.
    //
    // This is really a simple strength-order traversal of the
    // current prim index. It is complicated by the fact that we 
    // may be in the middle of recursive calls to Pcp_BuildPrimIndex
    // that are building up subgraphs that will eventually be joined
    // together. To deal with this, we need to keep track of the 
    // stack frames for these recursive calls so that we can traverse 
    // the prim index as if it were fully constructed.
    //
    // Translate the given path up to the root node of the *entire* 
    // prim index under construction, keeping track of when we need
    // to hop across a stack frame.
    _StackFrameAndChildNodeVector previousStackFrames;
    PcpNodeRef rootNode = node;
    SdfPath pathInRoot = pathInNode;
    _ConvertToRootNodeAndPath(&rootNode, &pathInRoot);

    // First check if we have already resolved this variant set in the current
    // stack frame. Try all nodes in all parent frames; ancestorRecursionDepth
    // accounts for any ancestral recursion.
    if (_FindPriorVariantSelection(rootNode, pathInRoot,
                                   indexer->ancestorRecursionDepth,
                                   vset, vsel, nodeWithVsel)) {
        return;
    }

    for (PcpPrimIndex_StackFrame *previousFrame = indexer->previousFrame;
         previousFrame; previousFrame = previousFrame->previousFrame) {
        // There may not be a valid mapping for the current path across 
        // the previous stack frame. For example, this may happen when
        // trying to compose ancestral variant selections on a sub-root
        // reference (see SubrootReferenceAndVariants for an example).
        // This failure means there are no further sites with relevant 
        // variant selection opinions across this stack frame. In this case, 
        // we break out of the loop and only search the portion of the prim
        // index we've traversed.
        SdfPath pathInPreviousFrame =
            previousFrame->arcToParent->mapToParent.MapSourceToTarget(
                pathInRoot);
        PcpNodeRef rootNodeInPreviousFrame = previousFrame->parentNode;
        // Note that even if the path can be mapped across the stack frame it 
        // may not map all the way up to the root of the previous stack frame. 
        // This can happen when composing an ancestor with a variant set for a 
        // subroot inherit. Inherit arcs always have an identity mapping so an 
        // ancestral prim path can still map across the inherit's stack frame, 
        // but it may not map across other arcs, like references, on the way up 
        // to the root. In this case we break out of the loop and only search 
        // the the portion of the index before the stack frame jump.
        if (pathInPreviousFrame.IsEmpty() ||
            !_ConvertToRootNodeAndPath(&rootNodeInPreviousFrame, 
                                       &pathInPreviousFrame)) {
            break;
        }

        // Check if we have already resolved this variant set in this previous
        // stack as well.
        if (_FindPriorVariantSelection(rootNodeInPreviousFrame, 
                                       pathInPreviousFrame,
                                       indexer->ancestorRecursionDepth,
                                       vset, vsel, nodeWithVsel)) {
            return;
        }

        // rootNode is still set to be child of the previous frame's arc which
        // is why do this first.
        previousStackFrames.push_back(
            _StackFrameAndChildNode(previousFrame, rootNode));

        // Update the root node and path to be the root of this previous stack
        // frame.
        rootNode = rootNodeInPreviousFrame;
        pathInRoot = pathInPreviousFrame;
    }

    // Now recursively walk the prim index in strong-to-weak order
    // looking for a variant selection.
    _ComposeVariantSelectionAcrossStackFrames(
        rootNode, pathInRoot, vset, vsel, &previousStackFrames,
        nodeWithVsel, indexer);
}

static bool
_ShouldUseVariantFallback(
    const Pcp_PrimIndexer *indexer,
    const std::string& vset,
    const std::string& vsel,
    const std::string& vselFallback,
    const PcpNodeRef &nodeWithVsel)
{
    // Can't use fallback if we don't have one.
    if (vselFallback.empty()) {
        return false;
    }

    // If there's no variant selected then use the default.
    if (vsel.empty()) {
        return true;
    }

    // The "standin" variant set has special behavior, below.
    // All other variant sets default when there is no selection.
    //
    // XXX This logic can be simpler when we remove the old standin stuff
    if (vset != "standin") {
        return false;
    }

    // If we're using the new behavior then the preferences can't win over
    // the opinion in vsel.
    if (PcpIsNewDefaultStandinBehaviorEnabled()) {
        return false;
    }

    // From here down we're trying to match the Csd policy, which can
    // be rather peculiar.  See bugs 29039 and 32264 for history that
    // lead to some of these policies.

    // If nodeWithVsel is a variant node that makes a selection for vset,
    // it structurally represents the fact that we have already decided
    // which variant selection to use for vset in this primIndex.  In
    // this case, we do not want to apply standin preferences, because
    // we will have already applied them.
    //
    // (Applying the policy again here could give us an incorrect result,
    // because this might be a different nodeWithVsel than was used
    // originally to apply the policy.)
    if (nodeWithVsel.GetArcType() == PcpArcTypeVariant      &&
        nodeWithVsel.GetPath().IsPrimVariantSelectionPath() &&
        nodeWithVsel.GetPath().GetVariantSelection().first == vset) {
        return false;
    }

    // Use the standin preference if the authored selection came from
    // inside the payload.
    for (PcpNodeRef n = nodeWithVsel; n; n = n.GetParentNode()) {
        if (n.GetArcType() == PcpArcTypePayload) {
            return true;
        }
    }

    // Use vsel if it came from a session layer, otherwise check the
    // standin preferences. For efficiency, we iterate over the full
    // layer stack instead of using PcpLayerStack::GetSessionLayerStack.
    const SdfLayerHandle rootLayer = 
        indexer->rootSite.layerStack->GetIdentifier().rootLayer;
    TF_FOR_ALL(layer, indexer->rootSite.layerStack->GetLayers()) {
        if (*layer == rootLayer) {
            break;
        }

        static const TfToken field = SdfFieldKeys->VariantSelection;

        const VtValue& value =
            (*layer)->GetField(indexer->rootSite.path, field);
        if (value.IsHolding<SdfVariantSelectionMap>()) {
            const SdfVariantSelectionMap & vselMap =
                value.UncheckedGet<SdfVariantSelectionMap>();
            SdfVariantSelectionMap::const_iterator i = vselMap.find(vset);
            if (i != vselMap.end() && i->second == vsel) {
                // Standin selection came from the session layer.
                return false;
            }
        }
    }

    // If we don't have a standin selection in the root node then check
    // the standin preferences.
    if (nodeWithVsel.GetArcType() != PcpArcTypeRoot) {
        return true;
    }

    return false;
}

static std::string
_ChooseBestFallbackAmongOptions(
    const std::string &vset,
    const std::set<std::string> &vsetOptions,
    const PcpVariantFallbackMap& variantFallbacks)
{
    PcpVariantFallbackMap::const_iterator vsetIt = variantFallbacks.find(vset);
    if (vsetIt != variantFallbacks.end()) {
        for (const auto &vselIt: vsetIt->second) {
            if (vsetOptions.find(vselIt) != vsetOptions.end()) {
                return vselIt;
            }
        }
    }
    return std::string();
}

static void
_AddVariantArc(Pcp_PrimIndexer *indexer,
               const PcpNodeRef &node,
               const std::string &vset,
               int vsetNum,
               const std::string &vsel)
{
    // Variants do not remap the scenegraph's namespace, they simply
    // represent a branch off into a different section of the layer
    // storage.  For this reason, the source site includes the
    // variant selection but the mapping function is identity.
    SdfPath varPath = node.GetSite().path.AppendVariantSelection(vset, vsel);
    if (_AddArc(PcpArcTypeVariant,
                /* parent = */ node,
                /* origin = */ node,
                PcpLayerStackSite( node.GetLayerStack(), varPath ),
                /* mapExpression = */ PcpMapExpression::Identity(),
                /* arcSiblingNum = */ vsetNum, 
                /* directNodeShouldContributeSpecs = */ true,
                /* includeAncestralOpinions = */ false,
                /* skipDuplicateNodes = */ false,
                indexer )) {
        // If we expanded a variant set, it may have introduced new
        // authored variant selections, so we must retry any pending
        // variant tasks as authored tasks.
        indexer->RetryVariantTasks();
    }
}

static void
_EvalNodeVariantSets(
    PcpPrimIndex *index, 
    const PcpNodeRef& node, 
    Pcp_PrimIndexer *indexer)
{
    PCP_INDEXING_PHASE(
        indexer, node,
        "Evaluating variant sets at %s", 
        Pcp_FormatSite(node.GetSite()).c_str());

    if (!node.CanContributeSpecs())
        return;

    std::vector<std::string> vsetNames;
    PcpComposeSiteVariantSets(node, &vsetNames);

    for (int vsetNum=0, numVsets=vsetNames.size();
         vsetNum < numVsets; ++vsetNum) {
        indexer->AddTask(Task(Task::Type::EvalNodeVariantAuthored,
                              node, std::move(vsetNames[vsetNum]), vsetNum));
    }
}

static void
_EvalNodeAuthoredVariant(
    PcpPrimIndex *index, 
    const PcpNodeRef& node, 
    Pcp_PrimIndexer *indexer,
    const std::string &vset,
    int vsetNum)
{
    PCP_INDEXING_PHASE(
        indexer, node,
        "Evaluating authored selections for variant set %s at %s", 
        vset.c_str(),
        Pcp_FormatSite(node.GetSite()).c_str());

    if (!node.CanContributeSpecs())
        return;

    // Compose options.
    std::set<std::string> vsetOptions;
    PcpComposeSiteVariantSetOptions(node, vset, &vsetOptions);

    // Determine what the fallback selection would be.
    // Generally speaking, authoring opinions win over fallbacks, however if
    // MENV30_ENABLE_NEW_DEFAULT_STANDIN_BEHAVIOR==false then that is not
    // always the case, and we must check the fallback here first.
    // TODO Remove this once we phase out the old behavior!
    const std::string vselFallback =
        _ChooseBestFallbackAmongOptions( vset, vsetOptions,
                                         *indexer->inputs.variantFallbacks );
    if (!vselFallback.empty()) {
        PCP_INDEXING_MSG(
            indexer, node, "Found fallback {%s=%s}",
            vset.c_str(),
            vselFallback.c_str());
    }

    // Determine the authored variant selection for this set, if any.
    std::string vsel;
    PcpNodeRef nodeWithVsel;
    _ComposeVariantSelection(node, node.GetPath().StripAllVariantSelections(),
                             indexer, vset, &vsel, &nodeWithVsel);
    if (!vsel.empty()) {
        PCP_INDEXING_MSG(
            indexer, node, "Found variant selection {%s=%s} at %s",
            vset.c_str(),
            vsel.c_str(),
            Pcp_FormatSite(nodeWithVsel.GetSite()).c_str());
    }
    // Check if we should use the fallback
    if (_ShouldUseVariantFallback(indexer, vset, vsel, vselFallback,
                                  nodeWithVsel)) {
        PCP_INDEXING_MSG(indexer, node, "Deferring to variant fallback");
        indexer->AddTask(Task(Task::Type::EvalNodeVariantFallback,
                              node, vset, vsetNum));
        return;
    }
    // If no variant was chosen, do not expand this variant set.
    if (vsel.empty()) {
        PCP_INDEXING_MSG(indexer, node,
                         "No variant selection found for set '%s'",
                         vset.c_str());
        indexer->AddTask(Task(Task::Type::EvalNodeVariantNoneFound,
                              node, vset, vsetNum));
        return;
    }

    _AddVariantArc(indexer, node, vset, vsetNum, vsel);
}

static void
_EvalNodeFallbackVariant(
    PcpPrimIndex *index, 
    const PcpNodeRef& node, 
    Pcp_PrimIndexer *indexer,
    const std::string &vset,
    int vsetNum)
{
    PCP_INDEXING_PHASE(
        indexer, node,
        "Evaluating fallback selections for variant set %s s at %s", 
        vset.c_str(),
        Pcp_FormatSite(node.GetSite()).c_str());

    if (!node.CanContributeSpecs())
        return;

    // Compose options.
    std::set<std::string> vsetOptions;
    PcpComposeSiteVariantSetOptions(node, vset, &vsetOptions);

    // Determine what the fallback selection would be.
    const std::string vsel =
        _ChooseBestFallbackAmongOptions( vset, vsetOptions,
                                         *indexer->inputs.variantFallbacks );
    // If no variant was chosen, do not expand this variant set.
    if (vsel.empty()) {
        PCP_INDEXING_MSG(indexer, node,
                      "No variant fallback found for set '%s'", vset.c_str());
        indexer->AddTask(Task(Task::Type::EvalNodeVariantNoneFound,
                              node, vset, vsetNum));
        return;
    }

    _AddVariantArc(indexer, node, vset, vsetNum, vsel);
}

////////////////////////////////////////////////////////////////////////
// Prim Specs

void
_GatherNodesRecursively(
    const PcpNodeRef& node,
    std::vector<PcpNodeRef> *result)
{
    result->push_back(node);

    // Strength-order (strong-to-weak) traversal.
    TF_FOR_ALL(child, Pcp_GetChildrenRange(node)) {
        _GatherNodesRecursively(*child, result);
    }
}

static void
_EnforcePermissions(
    PcpPrimIndex *primIndex,
    PcpErrorVector *allErrors)
{
    TRACE_FUNCTION();

    PcpNodeRef rootNode = primIndex->GetRootNode();
    TF_VERIFY(rootNode);

    // Gather all the nodes that may contribute prim specs.
    std::vector<PcpNodeRef> allNodes;
    _GatherNodesRecursively(rootNode, &allNodes);

    // Go backwards through the list of nodes, looking for prim specs.
    // If we find a node that isn't public, we stash it away, and then
    // issue an error for any stronger nodes, which violate permissions.
    PcpNodeRef privateNode;
    TF_REVERSE_FOR_ALL(nodeIter, allNodes) {
        PcpNodeRef curNode = *nodeIter;
        if (!curNode.CanContributeSpecs()) {
            // XXX: Should we be setting permissionDenied?
            continue;
        }

        // If we previously found a private node, the current node is
        // not allowed to contribute specs. 
        if (privateNode) {
            curNode.SetRestricted(true);

            // Check for prim specs in reverse strength order (weak-to-strong).
            // XXX: We should avoid collecting the prim specs here
            //      and then again later when building the prim stack.
            //      If we built the prim stack first we'd have to
            //      discard stuff we discover to be private;  that's
            //      going to be rare so it's okay.
            if (curNode.HasSpecs()) {
                TF_REVERSE_FOR_ALL(layer,
                                   curNode.GetLayerStack()->GetLayers()) {
                    if ((*layer)->HasSpec(curNode.GetPath())) {
                        // The current node has a prim spec. Since this violates
                        // permissions, we ignore this node's specs and report
                        // an error.
                        PcpErrorPrimPermissionDeniedPtr err =
                            PcpErrorPrimPermissionDenied::New();
                        err->rootSite =
                            PcpSite(curNode.GetRootNode().GetSite());
                        err->site = PcpSite(curNode.GetSite());
                        err->privateSite = PcpSite(privateNode.GetSite());
                        Pcp_PrimIndexer::RecordError(err, primIndex, allErrors);
                        break;
                    }
                }
            }
        }
        // If this node is private, any subsequent nodes will generate 
        // errors (see above).
        if (!privateNode &&
            curNode.GetPermission() != SdfPermissionPublic) { 
            privateNode = curNode;
        }
    }
}

void
Pcp_RescanForSpecs(PcpPrimIndex *index, bool usd, bool updateHasSpecs)
{
    TfAutoMallocTag2 tag("Pcp", "Pcp_RescanForSpecs");

    if (usd) {
        // USD does not retain prim stacks.
        // We do need to update the HasSpecs flag on nodes, however.
        if (updateHasSpecs) {
            TF_FOR_ALL(nodeIt, index->GetNodeRange()) {
                nodeIt->SetHasSpecs(PcpComposeSiteHasPrimSpecs(*nodeIt));
            }
        }
    } else {
        Pcp_CompressedSdSiteVector primSites;
        TF_FOR_ALL(nodeIt, index->GetNodeRange()) {
            PcpNodeRef node = *nodeIt;
            bool nodeHasSpecs = false;
            if (!node.IsCulled() && node.CanContributeSpecs()) {
                // Add prim specs in strength order (strong-to-weak).
                const SdfLayerRefPtrVector& layers =
                    node.GetLayerStack()->GetLayers();
                const SdfPath& path = node.GetPath();
                for (size_t i = 0, n = layers.size(); i != n; ++i) {
                    if (layers[i]->HasSpec(path)) {
                        nodeHasSpecs = true;
                        primSites.push_back(node.GetCompressedSdSite(i));
                    }
                }
            }
            if (updateHasSpecs) {
                node.SetHasSpecs(nodeHasSpecs);
            }
        }
        index->_primStack.swap(primSites);
    }
}

////////////////////////////////////////////////////////////////////////

static std::pair<
    PcpNodeRef_PrivateChildrenConstIterator, 
    PcpNodeRef_PrivateChildrenConstIterator>
_GetDirectChildRange(const PcpNodeRef& node, PcpArcType arcType)
{
    auto range = std::make_pair(
        PcpNodeRef_PrivateChildrenConstIterator(node),
        PcpNodeRef_PrivateChildrenConstIterator(node, /* end = */ true));
    for (; range.first != range.second; ++range.first) {
        const PcpNodeRef& childNode = *range.first;
        if (childNode.GetArcType() == arcType && !childNode.IsDueToAncestor()) {
            break;
        }
    }

    auto end = range.second;
    for (range.second = range.first; range.second != end; ++range.second) {
        const PcpNodeRef& childNode = *range.second;
        if (childNode.GetArcType() != arcType || childNode.IsDueToAncestor()) {
            break;
        }
    }

    return range;
}

static bool
_ComputedAssetPathWouldCreateDifferentNode(
    const PcpNodeRef& node, const std::string& newAssetPath)
{
    // Get any file format arguments that were originally used to open the
    // layer so we can apply them to the new asset path.
    const SdfLayerRefPtr& nodeRootLayer = 
        node.GetLayerStack()->GetIdentifier().rootLayer;
    
    std::string oldAssetPath;
    SdfLayer::FileFormatArguments oldArgs;
    if (!TF_VERIFY(SdfLayer::SplitIdentifier(
            nodeRootLayer->GetIdentifier(), &oldAssetPath, &oldArgs))) {
        return true;
    }

    // If no such layer is already open, this asset path must indicate a
    // layer that differs from the given node's root layer.
    const SdfLayerHandle newLayer = SdfLayer::Find(newAssetPath, oldArgs);
    if (!newLayer) {
        return true;
    }

    // Otherwise, if this layer differs from the given node's root layer,
    // this asset path would result in a different node during composition.
    return nodeRootLayer != newLayer;
}

template <PcpArcType RefOrPayload>
auto _GetSourceArcs(const PcpNodeRef& node, PcpSourceArcInfoVector* info);

template <>
auto _GetSourceArcs<PcpArcTypeReference>(
    const PcpNodeRef& node, PcpSourceArcInfoVector* info)
{
    SdfReferenceVector refs;
    PcpComposeSiteReferences(node, &refs, info);
    return refs;
}

template <>
auto _GetSourceArcs<PcpArcTypePayload>(
    const PcpNodeRef& node, PcpSourceArcInfoVector* info)
{
    SdfPayloadVector payloads;
    PcpComposeSitePayloads(node, &payloads, info);
    return payloads;
}

// Check the reference or payload arcs on the given node to determine if
// their asset paths now resolve to a different layer. See _EvalNodeReferences
// and _EvalNodePaylods.
template <PcpArcType RefOrPayload>
static bool
_NeedToRecomputeDueToAssetPathChange(
    const PcpNodeRef& node)
{
    auto arcRange = _GetDirectChildRange(node, RefOrPayload);
    if (arcRange.first != arcRange.second) {
        PcpSourceArcInfoVector sourceInfo;
        const auto sourceArcs = _GetSourceArcs<RefOrPayload>(node, &sourceInfo);
        TF_VERIFY(sourceArcs.size() == sourceInfo.size());

        const size_t numArcs = std::distance(arcRange.first, arcRange.second);
        if (numArcs != sourceArcs.size()) {
            // This could happen if there was some scene description
            // change that added/removed arcs, but also if a 
            // layer couldn't be opened when this index was computed. 
            // We conservatively mark this index as needing recomputation
            // in the latter case to simplify things.
            return true;
        }

        for (size_t i = 0; i < sourceArcs.size(); ++i, ++arcRange.first) {
            // Skip internal references/payloads since there's no asset path
            // computation that occurs when processing them.
            if (sourceArcs[i].GetAssetPath().empty()) {
                continue;
            }

            // PcpComposeSiteReferences/Payloads will have filled in each
            // object with the same asset path that would be used
            // during composition to open layers.
            const std::string& anchoredAssetPath = sourceArcs[i].GetAssetPath();

            if (_ComputedAssetPathWouldCreateDifferentNode(
                    *arcRange.first, anchoredAssetPath)) {
                return true;
            }
        }
    }

    return false;
}

bool
Pcp_NeedToRecomputeDueToAssetPathChange(const PcpPrimIndex& index)
{
    // Scan the index for any direct composition arcs that target another
    // layer. If any exist, try to determine if the asset paths that were
    // computed to load those layers would now target a different layer.
    // If so, this prim index needs to be recomputed to include that
    // new layer.
    for (const PcpNodeRef& node : index.GetNodeRange()) {
        if (!node.CanContributeSpecs()) {
            continue;
        }

        if (_NeedToRecomputeDueToAssetPathChange<PcpArcTypeReference>(node) ||
            _NeedToRecomputeDueToAssetPathChange<PcpArcTypePayload>(node)) {
            return true;
        }
    }

    return false;
}

////////////////////////////////////////////////////////////////////////
// Index Construction

static void 
_ConvertNodeForChild(
    PcpNodeRef node,
    const PcpPrimIndexInputs& inputs,
    bool isRoot=true)
{
    // Because the child site is at a deeper level of namespace than
    // the parent, there may no longer be any specs.
    if (node.HasSpecs()) {
        node.SetHasSpecs(PcpComposeSiteHasPrimSpecs(node));
    }

    // Inert nodes are just placeholders, so we can skip computing these
    // bits of information since these nodes shouldn't have any opinions to
    // contribute.
    if (!inputs.usd && !node.IsInert() && node.HasSpecs()) {
        // If the parent's permission is private, it will be inherited by the
        // child. Otherwise, we recompute it here.
        if (node.GetPermission() == SdfPermissionPublic) {
            node.SetPermission(PcpComposeSitePermission(node));
        }
        
        // If the parent had symmetry, it will be inherited by the child.
        // Otherwise, we recompute it here.
        if (!node.HasSymmetry()) {
            node.SetHasSymmetry(PcpComposeSiteHasSymmetry(node));
        }
    }

    // Arbitrary-order traversal.
    TF_FOR_ALL(child, Pcp_GetChildrenRange(node)) {
        _ConvertNodeForChild(*child, inputs, /*isRoot=*/false);
    }

    // Initial child nodes are always due to their parent, except the root node.
    if (!isRoot) {
        node.SetIsDueToAncestor(true);
    }

}

// Returns true if the given node can be culled, false otherwise.
//
// In general, a node can be culled if no descendant nodes contribute 
// opinions, i.e., no specs are found in that subtree. There are some 
// exceptions that are documented in the function.
static inline bool
_NodeCanBeCulled(
    const PcpNodeRef& node,
    const PcpLayerStackSite& rootSite)
{
    // Trivial case if this node has already been culled. 
    // This could happen if this node was culled ancestrally.
    if (node.IsCulled()) {
#ifdef PCP_DIAGNOSTIC_VALIDATION
        TF_VERIFY(!node.IsRootNode());
#endif // PCP_DIAGNOSTIC_VALIDATION
        return true;
    }

    // The root node of a prim index is never culled. If needed, this
    // node will be culled when attached to another prim index in _AddArc.
    if (node.IsRootNode()) {
        return false;
    }

    // We cannot cull any nodes that denote the addition of a new arc.
    // These nodes introduce dependencies and must be discoverable.
    // This usually isn't an issue -- arcs are generally added to sites
    // where prim specs exist, so even without this check these nodes
    // wouldn't be culled anyway. However, if an arc to a site with no prims
    // is added (e.g., a reference to a prim that doesn't exist), we need
    // to explicitly keep that around.
    if (node.GetDepthBelowIntroduction() == 0) {
        return false;
    }

    // XXX: The following are unfortunate cases where Pcp needs to keep
    //      around nodes it would otherwise cull solely for consumers in Csd.
    //      In theory, Csd would be able to generate this info by computing
    //      unculled prim indices as needed, but in these cases, that
    //      performance cost is too great.

    // Because of how Csd composes symmetry across namespace ancestors in a 
    // layer stack before composing across arcs, Pcp needs to keep around 
    // any node that directly OR ancestrally provides symmetry info.
    if (node.HasSymmetry()) {
        return false;
    }

    // CsdPrim::GetBases wants to return the path of all prims in the
    // composed scene from which this prim inherits opinions. To ensure
    // Csd has all the info it needs for this, Pcp has to avoid culling any
    // subroot prim inherit nodes in the root layer stack. To see why, consider:
    //
    // root layer stack      ref layer stack
    //                       /GlobalClass <--+ 
    //                                       | (root prim inh) 
    // /Model_1  (ref) ----> /Model    ------+
    //                        + SymArm <-+
    //                                   | (subroot prim inh)
    //                        + LArm   --+
    //
    // The prim index for /Model_1/LArm would normally have the inherit nodes 
    // for /GlobalClass/LArm and /Model_1/SymArm culled, as there are no specs
    // for either in the root layer stack. The nature of root classes implies
    // that, if no specs for /GlobalClass exist in the root layer, there is
    // no /GlobalClass in the composed scene. So, we don't have to protect
    // root prim inherits from being culled. However, because of referencing, 
    // the subroot inherit /Model_1/SymArm *does* exist in the composed scene.
    // So, we can't cull that node -- GetBases needs it.
    if (node.GetArcType() == PcpArcTypeInherit &&
        node.GetLayerStack() == rootSite.layerStack) {
        // We check the intro path of the origin node as there are cases where
        // a new implied inherit arc is created from an ancestral inherit 
        // which means it will be introduced from a subroot path even if the
        // original inherit node is a root prim path.
        const PcpNodeRef &originNode = 
            node.GetOriginNode() == node.GetParentNode() ? 
            node : node.GetOriginRootNode();
        if (!originNode.GetPathAtIntroduction().IsRootPrimPath()) {
            return false;
        }
    }

    // If any subtree beneath this node wasn't culled, we can't cull
    // this node either.
    TF_FOR_ALL(it, Pcp_GetChildrenRange(node)) {
        const PcpNodeRef& child = *it;
        if (!child.IsCulled()) {
            return false;
        }
    }

    // If this node contributes any opinions, we can't cull it.
    if (node.HasSpecs() && node.CanContributeSpecs())
        return false;

    return true;
}

// Helper that recursively culls subtrees at and under the given node.
static void
_CullSubtreesWithNoOpinions(
    PcpNodeRef node,
    const PcpLayerStackSite& rootSite)
{
    // Recurse and attempt to cull all children first. Order doesn't matter.
    TF_FOR_ALL(child, Pcp_GetChildrenRange(node)) {
        // XXX: 
        // We propagate and maintain duplicate node structure in the graph
        // for specializes arcs, so when we cull we need to ensure we do so
        // in both places consistently. For simplicity, we're going to skip
        // this for now and not cull beneath any specializes arcs.
        if (PcpIsSpecializeArc(child->GetArcType())) {
            continue;
        }

        _CullSubtreesWithNoOpinions(*child, rootSite);
    }

    // Now, mark this node as culled if we can. These nodes will be
    // removed from the prim index at the end of prim indexing.
    if (_NodeCanBeCulled(node, rootSite)) {
        node.SetCulled(true);
    }
}

// Helper that sets any nodes that cannot have overrides on name children
// as inert.
struct Pcp_DisableNonInstanceableNodesVisitor
{
    bool Visit(PcpNodeRef node, bool nodeIsInstanceable)
    {
        if (!nodeIsInstanceable) {
            node.SetInert(true);
            return true;
        }
        return false;
    }
};

const PcpPrimIndex &
Pcp_ComputePrimIndexWithCompatibleInputs(
    PcpCache &cache,
    const SdfPath & path, const PcpPrimIndexInputs &inputs,
    PcpErrorVector *allErrors) {
    return cache._ComputePrimIndexWithCompatibleInputs(path, inputs, allErrors);
}    

static void
_BuildInitialPrimIndexFromAncestor(
    const PcpLayerStackSite &site,
    const PcpLayerStackSite &rootSite,
    int ancestorRecursionDepth,
    PcpPrimIndex_StackFrame *previousFrame,
    bool evaluateImpliedSpecializes,
    bool rootNodeShouldContributeSpecs,
    const PcpPrimIndexInputs& inputs,
    PcpPrimIndexOutputs* outputs)
{
    bool ancestorIsInstanceable = false;

    // If we're asking for a prim index in the cache's layer stack and
    // we're not excluding anything from the prim index then ask the
    // cache for the prim index.  This will get it from the cache if
    // it's already there, and cache it and record dependencies if not.
    if (!previousFrame &&
        evaluateImpliedSpecializes &&
        inputs.cache->GetLayerStack() == site.layerStack &&
        inputs.cache->GetPrimIndexInputs().IsEquivalentTo(inputs)) {
        // Get prim index through our cache.  This ensures the lifetime
        // of layer stacks brought in by ancestors.
        const PcpPrimIndex& parentIndex =
            inputs.parentIndex ? *inputs.parentIndex :
            Pcp_ComputePrimIndexWithCompatibleInputs(
                *inputs.cache, site.path.GetParentPath(), inputs,
                &outputs->allErrors);

        // Clone the parent's graph..
        outputs->primIndex.SetGraph(
            PcpPrimIndex_Graph::New(parentIndex.GetGraph()));

        ancestorIsInstanceable = parentIndex.IsInstanceable();

        PCP_INDEXING_UPDATE(
            _GetOriginatingIndex(previousFrame, outputs),
            outputs->primIndex.GetRootNode(),
            "Retrieved index for <%s> from cache",
            site.path.GetParentPath().GetText());
    }
    else {
        // First build the prim index for the given site's parent.
        // Note that variants and payloads are always evaluated to ensure
        // ancestral opinions are picked up.
        const PcpLayerStackSite parentSite(site.layerStack,
                                           site.path.GetParentPath());

        Pcp_BuildPrimIndex(parentSite, parentSite,
                           ancestorRecursionDepth+1,
                           evaluateImpliedSpecializes,
                           /* Always pick up ancestral opinions from variants
                              evaluateVariants = */ true,
                           /* rootNodeShouldContributeSpecs = */ true,
                           previousFrame, inputs, outputs);

        ancestorIsInstanceable = 
            Pcp_PrimIndexIsInstanceable(outputs->primIndex);
    }

    // If the ancestor graph is an instance, mark every node that cannot
    // have opinions about name children as inert. This will cause any
    // opinions in restricted locations to be ignored.
    if (ancestorIsInstanceable) {
        Pcp_DisableNonInstanceableNodesVisitor visitor;
        Pcp_TraverseInstanceableStrongToWeak(outputs->primIndex, &visitor);
    }

    // Adjust the parent graph for this child.
    const PcpPrimIndex_GraphRefPtr &graph = outputs->primIndex.GetGraph();
    graph->AppendChildNameToAllSites(site.path);

    // Reset the 'has payload' flag on this prim index.
    // This flag should only be set when a prim introduces a payload,
    // not when any of its parents introduced a payload. 
    // Also reset the payload state in the outputs for the same reason.
    //
    // XXX: 
    // Updating the graph's payload flag may cause a new copy of the prim 
    // index graph to be created, which is wasteful if this graph will
    // later set the flag back to its original value. It would be
    // better to defer setting this bit until we have the final
    // answer.
    graph->SetHasPayloads(false);
    outputs->payloadState = PcpPrimIndexOutputs::NoPayload;

    PcpNodeRef rootNode = outputs->primIndex.GetRootNode();
    _ConvertNodeForChild(rootNode, inputs);

    if (inputs.cull) {
        _CullSubtreesWithNoOpinions(rootNode, rootSite);
    }

    // Force the root node to inert if the caller has specified that the
    // root node should not contribute specs. Note that the node
    // may already be set to inert when applying instancing restrictions
    // above.
    if (!rootNodeShouldContributeSpecs) {
        rootNode.SetInert(true);
    }

    PCP_INDEXING_UPDATE(
        _GetOriginatingIndex(previousFrame, outputs),
        rootNode,
        "Adjusted ancestral index for %s", site.path.GetName().c_str());
}

static void
Pcp_BuildPrimIndex(
    const PcpLayerStackSite & site,
    const PcpLayerStackSite& rootSite,
    int ancestorRecursionDepth,
    bool evaluateImpliedSpecializes,
    bool evaluateVariants,
    bool rootNodeShouldContributeSpecs,
    PcpPrimIndex_StackFrame *previousFrame,
    const PcpPrimIndexInputs& inputs,
    PcpPrimIndexOutputs* outputs )
{
    Pcp_PrimIndexingDebug debug(&outputs->primIndex,
                                _GetOriginatingIndex(previousFrame, outputs),
                                site);

    // We only index prims (including the pseudo-root) or variant-selection
    // paths, and only with absolute paths.
    if (!TF_VERIFY(site.path.IsAbsolutePath() &&
                   (site.path.IsAbsoluteRootOrPrimPath() ||
                    site.path.IsPrimVariantSelectionPath()),
                   "%s", site.path.GetText())) {
        return;
    }

    // Establish initial PrimIndex contents.
    if (site.path.GetPathElementCount() == 0) {
        // Base case for the pseudo-root: just use the single site.
        outputs->primIndex.SetGraph(PcpPrimIndex_Graph::New(site, inputs.usd));
        // Even though the pseudo root spec exists implicitly, don't
        // assume that here.
        PcpNodeRef node = outputs->primIndex.GetGraph()->GetRootNode();
        node.SetHasSpecs(PcpComposeSiteHasPrimSpecs(node));
        // Optimization: Since no composition arcs can live on the
        // pseudo-root, we can return early.
        return;
    } else if (site.path.IsPrimVariantSelectionPath()) {
        // For variant selection paths, unlike regular prim paths, we do not
        // recurse on the parent to obtain ancestral opinions. This is
        // because variant arcs are evaluated in the process of evaluating
        // the parent path site, which will already account for ancestral
        // opinions about the variant itself.
        outputs->primIndex.SetGraph(PcpPrimIndex_Graph::New(site, inputs.usd));

        PcpNodeRef node = outputs->primIndex.GetGraph()->GetRootNode();
        node.SetHasSpecs(PcpComposeSiteHasPrimSpecs(node));
        node.SetInert(!rootNodeShouldContributeSpecs);
    } else {
        // Start by building and cloning the namespace parent's index.
        // This is to account for ancestral opinions: references and
        // other arcs introduced by namespace ancestors that might
        // contribute opinions to this child.
        _BuildInitialPrimIndexFromAncestor(
            site, rootSite, ancestorRecursionDepth, previousFrame,
            evaluateImpliedSpecializes,
            rootNodeShouldContributeSpecs,
            inputs, outputs);
    }

    // Initialize the task list.
    Pcp_PrimIndexer indexer(inputs, outputs, rootSite, ancestorRecursionDepth,
                            previousFrame, evaluateImpliedSpecializes,
                            evaluateVariants);
    indexer.AddTasksForRootNode(outputs->primIndex.GetRootNode());

    // Process task list.
    bool tasksAreLeft = true;
    while (tasksAreLeft) {
        Task task = indexer.PopTask();
        switch (task.type) {
        case Task::Type::EvalNodeRelocations:
            _EvalNodeRelocations(&outputs->primIndex, task.node, &indexer);
            break;
        case Task::Type::EvalImpliedRelocations:
            _EvalImpliedRelocations(&outputs->primIndex, task.node, &indexer);
            break;
        case Task::Type::EvalNodeReferences:
            _EvalNodeReferences(&outputs->primIndex, task.node, &indexer);
            break;
        case Task::Type::EvalNodePayload:
            _EvalNodePayloads(&outputs->primIndex, task.node, &indexer);
            break;
        case Task::Type::EvalNodeInherits:
            _EvalNodeInherits(&outputs->primIndex, task.node, &indexer);
            break;
        case Task::Type::EvalImpliedClasses:
            _EvalImpliedClasses(&outputs->primIndex, task.node, &indexer);
            break;
        case Task::Type::EvalNodeSpecializes:
            _EvalNodeSpecializes(&outputs->primIndex, task.node, &indexer);
            break;
        case Task::Type::EvalImpliedSpecializes:
            _EvalImpliedSpecializes(&outputs->primIndex, task.node, &indexer);
            break;
        case Task::Type::EvalNodeVariantSets:
            _EvalNodeVariantSets(&outputs->primIndex, task.node, &indexer);
            break;
        case Task::Type::EvalNodeVariantAuthored:
            _EvalNodeAuthoredVariant(&outputs->primIndex, task.node, &indexer,
                                     task.vsetName, task.vsetNum);
            break;
        case Task::Type::EvalNodeVariantFallback:
            _EvalNodeFallbackVariant(&outputs->primIndex, task.node, &indexer,
                                     task.vsetName, task.vsetNum);
            break;
        case Task::Type::EvalNodeVariantNoneFound:
            // No-op.  These tasks are just markers for RetryVariantTasks().
            break;
        case Task::Type::None:
            tasksAreLeft = false;
            break;
        }
    }
}

void
PcpComputePrimIndex(
    const SdfPath& primPath,
    const PcpLayerStackPtr& layerStack,
    const PcpPrimIndexInputs& inputs,
    PcpPrimIndexOutputs* outputs,
    ArResolver* resolver)
{
    TfAutoMallocTag2 tag("Pcp", "PcpComputePrimIndex");

    TRACE_FUNCTION();

    if (!(primPath.IsAbsolutePath()                 &&
             (primPath.IsAbsoluteRootOrPrimPath()   ||
              primPath.IsPrimVariantSelectionPath()))) {
        TF_CODING_ERROR("Path <%s> must be an absolute path to a prim, "
                        "a prim variant-selection, or the pseudo-root.",
                        primPath.GetText());
        return;
    }

    ArResolverContextBinder binder(
        resolver ? resolver : &ArGetResolver(),
        layerStack->GetIdentifier().pathResolverContext);

    const PcpLayerStackSite site(layerStack, primPath);
    Pcp_BuildPrimIndex(site, site,
                       /* ancestorRecursionDepth = */ 0,
                       /* evaluateImpliedSpecializes = */ true,
                       /* evaluateVariants = */ true,
                       /* rootNodeShouldContributeSpecs = */ true,
                       /* previousFrame = */ NULL,
                       inputs, outputs);

    // Tag each node that's not allowed to contribute prim specs due to 
    // permissions. Note that we do this as a post-processing pass here, 
    // but not in Pcp_BuildPrimIndex(), which gets called recursively above.
    // We don't actually need to *enforce* permissions until after the node 
    // graph has been built. While it's being built, we only need to make
    // sure each node's permission is set correctly, which is done in
    // _AddArc() and _ConvertNodeForChild(). So we can defer calling
    // _EnforcePermissions() until the very end, which saves us from
    // doing some redundant work.
    if (!inputs.usd) {
        _EnforcePermissions(&outputs->primIndex, &outputs->allErrors);
    }

    // Determine whether this prim index is instanceable and store that
    // information in the prim index. This requires composed metadata
    // values, so we do this here after the prim index is fully composed
    // instead of in Pcp_BuildPrimIndex.
    outputs->primIndex.GetGraph()->SetIsInstanceable(
        Pcp_PrimIndexIsInstanceable(outputs->primIndex));

    // We're done modifying the graph, so finalize it.
    outputs->primIndex.GetGraph()->Finalize();

    // Collect the prim stack and the node for each prim in the stack.
    // Also collect all prim specs found in any node -- this is different
    // from the prim stack when nodes don't contribute prim specs.
    //
    // Note that we *must* do this after the graph is finalized, as 
    // finalization will cause outstanding PcpNodeRefs to be invalidated.
    Pcp_RescanForSpecs(&outputs->primIndex, inputs.usd,
                       /* updateHasSpecs */false );
}

////////////////////////////////////////////////////////////////////////
// Name children / property names

// Walk the graph, strong-to-weak, composing prim child names.
// Account for spec children in each layer, list-editing statements,
// and relocations.
static void
_ComposePrimChildNamesAtNode(
    const PcpPrimIndex& primIndex,
    const PcpNodeRef& node,
    bool usd,
    TfTokenVector *nameOrder,
    PcpTokenSet *nameSet,
    PcpTokenSet *prohibitedNameSet)
{
    if (!usd) {
        // Apply relocations from just this layer stack.
        // Classify them into three groups:  names to add, remove, or replace.
        std::set<TfToken> namesToAdd, namesToRemove;
        std::map<TfToken, TfToken> namesToReplace;

        // Check for relocations with a child as source.
        // See _EvalNodeRelocations for why we use the incremental relocates.
        const SdfRelocatesMap & relocatesSourceToTarget =
            node.GetLayerStack()->GetIncrementalRelocatesSourceToTarget();
        for (SdfRelocatesMap::const_iterator i =
                 relocatesSourceToTarget.lower_bound(node.GetPath());
             i != relocatesSourceToTarget.end() &&
                 i->first.HasPrefix(node.GetPath()); ++i) {
            const SdfPath & oldPath = i->first;
            const SdfPath & newPath = i->second;

            if (oldPath.GetParentPath() == node.GetPath()) {
                if (newPath.GetParentPath() == node.GetPath()) {
                    // Target is the same parent, so this is a rename.
                    namesToReplace[oldPath.GetNameToken()] =
                        newPath.GetNameToken();
                } else {
                    // Target is not the same parent, so this is remove.
                    namesToRemove.insert(oldPath.GetNameToken());
                }
                // The source name is now prohibited.
                prohibitedNameSet->insert(oldPath.GetNameToken());
            }
        }

        // Check for relocations with a child as target.
        // See _EvalNodeRelocations for why we use the incremental relocates.
        const SdfRelocatesMap & relocatesTargetToSource =
            node.GetLayerStack()->GetIncrementalRelocatesTargetToSource();
        for (SdfRelocatesMap::const_iterator i =
                 relocatesTargetToSource.lower_bound(node.GetPath());
             i != relocatesTargetToSource.end() &&
                 i->first.HasPrefix(node.GetPath()); ++i) {
            const SdfPath & newPath = i->first;
            const SdfPath & oldPath = i->second;

            if (newPath.GetParentPath() == node.GetPath()) {
                if (oldPath.GetParentPath() == node.GetPath()) {
                    // Source is the same parent, so this is a rename.
                    // We will have already handled this above.
                } else {
                    // Source is not the same parent, so this is an add.
                    if (nameSet->find(newPath.GetNameToken()) ==
                        nameSet->end()) {
                        namesToAdd.insert(newPath.GetNameToken());
                    }
                }
            }
        }

        // Apply the names to replace or remove.
        if (!namesToReplace.empty() || !namesToRemove.empty()) {
            // Do one pass, building a list of names to retain.
            TfTokenVector namesToRetain;
            namesToRetain.reserve( nameOrder->size() );
            TF_FOR_ALL(name, *nameOrder) {
                std::map<TfToken, TfToken>::const_iterator i =
                    namesToReplace.find(*name);
                if (i != namesToReplace.end()) {
                    // This name was replaced.
                    const TfToken & newName = i->second;
                    nameSet->erase(*name);
                
                    // Check if newName is already in the nameSet before adding
                    // it to the new name order.  newName may already be in
                    // the nameSet (and nameOrder) if it was contributed by
                    // a child spec from a weaker node.
                    //
                    // This can happen when a relocation renames X to Y and
                    // there is also a child spec for Y across a reference.
                    // The intended behavior of the relocation arc is that
                    // that "shadow" child Y is silently ignored.  PcpPrimIndex
                    // already ignores it when composing Y, but we also need
                    // to check for it here, when composing the child names
                    // for Y's parent.  See TrickyMultipleRelocations for a
                    // test that exercises this.
                    //
                    // TODO: Although silently ignoring the duplicate
                    // name is consistent with Csd's behavior, which we want
                    // to preserve for the initial Pcp work, we think this
                    // should perhaps be reported as a composition error,
                    // since the relocation arc is introducing a name collision.
                    //
                    if (nameSet->insert(newName).second) {
                        // Retain the new name in the same position as the
                        // old name.
                        namesToRetain.push_back(newName);
                    }
                } else if (namesToRemove.find(*name) == namesToRemove.end()) {
                    // Retain this name as-is.
                    namesToRetain.push_back(*name);
                } else {
                    // Do not retain this name.
                    nameSet->erase(*name);
                }
            }
            nameOrder->swap(namesToRetain);
        }

        // Append children relocated to under this prim in lexicographic order.
        //
        // Semantics note: We use alphabetical order as a default ordering
        // because there is no required statement of ordering among prims
        // relocated here.  (We will, however, subsequently apply 
        // re-ordering restatements in this site's layer stack.)
        //
        nameOrder->insert(nameOrder->end(), namesToAdd.begin(),
                          namesToAdd.end());
        nameSet->insert(namesToAdd.begin(), namesToAdd.end());
    }

    // Compose the site's local names over the current result.
    if (node.CanContributeSpecs()) {
        PcpComposeSiteChildNames(
            node.GetLayerStack()->GetLayers(), node.GetPath(), 
            SdfChildrenKeys->PrimChildren, nameOrder, nameSet,
            &SdfFieldKeys->PrimOrder);
    }

    // Post-conditions, for debugging.
    // Disabled by default to avoid extra overhead.
#ifdef PCP_DIAGNOSTIC_VALIDATION
    TF_VERIFY(nameSet->size() == nameOrder->size());
    TF_VERIFY(*nameSet == PcpTokenSet(nameOrder->begin(), nameOrder->end()));
#endif // PCP_DIAGNOSTIC_VALIDATION
}

static void
_ComposePrimChildNames( const PcpPrimIndex& primIndex,
                        const PcpNodeRef& node,
                        bool usd,
                        TfTokenVector *nameOrder,
                        PcpTokenSet *nameSet,
                        PcpTokenSet *prohibitedNameSet )
{
    if (node.IsCulled()) {
        return;
    }

    // Reverse strength-order traversal (weak-to-strong).
    TF_REVERSE_FOR_ALL(child, Pcp_GetChildrenRange(node)) {
        _ComposePrimChildNames(primIndex, *child, usd,
                               nameOrder, nameSet, prohibitedNameSet);
    }

    _ComposePrimChildNamesAtNode(
        primIndex, node, usd, nameOrder, nameSet, prohibitedNameSet);
}

// Helper struct for _ComposePrimChildNamesForInstance, see comments
// below.
struct Pcp_PrimChildNameVisitor
{
    Pcp_PrimChildNameVisitor( const PcpPrimIndex& primIndex,
                              bool usd,
                              TfTokenVector *nameOrder,
                              PcpTokenSet *nameSet,
                              PcpTokenSet *prohibitedNameSet )
        : _primIndex(primIndex)
        , _usd(usd)
        , _nameOrder(nameOrder)
        , _nameSet(nameSet)
        , _prohibitedNameSet(prohibitedNameSet)
    {
    }

    void Visit(PcpNodeRef node, bool nodeIsInstanceable)
    {
        if (nodeIsInstanceable) {
            _ComposePrimChildNamesAtNode(
                _primIndex, node, _usd,
                _nameOrder, _nameSet, _prohibitedNameSet);
        }
    }

private:
    const PcpPrimIndex& _primIndex;
    bool _usd;
    TfTokenVector* _nameOrder;
    PcpTokenSet* _nameSet;
    PcpTokenSet* _prohibitedNameSet;
};

static void
_ComposePrimChildNamesForInstance( const PcpPrimIndex& primIndex,
                                   bool usd,
                                   TfTokenVector *nameOrder,
                                   PcpTokenSet *nameSet,
                                   PcpTokenSet *prohibitedNameSet )
{
    Pcp_PrimChildNameVisitor visitor(
        primIndex, usd, nameOrder, nameSet, prohibitedNameSet);
    Pcp_TraverseInstanceableWeakToStrong(primIndex, &visitor);
}

static void
_ComposePrimPropertyNames( const PcpPrimIndex& primIndex,
                           const PcpNodeRef& node,
                           bool isUsd,
                           TfTokenVector *nameOrder,
                           PcpTokenSet *nameSet )
{
    if (node.IsCulled()) {
        return;
    }

    // Reverse strength-order traversal (weak-to-strong).
    TF_REVERSE_FOR_ALL(child, Pcp_GetChildrenRange(node)) {
        _ComposePrimPropertyNames(
            primIndex, *child, isUsd, nameOrder, nameSet );
    }

    // Compose the site's local names over the current result.
    if (node.CanContributeSpecs()) {
        PcpComposeSiteChildNames(
            node.GetLayerStack()->GetLayers(), node.GetPath(), 
            SdfChildrenKeys->PropertyChildren, nameOrder, nameSet,
            isUsd ? nullptr : &SdfFieldKeys->PropertyOrder);
    }
}

void
PcpPrimIndex::ComputePrimChildNames( TfTokenVector *nameOrder,
                                     PcpTokenSet *prohibitedNameSet ) const
{
    if (!_graph) {
        return;
    }

    TRACE_FUNCTION();

    // Provide a set with any existing nameOrder contents.
    PcpTokenSet nameSet(nameOrder->begin(), nameOrder->end());

    // Walk the graph to compose prim child names.
    if (IsInstanceable()) {
        _ComposePrimChildNamesForInstance(
            *this, IsUsd(),
            nameOrder, &nameSet, prohibitedNameSet);
    }
    else {
        _ComposePrimChildNames(
            *this, GetRootNode(), IsUsd(),
            nameOrder, &nameSet, prohibitedNameSet);
    }

    // Remove prohibited names from the composed prim child names.
    if (!prohibitedNameSet->empty()) {
        nameOrder->erase(
            std::remove_if(nameOrder->begin(), nameOrder->end(),
                [prohibitedNameSet](const TfToken& name) {
                    return prohibitedNameSet->find(name) 
                        != prohibitedNameSet->end();
                }),
            nameOrder->end());
    }
}

void
PcpPrimIndex::ComputePrimPropertyNames( TfTokenVector *nameOrder ) const
{
    if (!_graph) {
        return;
    }

    TRACE_FUNCTION();

    // Provide a set with any existing nameOrder contents.
    PcpTokenSet nameSet;
    nameSet.insert(nameOrder->begin(), nameOrder->end());

    // Walk the graph to compose prim child names.
    _ComposePrimPropertyNames(
        *this, GetRootNode(), IsUsd(), nameOrder, &nameSet);
}

PXR_NAMESPACE_CLOSE_SCOPE
