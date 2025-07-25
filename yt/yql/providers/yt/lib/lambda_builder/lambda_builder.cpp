#include "lambda_builder.h"

#include <yql/essentials/utils/yql_panic.h>
#include <yql/essentials/minikql/mkql_opt_literal.h>
#include <yql/essentials/minikql/mkql_program_builder.h>
#include <yql/essentials/minikql/mkql_node_serialization.h>
#include <yql/essentials/minikql/comp_nodes/mkql_factories.h>

#include <util/generic/strbuf.h>
#include <util/system/env.h>

namespace NYql {

using namespace NCommon;
using namespace NNodes;
using namespace NKikimr;
using namespace NKikimr::NMiniKQL;

TLambdaBuilder::TLambdaBuilder(const NKikimr::NMiniKQL::IFunctionRegistry* functionRegistry,
        NKikimr::NMiniKQL::TScopedAlloc& alloc,
        const NKikimr::NMiniKQL::TTypeEnvironment* env,
        const TIntrusivePtr<IRandomProvider>& randomProvider,
        const TIntrusivePtr<ITimeProvider>& timeProvider,
        NKikimr::NMiniKQL::IStatsRegistry* jobStats,
        NKikimr::NUdf::ICountersProvider* counters,
        const NKikimr::NUdf::ISecureParamsProvider* secureParamsProvider,
        const NKikimr::NUdf::ILogProvider* logProvider,
        TLangVersion langVer)
    : FunctionRegistry(functionRegistry)
    , Alloc(alloc)
    , RandomProvider(randomProvider)
    , TimeProvider(timeProvider)
    , JobStats(jobStats)
    , Counters(counters)
    , SecureParamsProvider(secureParamsProvider)
    , LogProvider(logProvider)
    , LangVer(langVer)
    , Env(env)
{
}

TLambdaBuilder::~TLambdaBuilder() {
}

void TLambdaBuilder::SetExternalEnv(const NKikimr::NMiniKQL::TTypeEnvironment* env) {
    Env = env;
}

const NKikimr::NMiniKQL::TTypeEnvironment* TLambdaBuilder::CreateTypeEnv() const {
    YQL_ENSURE(!EnvPtr);
    EnvPtr = std::make_shared<NKikimr::NMiniKQL::TTypeEnvironment>(Alloc);
    return EnvPtr.get();
}

TRuntimeNode TLambdaBuilder::BuildLambda(const IMkqlCallableCompiler& compiler, const TExprNode::TPtr& lambdaNode,
    TExprContext& exprCtx, TArgumentsMap&& arguments) const
{
    TProgramBuilder pgmBuilder(GetTypeEnvironment(), *FunctionRegistry, false, LangVer);
    TMkqlBuildContext ctx(compiler, pgmBuilder, exprCtx, lambdaNode->UniqueId(), std::move(arguments));
    return MkqlBuildExpr(*lambdaNode, ctx);
}

TRuntimeNode TLambdaBuilder::TransformAndOptimizeProgram(NKikimr::NMiniKQL::TRuntimeNode root,
    TCallableVisitFuncProvider funcProvider) {
    TExploringNodeVisitor explorer;
    explorer.Walk(root.GetNode(), GetTypeEnvironment().GetNodeStack());
    bool wereChanges = false;
    TRuntimeNode program = SinglePassVisitCallables(root, explorer, funcProvider, GetTypeEnvironment(), true, wereChanges);
    program = LiteralPropagationOptimization(program, GetTypeEnvironment(), true);
    return program;
}

THolder<IComputationGraph> TLambdaBuilder::BuildGraph(
    const NKikimr::NMiniKQL::TComputationNodeFactory& factory,
    NUdf::EValidateMode validateMode,
    NUdf::EValidatePolicy validatePolicy,
    const TString& optLLVM,
    NKikimr::NMiniKQL::EGraphPerProcess graphPerProcess,
    TExploringNodeVisitor& explorer,
    TRuntimeNode root) const
{
    return BuildGraph(factory, validateMode, validatePolicy, optLLVM, graphPerProcess, explorer, root, {root.GetNode()});
}

std::tuple<
    THolder<NKikimr::NMiniKQL::IComputationGraph>,
    TIntrusivePtr<IRandomProvider>,
    TIntrusivePtr<ITimeProvider>
>
TLambdaBuilder::BuildLocalGraph(
    const NKikimr::NMiniKQL::TComputationNodeFactory& factory,
    NUdf::EValidateMode validateMode,
    NUdf::EValidatePolicy validatePolicy,
    const TString& optLLVM,
    NKikimr::NMiniKQL::EGraphPerProcess graphPerProcess,
    TExploringNodeVisitor& explorer,
    TRuntimeNode root) const
{
    auto randomProvider = RandomProvider;
    auto timeProvider = TimeProvider;
    if (GetEnv(TString("YQL_DETERMINISTIC_MODE"))) {
        randomProvider = CreateDeterministicRandomProvider(1);
        timeProvider = CreateDeterministicTimeProvider(10000000);
    }

    return std::make_tuple(BuildGraph(factory, validateMode, validatePolicy, optLLVM, graphPerProcess, explorer, root, {root.GetNode()},
        randomProvider, timeProvider), randomProvider, timeProvider);
}

class TComputationGraphProxy: public IComputationGraph {
public:
    TComputationGraphProxy(IComputationPattern::TPtr&& pattern, THolder<IComputationGraph>&& graph)
        : Pattern(std::move(pattern))
        , Graph(std::move(graph))
    {}

