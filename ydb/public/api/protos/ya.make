PROTO_LIBRARY(api-protos)

MAVEN_GROUP_ID(com.yandex.ydb)

PEERDIR(
    ydb/public/api/protos/annotations
)

SRCS(
    draft/datastreams.proto
    draft/field_transformation.proto
    draft/fq.proto
    draft/persqueue_common.proto
    draft/persqueue_error_codes.proto
    draft/ydb_backup.proto
    draft/ydb_bridge.proto
    draft/ydb_dynamic_config.proto
    draft/ydb_logstore.proto
    draft/ydb_maintenance.proto
    draft/ydb_object_storage.proto
    draft/ydb_replication.proto
    draft/ydb_tablet.proto
    draft/ydb_view.proto
    draft/ymq.proto
    persqueue_error_codes_v1.proto
    ydb_auth.proto
    ydb_clickhouse_internal.proto
    ydb_cms.proto
    ydb_common.proto
    ydb_config.proto
    ydb_coordination.proto
    ydb_debug.proto
    ydb_discovery.proto
    ydb_export.proto
    ydb_federation_discovery.proto
    ydb_formats.proto
    ydb_import.proto
    ydb_issue_message.proto
    ydb_keyvalue.proto
    ydb_monitoring.proto
    ydb_operation.proto
    ydb_persqueue_cluster_discovery.proto
    ydb_persqueue_v1.proto
    ydb_query.proto
    ydb_query_stats.proto
    ydb_rate_limiter.proto
    ydb_scheme.proto
    ydb_scripting.proto
    ydb_status_codes.proto
    ydb_table.proto
    ydb_topic.proto
    ydb_value.proto
)

CPP_PROTO_PLUGIN0(validation ydb/public/lib/validation)

# .pb.h are only available in C++ variant of PROTO_LIBRARY
IF (MODULE_TAG == "CPP_PROTO")
    GENERATE_ENUM_SERIALIZATION(draft/persqueue_common.pb.h)
    GENERATE_ENUM_SERIALIZATION(ydb_persqueue_cluster_discovery.pb.h)
    GENERATE_ENUM_SERIALIZATION(draft/datastreams.pb.h)
    GENERATE_ENUM_SERIALIZATION(ydb_topic.pb.h)
ENDIF()

EXCLUDE_TAGS(GO_PROTO)

END()
