#pragma once
#include <util/generic/strbuf.h>
#include <ydb/library/actors/core/actorid.h>

namespace NKikimr::NKqp {

const TStringBuf DefaultKikimrPublicClusterName = "db";

TString EncodeSessionId(ui32 nodeId, const TString& id);

inline NActors::TActorId MakeKqpProxyID(ui32 nodeId, size_t n = 0) {
    static const char name[10][11] = {
        "kqp_proxy0",
        "kqp_proxy1",
        "kqp_proxy2",
        "kqp_proxy3",
        "kqp_proxy4",
        "kqp_proxy5",
        "kqp_proxy6",
        "kqp_proxy7",
        "kqp_proxy8",
        "kqp_proxy9",
    };

    if (n > 9) {
        n = 0;
    }

    return NActors::TActorId(nodeId, TStringBuf(name[n], 11));
}

inline NActors::TActorId MakeKqpCompileServiceID(ui32 nodeId) {
    const char name[12] = "kqp_compile";
    return NActors::TActorId(nodeId, TStringBuf(name, 12));
}

inline NActors::TActorId MakeKqpResourceManagerServiceID(ui32 nodeId) {
    const char name[12] = "kqp_resman";
    return NActors::TActorId(nodeId, TStringBuf(name, 12));
}

inline NActors::TActorId MakeKqpRmServiceID(ui32 nodeId) {
    const char name[12] = "kqp_rm";
    return NActors::TActorId(nodeId, TStringBuf(name, 12));
}

inline NActors::TActorId MakeKqpNodeServiceID(ui32 nodeId) {
    const char name[12] = "kqp_node";
    return NActors::TActorId(nodeId, TStringBuf(name, 12));
}

inline NActors::TActorId MakeKqpCompileComputationPatternServiceID(ui32 nodeId) {
    const char name[12] = "kqp_comp_cp";
    return NActors::TActorId(nodeId, TStringBuf(name, 12));
}

inline NActors::TActorId MakeKqpFinalizeScriptServiceId(ui32 nodeId) {
    const char name[12] = "kqp_sfinal";
    return NActors::TActorId(nodeId, TStringBuf(name, 12));
}

inline NActors::TActorId MakeKqpWorkloadServiceId(ui32 nodeId) {
    const char name[12] = "kqp_workld";
    return NActors::TActorId(nodeId, TStringBuf(name, 12));
}

} // namespace NKikimr::NKqp
