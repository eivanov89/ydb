#include "operation_client.h"

#include <yt/yt/client/job_tracker_client/helpers.h>

#include <yt/yt/core/ytree/fluent.h>
#include <yt/yt/core/ytree/yson_struct.h>

#include <library/cpp/string_utils/base64/base64.h>

namespace NYT::NApi {

using namespace NYTree;
using namespace NJobTrackerClient;
using namespace NYson;

////////////////////////////////////////////////////////////////////////////////

void TListJobsContinuationTokenSerializer::Register(TRegistrar registrar)
{
    registrar.ExternalClassParameter("version", &TThat::Version)
        .Default(0)
        .DontSerializeDefault();

    registrar.ExternalBaseClassParameter("job_competition_id", &TListJobsOptions::JobCompetitionId)
        .Default()
        .DontSerializeDefault();

    registrar.ExternalBaseClassParameter("type", &TThat::Type)
        .Default()
        .DontSerializeDefault();

    registrar.ExternalBaseClassParameter("state", &TThat::State)
        .Default()
        .DontSerializeDefault();

    registrar.ExternalBaseClassParameter("address", &TThat::Address)
        .Default()
        .DontSerializeDefault();

    registrar.ExternalBaseClassParameter("with_stderr", &TThat::WithStderr)
        .Default()
        .DontSerializeDefault();

    registrar.ExternalBaseClassParameter("with_fail_context", &TThat::WithFailContext)
        .Default()
        .DontSerializeDefault();

    registrar.ExternalBaseClassParameter("with_spec", &TThat::WithSpec)
        .Default()
        .DontSerializeDefault();

    registrar.ExternalBaseClassParameter("with_competitors", &TThat::WithCompetitors)
        .Default()
        .DontSerializeDefault();

    registrar.ExternalBaseClassParameter("with_monitoring_gescriptor", &TThat::WithMonitoringDescriptor)
        .Default()
        .DontSerializeDefault();

    registrar.ExternalBaseClassParameter("with_interruption_info", &TThat::WithInterruptionInfo)
        .Default()
        .DontSerializeDefault();

    registrar.ExternalBaseClassParameter("task_name", &TThat::TaskName)
        .Default()
        .DontSerializeDefault();

    registrar.ExternalBaseClassParameter("operation_incarnation", &TThat::OperationIncarnation)
        .Default()
        .DontSerializeDefault();

    registrar.ExternalBaseClassParameter("from_time", &TThat::FromTime)
        .Default()
        .DontSerializeDefault();

    registrar.ExternalBaseClassParameter("to_time", &TThat::ToTime)
        .Default()
        .DontSerializeDefault();

    registrar.ExternalBaseClassParameter("attributes", &TThat::Attributes)
        .Default()
        .DontSerializeDefault();

    registrar.ExternalBaseClassParameter("running_jobs_lookbehind_period", &TThat::RunningJobsLookbehindPeriod)
        .Default(TDuration::Max())
        .DontSerializeDefault();

    registrar.ExternalBaseClassParameter("sort_field", &TThat::SortField)
        .Default()
        .DontSerializeDefault();

    registrar.ExternalBaseClassParameter("sort_order", &TThat::SortOrder)
        .Default()
        .DontSerializeDefault();

    registrar.ExternalBaseClassParameter("offset", &TThat::Offset)
        .Default(0)
        .DontSerializeDefault();

    registrar.ExternalBaseClassParameter("limit", &TThat::Limit)
        .Default(1000)
        .DontSerializeDefault();

    registrar.ExternalBaseClassParameter("include_archive", &TThat::IncludeArchive)
        .Default(false)
        .DontSerializeDefault();

    registrar.ExternalBaseClassParameter("include_cypress", &TThat::IncludeCypress)
        .Default(false)
        .DontSerializeDefault();

    registrar.ExternalBaseClassParameter("include_controller_agent", &TThat::IncludeControllerAgent)
        .Default(false)
        .DontSerializeDefault();
}

std::string EncodeNewToken(TListJobsOptions&& options, int jobCount)
{
    options.Offset += jobCount;
    options.ContinuationToken.reset();

    TListJobsContinuationToken token;
    static_cast<TListJobsOptions&>(token) = std::move(options);

    auto optionsYson = ConvertToYsonString(token);
    return Base64Encode(optionsYson.ToString());
}

TListJobsOptions DecodeListJobsOptionsFromToken(const std::string& continuationToken)
{
    auto optionsYson = TYsonString(Base64StrictDecode(continuationToken));
    return ConvertTo<TListJobsContinuationToken>(optionsYson);
}

void Serialize(
    const TOperation& operation,
    NYson::IYsonConsumer* consumer,
    bool needType,
    bool needOperationType,
    bool idWithAttributes)
{
    auto fillItems = [&] (TFluentMap fluent) {
        fluent
            .OptionalItem("id", operation.Id)
            .OptionalItem("state", operation.State)
            .DoIf(operation.Type.operator bool(), [&] (TFluentMap fluent) {
                if (needType) {
                    fluent.Item("type").Value(operation.Type);
                }
                if (needOperationType) {
                    fluent.Item("operation_type").Value(operation.Type);
                }
            })
            .OptionalItem("authenticated_user", operation.AuthenticatedUser)
            .OptionalItem("start_time", operation.StartTime)
            .OptionalItem("finish_time", operation.FinishTime)
            .OptionalItem("brief_progress", operation.BriefProgress)
            .OptionalItem("progress", operation.Progress)
            .OptionalItem("brief_spec", operation.BriefSpec)
            .OptionalItem("full_spec", operation.FullSpec)
            .OptionalItem("spec", operation.Spec)
            .OptionalItem("provided_spec", operation.ProvidedSpec)
            .OptionalItem("experiment_assignments", operation.ExperimentAssignments)
            .OptionalItem("experiment_assignment_names", operation.ExperimentAssignmentNames)
            .OptionalItem("unrecognized_spec", operation.UnrecognizedSpec)
            .OptionalItem("runtime_parameters", operation.RuntimeParameters)
            .OptionalItem("suspended", operation.Suspended)
            .OptionalItem("result", operation.Result)
            .OptionalItem("events", operation.Events)
            .OptionalItem("scheduling_attributes_per_pool_tree", operation.SchedulingAttributesPerPoolTree)
            // COMPAT(omgronny)
            .OptionalItem("slot_index_per_pool_tree", operation.SlotIndexPerPoolTree)
            .OptionalItem("alerts", operation.Alerts)
            .OptionalItem("alert_events", operation.AlertEvents)
            .OptionalItem("task_names", operation.TaskNames)
            .OptionalItem("controller_features", operation.ControllerFeatures)
            .DoIf(operation.OtherAttributes.operator bool(), [&] (TFluentMap fluent) {
                for (const auto& [key, value] : operation.OtherAttributes->ListPairs()) {
                    fluent.Item(key).Value(value);
                }
            });
    };

    if (idWithAttributes) {
        if (!operation.Id) {
            THROW_ERROR_EXCEPTION(
                "Cannot serialize operation in id-with-attributes format "
                "as \"id\" attribute is missing from attribute filter");
        }
        BuildYsonFluently(consumer)
            .BeginAttributes()
                .Do(fillItems)
            .EndAttributes()
            .Value(*operation.Id);
    } else {
        BuildYsonFluently(consumer)
            .BeginMap()
                .Do(fillItems)
            .EndMap();
    }
}

void Deserialize(TOperation& operation, NYTree::IAttributeDictionaryPtr attributes, bool clone)
{
    if (clone) {
        attributes = attributes->Clone();
    }

    auto setField = [&] (auto& field, const TString& name) {
        using T = std::remove_reference_t<decltype(field)>;
        if constexpr (std::is_same_v<T, NYson::TYsonString>) {
            if (auto value = attributes->FindYson(name)) {
                field = std::move(value);
                attributes->Remove(name);
            } else {
                field = {};
            }
        } else {
            using TValue = typename TOptionalTraits<T>::TValue;
            if (auto value = attributes->FindAndRemove<TValue>(name)) {
                field = std::move(value);
            } else {
                field.reset();
            }
        }
    };

    setField(operation.Id, "id");
    setField(operation.Type, "type");
    setField(operation.State, "state");
    setField(operation.StartTime, "start_time");
    setField(operation.FinishTime, "finish_time");
    setField(operation.AuthenticatedUser, "authenticated_user");
    setField(operation.BriefSpec, "brief_spec");
    setField(operation.Spec, "spec");
    setField(operation.ProvidedSpec, "provided_spec");
    setField(operation.ExperimentAssignments, "experiment_assignments");
    setField(operation.ExperimentAssignmentNames, "experiment_assignment_names");
    setField(operation.FullSpec, "full_spec");
    setField(operation.UnrecognizedSpec, "unrecognized_spec");
    setField(operation.BriefProgress, "brief_progress");
    setField(operation.Progress, "progress");
    setField(operation.RuntimeParameters, "runtime_parameters");
    setField(operation.Suspended, "suspended");
    setField(operation.Events, "events");
    setField(operation.Result, "result");
    setField(operation.SchedulingAttributesPerPoolTree, "scheduling_attributes_per_pool_tree");
    setField(operation.SlotIndexPerPoolTree, "slot_index_per_pool_tree");
    setField(operation.Alerts, "alerts");
    setField(operation.AlertEvents, "alert_events");
    setField(operation.TaskNames, "task_names");
    setField(operation.ControllerFeatures, "controller_features");

    operation.OtherAttributes = std::move(attributes);
}

////////////////////////////////////////////////////////////////////////////////

std::optional<EJobState> TJob::GetState() const
{
    if (ArchiveState && ControllerState) {
        if (IsJobInProgress(*ArchiveState)) {
            return ControllerState;
        } else {
            return ArchiveState;
        }
    } else if (ArchiveState) {
        return ArchiveState;
    } else if (ControllerState) {
        return ControllerState;
    }
    return std::nullopt;
}

// Tries to find "abort_reason" attribute in the error and parse it as |EAbortReason|.
// Returns |std::nullopt| if the attribute is not found or any of two parsings is unsuccessful.
static std::optional<NScheduler::EAbortReason> TryGetJobAbortReasonFromError(const NYson::TYsonString& errorYson)
{
    if (!errorYson) {
        return std::nullopt;
    }

    TError error;
    try {
        error = ConvertTo<TError>(errorYson);
    } catch (const std::exception& exception) {
        return std::nullopt;
    }

    try {
        return error.Attributes().Find<NScheduler::EAbortReason>("abort_reason");
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

void Serialize(const TJob& job, NYson::IYsonConsumer* consumer, TStringBuf idKey)
{
    NYTree::BuildYsonFluently(consumer)
        .BeginMap()
            .OptionalItem(idKey, job.Id)
            .OptionalItem("operation_id", job.OperationId)
            .OptionalItem("type", job.Type)
            .OptionalItem("state", job.GetState())
            .OptionalItem("controller_state", job.ControllerState)
            .OptionalItem("archive_state", job.ArchiveState)
            .OptionalItem("address", job.Address)
            .OptionalItem("addresses", job.Addresses)
            .OptionalItem("start_time", job.StartTime)
            .OptionalItem("finish_time", job.FinishTime)
            .OptionalItem("has_spec", job.HasSpec)
            .OptionalItem("job_competition_id", job.JobCompetitionId)
            .OptionalItem("probing_job_competition_id", job.ProbingJobCompetitionId)
            .OptionalItem("has_competitors", job.HasCompetitors)
            .OptionalItem("has_probing_competitors", job.HasProbingCompetitors)
            .OptionalItem("progress", job.Progress)
            .OptionalItem("stderr_size", job.StderrSize)
            .OptionalItem("fail_context_size", job.FailContextSize)
            .OptionalItem("error", job.Error)
            .OptionalItem("interruption_info", job.InterruptionInfo)
            .OptionalItem("abort_reason", TryGetJobAbortReasonFromError(job.Error))
            .OptionalItem("brief_statistics", job.BriefStatistics)
            .OptionalItem("input_paths", job.InputPaths)
            .OptionalItem("core_infos", job.CoreInfos)
            .OptionalItem("events", job.Events)
            .OptionalItem("statistics", job.Statistics)
            .OptionalItem("exec_attributes", job.ExecAttributes)
            .OptionalItem("task_name", job.TaskName)
            .OptionalItem("pool_tree", job.PoolTree)
            .OptionalItem("pool", job.Pool)
            .OptionalItem("monitoring_descriptor", job.MonitoringDescriptor)
            .OptionalItem("is_stale", job.IsStale)
            .OptionalItem("job_cookie", job.JobCookie)
            .OptionalItem("archive_features", job.ArchiveFeatures)
            .OptionalItem("operation_incarnation", job.OperationIncarnation)
            .OptionalItem("allocation_id", job.AllocationId)
            .OptionalItem("gang_rank", job.GangRank)
        .EndMap();
}

void Serialize(const TJobTraceEvent& traceEvent, NYson::IYsonConsumer* consumer)
{
    NYTree::BuildYsonFluently(consumer)
        .BeginMap()
            .Item("operation_id").Value(traceEvent.OperationId)
            .Item("job_id").Value(traceEvent.JobId)
            .Item("trace_id").Value(traceEvent.TraceId)
            .Item("event_index").Value(traceEvent.EventIndex)
            .Item("event").Value(traceEvent.Event)
            .Item("event_time").Value(traceEvent.EventTime.MicroSeconds())
        .EndMap();
}

void Serialize(const TOperationEvent& operationEvent, NYson::IYsonConsumer* consumer) {
    NYTree::BuildYsonFluently(consumer)
        .BeginMap()
            .Item("timestamp").Value(operationEvent.Timestamp)
            .Item("event_type").Value(operationEvent.EventType)
            .OptionalItem("incarnation", operationEvent.Incarnation)
            .OptionalItem("incarnation_switch_reason", operationEvent.IncarnationSwitchReason)
            .OptionalItem("incarnation_switch_info", operationEvent.IncarnationSwitchInfo)
        .EndMap();
}

////////////////////////////////////////////////////////////////////////////////

void TListOperationsAccessFilter::Register(TRegistrar registrar)
{
    registrar.Parameter("subject", &TThis::Subject);
    registrar.Parameter("permissions", &TThis::Permissions);
}

////////////////////////////////////////////////////////////////////////////////

TGetJobStderrResponse TGetJobStderrResponse::MakeJobStderr(const TSharedRef& data, const TGetJobStderrOptions& options)
{
    auto totalSize = std::ssize(data);
    auto endOffset = totalSize;
    auto offset = options.Offset.value_or(0);
    auto limit = options.Limit.value_or(0);

    if (!offset && !limit) {
        return {
            .Data = data,
            .TotalSize = totalSize,
            .EndOffset = endOffset,
        };
    };

    i64 firstPos = 0;
    if (offset > 0) {
        firstPos = offset;
    }

    if (firstPos >= std::ssize(data)) {
        return {
            .Data = TSharedRef{},
            .TotalSize = totalSize,
            .EndOffset = 0,
        };
    } else {
        auto lastPos = firstPos;
        if (limit > 0) {
            lastPos += limit;
        } else {
            lastPos += data.size();
        }
        if (lastPos > std::ssize(data)) {
            lastPos = data.size();
        }
        const auto dataCut = data.Slice(firstPos, lastPos);
        return {
            .Data = dataCut,
            .TotalSize = totalSize,
            .EndOffset = limit ? firstPos + std::ssize(dataCut) : endOffset,
        };
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NApi

