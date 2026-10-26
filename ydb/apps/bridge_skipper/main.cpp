#include <ydb/apps/bridge_skipper/config/config.h>
#include <ydb/apps/bridge_skipper/http/http.h>
#include <ydb/apps/bridge_skipper/runtime/controller.h>
#include <ydb/apps/bridge_skipper/runtime/gateway.h>
#include <ydb/apps/bridge_skipper/runtime/resources.h>

#include <ydb/library/actors/core/executor_pool_basic.h>
#include <ydb/library/actors/core/scheduler_basic.h>
#include <ydb/library/actors/http/http_proxy.h>
#include <ydb/library/actors/interconnect/poller/poller_actor.h>

#include <library/cpp/json/json_writer.h>
#include <library/cpp/svnversion/svnversion.h>
#include <ydb/apps/bridge_skipper/domain/clock.h>
#include <util/stream/output.h>
#include <util/system/yassert.h>

#include <chrono>
#include <csignal>
#include <thread>

namespace NBridgeSkipper {
namespace {

volatile std::sig_atomic_t StopRequested = 0;

void OnSignal(int) {
    StopRequested = 1;
}

int Run(const TConfig& config, const TEntrypointArguments& arguments, const TLoadedCredentials& credentials) {
    using namespace NActors;
    auto resources = std::make_shared<TRuntimeResources>(config, arguments.StateDir, arguments.MemberId);
    std::shared_ptr<ISdkGateway> sdk;
    THolder<TActorSystem> system;
    try {
        resources->Log("startup", {{"member_id", arguments.MemberId}, {"profile", TString(EvidenceProfileName(config.EvidenceProfile))},
            {"build", GetProgramCommitId()}, {"status", "production_auto_unqualified"},
            {"access", IsLoopbackHttpListen(config.Http.Listen) ? TString("loopback") : TString("public-port-authorizes-all-routes")},
            {"plaintext_token_opt_in", credentials.PlaintextTokenOptIn ? TString("true") : TString("false")}});
        sdk = CreateSdkGateway(config, credentials);
        auto setup = MakeHolder<TActorSystemSetup>();
        setup->NodeId = 1;
        setup->ExecutorsCount = 2;
        setup->Executors.Reset(new TAutoPtr<IExecutorPool>[2]);
        setup->Executors[0] = new TBasicExecutorPool(0, 2, 50, "skipper-control");
        setup->Executors[1] = new TBasicExecutorPool(1, 2, 50, "skipper-http");
        setup->Scheduler = new TBasicSchedulerThread(NActors::TSchedulerConfig(512, 0));
        setup->LocalServices.emplace_back(MakePollerActorId(), TActorSetupCmd(CreatePollerActor(), TMailboxType::ReadAsFilled, 1));
        system = MakeHolder<TActorSystem>(setup);
        resources->CompletionGate->Attach(system.Get());
        system->Start();
        const auto gateway = system->Register(CreateGatewayActor(config, resources, sdk), TMailboxType::ReadAsFilled, 0);
        const auto controller = system->Register(CreateControllerActor(config, resources, gateway), TMailboxType::ReadAsFilled, 0);
        const auto proxy = system->Register(NHttp::CreateHttpProxy(), TMailboxType::ReadAsFilled, 1);
        system->Register(CreateHttpActor(config, resources, controller, proxy), TMailboxType::ReadAsFilled, 1);
        // Main may wait; control and HTTP workers never perform startup I/O.
        const auto endpoint = ParseEndpoint("grpc://" + config.Http.Listen);
        auto listen = MakeHolder<NHttp::TEvHttpProxy::TEvAddListeningPort>(endpoint.Port);
        listen->Address = endpoint.Host.StartsWith('[') ? TString(TStringBuf(endpoint.Host).substr(1, endpoint.Host.size() - 2)) : endpoint.Host;
        system->Ask<NHttp::TEvHttpProxy::TEvConfirmListen>(proxy, std::move(listen), TDuration::Seconds(10)).GetValueSync();
        while (!StopRequested) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        resources->CloseAdmission();
        system->Send(new IEventHandle(controller, {}, new TEvents::TEvPoison()));
        system->Send(new IEventHandle(gateway, {}, new TEvRuntime::TEvGatewayQuiesce()));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            const auto snapshot = resources->ReadSnapshot();
            if (resources->ControllerDraining.load() && resources->PendingStorage() == 0 && snapshot && snapshot->PendingCommands == 0
                    && snapshot->InFlightReads == 0 && !snapshot->AdmissionUnknown && resources->Counters->GetCounter("inflight_mutations")->Val() == 0) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    } catch (...) {
        resources->CloseAdmission();
        if (sdk) {
            sdk->Stop();
        }
        resources->StopStorage();
        resources->CompletionGate->Close();
        if (system) {
            system->Stop();
            system->Cleanup();
        }
        throw;
    }
    sdk->Stop();
    resources->StopStorage();
    resources->CompletionGate->Close();
    system->Stop();
    system->Cleanup();
    return 0;
}

int StartupFailed(TStringBuf message) {
    NJson::TJsonValue value;
    value["event"] = "startup_failed";
    value["level"] = "error";
    value["message"] = TString(message.substr(0, 1024));
    Cerr << NJson::WriteJson(value, false, true) << "\n";
    return 1;
}

} // namespace
} // namespace NBridgeSkipper

int main(int argc, char** argv) {
    using namespace NBridgeSkipper;
    TSkipperClock::RegisterProvider(CreateDefaultTimeProvider());
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
    std::signal(SIGPIPE, SIG_IGN);
    TEntrypointArguments arguments;
    TConfig config;
    TLoadedCredentials credentials;
    try {
        TVector<TString> raw;
        for (int i = 1; i < argc; ++i) {
            raw.emplace_back(argv[i]);
        }
        arguments = ParseEntrypointArguments(raw);
        if (arguments.Action == EEntrypointAction::Help) {
            Cout << EntrypointHelp();
            return 0;
        }
        if (arguments.Action == EEntrypointAction::Version) {
            Cout << "bridge_skipper build=" << GetProgramCommitId() << " api=1 schema=1 standalone\n";
            return 0;
        }
        config = LoadConfig(arguments.ConfigFile);
        credentials = ValidateStartup(config, arguments);
    } catch (const std::exception& error) {
        // Configuration and argument validation messages never include
        // credential contents or credential paths.
        return StartupFailed(error.what());
    }
    if (arguments.Action == EEntrypointAction::CheckConfig) {
        Cout << "configuration valid\n";
        return 0;
    }
    try {
        return Run(config, arguments, credentials);
    } catch (const std::exception&) {
        // Low-level exceptions may contain remote payloads.
        return StartupFailed("Check state binding, credentials and listener availability");
    }
}