    void Prepare() final {
        Graph->Prepare();
    }
    NUdf::TUnboxedValue GetValue() final {
        return Graph->GetValue();
    }
    TComputationContext& GetContext() final {
        return Graph->GetContext();
    }
    IComputationExternalNode* GetEntryPoint(size_t index, bool require) override {
        return Graph->GetEntryPoint(index, require);
    }
    const TArrowKernelsTopology* GetKernelsTopology() override {
        return Graph->GetKernelsTopology();
    }
    const TComputationNodePtrDeque& GetNodes() const final {
        return Graph->GetNodes();
    }
    void Invalidate() final {
        return Graph->Invalidate();
    }
    void InvalidateCaches() final {
        return Graph->InvalidateCaches();
    }
    TMemoryUsageInfo& GetMemInfo() const final {
        return Graph->GetMemInfo();
    }
    const THolderFactory& GetHolderFactory() const final {
        return Graph->GetHolderFactory();
    }
    ITerminator* GetTerminator() const final {
        return Graph->GetTerminator();
    }
    bool SetExecuteLLVM(bool value) final {
        return Graph->SetExecuteLLVM(value);
    }
    TString SaveGraphState() final {
        return Graph->SaveGraphState();
    }
    void LoadGraphState(TStringBuf state) final {
        Graph->LoadGraphState(state);
    }
private:
    IComputationPattern::TPtr Pattern;
    THolder<IComputationGraph> Graph;
};


THolder<IComputationGraph> TLambdaBuilder::BuildGraph(
    const NKikimr::NMiniKQL::TComputationNodeFactory& factory,
    NUdf::EValidateMode validateMode,
    NUdf::EValidatePolicy validatePolicy,
    const TString& optLLVM,
    NKikimr::NMiniKQL::EGraphPerProcess graphPerProcess,
    TExploringNodeVisitor& explorer,
    TRuntimeNode& root,
    std::vector<NKikimr::NMiniKQL::TNode*>&& entryPoints,
    TIntrusivePtr<IRandomProvider> randomProvider,
    TIntrusivePtr<ITimeProvider> timeProvider) const
{
    if (!randomProvider) {
        randomProvider = RandomProvider;
    }

    if (!timeProvider) {
        timeProvider = TimeProvider;
    }

    TString serialized;

    TComputationPatternOpts patternOpts(Alloc.Ref(), GetTypeEnvironment());
    patternOpts.SetOptions(factory, FunctionRegistry, validateMode, validatePolicy,
        optLLVM, graphPerProcess, JobStats, Counters,
        SecureParamsProvider, LogProvider, LangVer);
    auto preparePatternFunc = [&]() {
        if (serialized) {
            auto tupleRunTimeNodes = DeserializeRuntimeNode(serialized, GetTypeEnvironment());
            auto tupleNodes = static_cast<const TTupleLiteral*>(tupleRunTimeNodes.GetNode());
            root = tupleNodes->GetValue(0);
            for (size_t index = 0; index < entryPoints.size(); ++index) {
                entryPoints[index] = tupleNodes->GetValue(1 + index).GetNode();
            }
        }
        explorer.Walk(root.GetNode(), GetTypeEnvironment().GetNodeStack());
        auto pattern = MakeComputationPattern(explorer, root, entryPoints, patternOpts);
        for (const auto& node : explorer.GetNodes()) {
            node->SetCookie(0);
        }
        return pattern;
    };

    auto pattern = preparePatternFunc();
    YQL_ENSURE(pattern);

    const TComputationOptsFull computeOpts(JobStats, Alloc.Ref(), GetTypeEnvironment(), *randomProvider, *timeProvider,
        validatePolicy, SecureParamsProvider, Counters, LogProvider, LangVer);
    auto graph = pattern->Clone(computeOpts);
    return MakeHolder<TComputationGraphProxy>(std::move(pattern), std::move(graph));
}

TRuntimeNode TLambdaBuilder::MakeTuple(const TVector<TRuntimeNode>& items) const {
    TProgramBuilder pgmBuilder(GetTypeEnvironment(), *FunctionRegistry, false, LangVer);
    return pgmBuilder.NewTuple(items);
}

TRuntimeNode TLambdaBuilder::Deserialize(const TString& code) {
    return DeserializeRuntimeNode(code, GetTypeEnvironment());
}

std::pair<TString, size_t> TLambdaBuilder::Serialize(TRuntimeNode rootNode) {
    TExploringNodeVisitor explorer;
    explorer.Walk(rootNode.GetNode(), GetTypeEnvironment().GetNodeStack());
    TString code = SerializeRuntimeNode(explorer, rootNode, GetTypeEnvironment().GetNodeStack());
    size_t nodes = explorer.GetNodes().size();
    return std::make_pair(code, nodes);
}

TRuntimeNode TLambdaBuilder::UpdateLambdaCode(TString& code, size_t& nodes, TCallableVisitFuncProvider funcProvider) {
    TRuntimeNode rootNode = Deserialize(code);
    rootNode = TransformAndOptimizeProgram(rootNode, funcProvider);
    std::tie(code, nodes) = Serialize(rootNode);
    return rootNode;
}

TGatewayLambdaBuilder::TGatewayLambdaBuilder(
    const NKikimr::NMiniKQL::IFunctionRegistry* functionRegistry,
    NKikimr::NMiniKQL::TScopedAlloc& alloc,
    const NKikimr::NMiniKQL::TTypeEnvironment* env,
    const TIntrusivePtr<IRandomProvider>& randomProvider,
    const TIntrusivePtr<ITimeProvider>& timeProvider,
    NKikimr::NMiniKQL::IStatsRegistry* jobStats,
    NKikimr::NUdf::ICountersProvider* counters,
    const NKikimr::NUdf::ISecureParamsProvider* secureParamsProvider,
    const NKikimr::NUdf::ILogProvider* logProvider,
    TLangVersion langVer)
    : TLambdaBuilder(functionRegistry, alloc, env, randomProvider, timeProvider, jobStats, counters, secureParamsProvider, logProvider, langVer)
{
}

TString TGatewayLambdaBuilder::BuildLambdaWithIO(const TString& prefix, const IMkqlCallableCompiler& compiler, TCoLambda lambda, TExprContext& exprCtx) {
    TProgramBuilder pgmBuilder(GetTypeEnvironment(), *FunctionRegistry);
    TArgumentsMap arguments(1U);
    if (lambda.Args().Size() > 0) {
        const auto arg = lambda.Args().Arg(0);
        const auto argType = arg.Ref().GetTypeAnn();
        auto inputItemType = NCommon::BuildType(arg.Ref(), *GetSeqItemType(argType), pgmBuilder);
        switch (bool isStream = true; argType->GetKind()) {
        case ETypeAnnotationKind::Flow:
            if (ETypeAnnotationKind::Multi == argType->Cast<TFlowExprType>()->GetItemType()->GetKind()) {
                arguments.emplace(arg.Raw(), TRuntimeNode(TCallableBuilder(GetTypeEnvironment(), prefix + "Input", pgmBuilder.NewFlowType(inputItemType)).Build(), false));
                break;
            }
            isStream = false;
            [[fallthrough]]; // AUTOGENERATED_FALLTHROUGH_FIXME
        case ETypeAnnotationKind::Stream: {
            auto inputStream = pgmBuilder.SourceOf(isStream ?
                pgmBuilder.NewStreamType(pgmBuilder.GetTypeEnvironment().GetTypeOfVoidLazy()) : pgmBuilder.NewFlowType(pgmBuilder.GetTypeEnvironment().GetTypeOfVoidLazy()));

            inputItemType = pgmBuilder.NewOptionalType(inputItemType);
            inputStream = pgmBuilder.Map(inputStream, [&] (TRuntimeNode item) {
                TCallableBuilder inputCall(GetTypeEnvironment(), prefix + "Input", inputItemType);
                inputCall.Add(item);
                return TRuntimeNode(inputCall.Build(), false);
            });
            inputStream = pgmBuilder.TakeWhile(inputStream, [&] (TRuntimeNode item) {
                return pgmBuilder.Exists(item);
            });

            inputStream = pgmBuilder.FlatMap(inputStream, [&] (TRuntimeNode item) {
                return item;
            });

            arguments[arg.Raw()] = inputStream;
            break;
        }
        default:
            YQL_ENSURE(false, "Unsupported lambda argument type: " << arg.Ref().GetTypeAnn()->GetKind());
        }
    }
    TMkqlBuildContext ctx(compiler, pgmBuilder, exprCtx, lambda.Ref().UniqueId(), std::move(arguments));
    TRuntimeNode outStream = MkqlBuildExpr(lambda.Body().Ref(), ctx);
    if (outStream.GetStaticType()->IsFlow()) {
        TCallableBuilder outputCall(GetTypeEnvironment(), prefix + "Output", pgmBuilder.NewFlowType(GetTypeEnvironment().GetTypeOfVoidLazy()));
        outputCall.Add(outStream);
        outStream = TRuntimeNode(outputCall.Build(), false);
    } else {
        outStream = pgmBuilder.Map(outStream, [&] (TRuntimeNode item) {
            TCallableBuilder outputCall(GetTypeEnvironment(), prefix + "Output", GetTypeEnvironment().GetTypeOfVoidLazy());
            outputCall.Add(item);
            return TRuntimeNode(outputCall.Build(), false);
        });
    }
    outStream = pgmBuilder.Discard(outStream);

    return SerializeRuntimeNode(outStream, GetTypeEnvironment());
}

} // namespace NYql
