LIBRARY()

SRCS(
    group_stat_aggregator.cpp
    group_stat_aggregator.h
    distconf.cpp
    distconf.h
    distconf_binding.cpp
    distconf_bridge.cpp
    distconf_cache.cpp
    distconf_connectivity.cpp
    distconf_console.cpp
    distconf_dynamic.cpp
    distconf_generate.cpp
    distconf_fsm.cpp
    distconf_invoke.h
    distconf_invoke_bridge.cpp
    distconf_invoke_common.cpp
    distconf_invoke_state_storage.cpp
    distconf_invoke_static_group.cpp
    distconf_invoke_storage_config.cpp
    distconf_mon.cpp
    distconf_persistent_storage.cpp
    distconf_quorum.h
    distconf_scatter_gather.cpp
    distconf_selfheal.h
    distconf_selfheal.cpp
    distconf_statestorage_config_generator.h
    distconf_statestorage_config_generator.cpp
    distconf_validate.cpp
    node_warden.h
    node_warden_cache.cpp
    node_warden_events.h
    node_warden_group.cpp
    node_warden_group_resolver.cpp
    node_warden_impl.cpp
    node_warden_impl.h
    node_warden_mon.cpp
    node_warden_pdisk.cpp
    node_warden_pipe.cpp
    node_warden_proxy.cpp
    node_warden_resource.cpp
    node_warden_scrub.cpp
    node_warden_stat_aggr.cpp
    node_warden_vdisk.cpp
)

PEERDIR(
    library/cpp/json
    library/cpp/openssl/crypto
    ydb/core/base
    ydb/core/blob_depot/agent
    ydb/core/blobstorage/bridge/syncer
    ydb/core/blobstorage/common
    ydb/core/blobstorage/crypto
    ydb/core/blobstorage/dsproxy/bridge
    ydb/core/blobstorage/groupinfo
    ydb/core/blobstorage/pdisk
    ydb/core/control/lib
    ydb/library/pdisk_io
    ydb/library/yaml_config
    ydb/core/util/actorsys_test
)

END()

RECURSE_FOR_TESTS(
    ut
    ut_sequence
)
