#include <ydb/library/actors/async/wait_for_event.h>

#include "common.h"
#include <ydb/library/actors/async/cancellation.h>
#include <ydb/library/actors/async/timeout.h>

namespace NAsyncTest {

    Y_UNIT_TEST_SUITE(WaitForEvent) {

        Y_UNIT_TEST(EventDelivered) {
            TVector<TString> sequence;

            TAsyncTestActor::TState state;
            TAsyncTestActorRuntime runtime;

            auto handler = [&](IEventHandle::TPtr& ev) {
                sequence.push_back(TStringBuilder() << "received event " << Hex(ev->GetTypeRewrite()));
                return true;
            };

            auto actor = runtime.StartAsyncActor(state, [&](auto*) -> async<void> {
                sequence.push_back("started");
                Y_DEFER { sequence.push_back("finished"); };

                auto ev = co_await ActorWaitForEvent<TEvents::TEvWakeup>(123);

                sequence.push_back("returning");
            }, handler);

            ASYNC_ASSERT_SEQUENCE(sequence, "started");

            // Wrong cookie doesn't cause ActorWaitForEvent to resume
            actor.Receive(new TEvents::TEvWakeup, 120);
            ASYNC_ASSERT_SEQUENCE(sequence, "received event 0x00010002");

            // Correct cookie but wrong event type doesn't cause ActorWaitForEvent to resume
            actor.Receive(new TEvents::TEvGone, 123);
            ASYNC_ASSERT_SEQUENCE(sequence, "received event 0x0001000D");

            // Correct cookie and event type cause ActorWaitForEvent to resume
            actor.Receive(new TEvents::TEvWakeup, 123);
            ASYNC_ASSERT_SEQUENCE(sequence, "returning", "finished");
        }

        Y_UNIT_TEST(SameCookiePreservesTypeMatchingAndRegistrationOrder) {
            TVector<TString> sequence;
            TAsyncTestActor::TState state;
            TAsyncTestActorRuntime runtime;

            auto actor = runtime.StartAsyncActor(state, [&](auto*) -> async<void> {
                co_await ActorWaitForEvent<TEvents::TEvWakeup>(123);
                sequence.push_back("first wakeup");
            }, [&](IEventHandle::TPtr&) {
                sequence.push_back("unhandled");
                return true;
            });

            actor.RunAsync([&]() -> async<void> {
                co_await ActorWaitForEvent<TEvents::TEvGone>(123);
                sequence.push_back("gone");
            });
            actor.RunAsync([&]() -> async<void> {
                co_await ActorWaitForEvent<TEvents::TEvWakeup>(123);
                sequence.push_back("second wakeup");
            });
            actor.RunAsync([&]() -> async<void> {
                auto ev = co_await ActorWaitForEvent<IEventHandle>(123);
                UNIT_ASSERT_VALUES_EQUAL(ev->GetTypeRewrite(), ui32(TEvents::TEvWakeup::EventType));
                sequence.push_back("any event");
            });
            actor.RunAsync([&]() -> async<void> {
                co_await ActorWaitForEvent<TEvents::TEvWakeup>(123);
                sequence.push_back("last wakeup");
            });

            actor.Receive(new TEvents::TEvGone, 123);
            ASYNC_ASSERT_SEQUENCE(sequence, "gone");
            actor.Receive(new TEvents::TEvWakeup, 123);
            ASYNC_ASSERT_SEQUENCE(sequence, "first wakeup");
            actor.Receive(new TEvents::TEvWakeup, 123);
            ASYNC_ASSERT_SEQUENCE(sequence, "second wakeup");
            actor.Receive(new TEvents::TEvWakeup, 123);
            ASYNC_ASSERT_SEQUENCE(sequence, "any event");
            actor.Receive(new TEvents::TEvWakeup, 123);
            ASYNC_ASSERT_SEQUENCE(sequence, "last wakeup");
            actor.Receive(new TEvents::TEvWakeup, 123);
            ASYNC_ASSERT_SEQUENCE(sequence, "unhandled");
        }

        class TRehashWaitActor : public TAsyncTestActor {
        public:
            TRehashWaitActor(TState& state, TVector<ui64>& completed)
                : TAsyncTestActor(state)
                , Completed(completed)
            {}

            void Wait(ui64 cookie) {
                auto ev = co_await ActorWaitForEvent<TEvents::TEvWakeup>(cookie);
                Completed.push_back(ev->Cookie);
            }

            void Rearm(ui64 cookie, size_t otherWaiters) {
                auto ev = co_await ActorWaitForEvent<TEvents::TEvWakeup>(cookie);
                Completed.push_back(ev->Cookie);
                // These root handlers register their waits immediately, before the
                // current matched-event callback returns to IActor::Receive.
                for (size_t i = 1; i <= otherWaiters; ++i) {
                    Wait(cookie + i);
                }
                ev = co_await ActorWaitForEvent<TEvents::TEvWakeup>(cookie);
                Completed.push_back(ev->Cookie);
            }

        private:
            TVector<ui64>& Completed;
        };

        Y_UNIT_TEST(MatchedContinuationCanRehashAndRearmSameCookie) {
            TVector<ui64> completed;
            TAsyncTestActor::TState state;
            TAsyncTestActorRuntime runtime;
            auto* self = new TRehashWaitActor(state, completed);
            TAsyncTestActorRuntime::TAsyncActorOperations actor(runtime, runtime.Register(self));
            actor.Step();

            constexpr ui64 cookie = 123;
            constexpr size_t otherWaiters = 256;
            actor.RunSync([&] { self->Rearm(cookie, otherWaiters); });
            actor.Receive(new TEvents::TEvWakeup, cookie);
            UNIT_ASSERT_VALUES_EQUAL(completed.size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(completed.back(), cookie);
            actor.Receive(new TEvents::TEvWakeup, cookie);
            UNIT_ASSERT_VALUES_EQUAL(completed.size(), 2);
            UNIT_ASSERT_VALUES_EQUAL(completed.back(), cookie);
            for (size_t i = 1; i <= otherWaiters; ++i) {
                actor.Receive(new TEvents::TEvWakeup, cookie + i);
                UNIT_ASSERT_VALUES_EQUAL(completed.size(), i + 2);
                UNIT_ASSERT_VALUES_EQUAL(completed.back(), cookie + i);
            }
            actor.Poison();
            UNIT_ASSERT(state.Destroyed);
        }

        Y_UNIT_TEST(MatchedContinuationCanCancelAnotherWaiterWithSameCookie) {
            TAsyncCancellationScope scope;
            bool matched = false;
            bool cancelled = false;
            size_t unhandled = 0;
            TAsyncTestActor::TState state;
            TAsyncTestActorRuntime runtime;

            auto actor = runtime.StartAsyncActor(state, [&](auto*) -> async<void> {
                co_await ActorWaitForEvent<TEvents::TEvWakeup>(123);
                scope.Cancel();
                matched = true;
            }, [&](IEventHandle::TPtr&) {
                ++unhandled;
                return true;
            });
            actor.RunAsync([&]() -> async<void> {
                const bool success = co_await scope.Wrap([]() -> async<void> {
                    ASYNC_ASSERT_NO_RETURN(co_await ActorWaitForEvent<TEvents::TEvGone>(123));
                });
                UNIT_ASSERT(!success);
                cancelled = true;
            });

            actor.Receive(new TEvents::TEvWakeup, 123);
            UNIT_ASSERT(matched);
            UNIT_ASSERT(cancelled);
            UNIT_ASSERT_VALUES_EQUAL(unhandled, 0);
            actor.Receive(new TEvents::TEvGone, 123);
            UNIT_ASSERT_VALUES_EQUAL(unhandled, 1);
        }

        Y_UNIT_TEST(MatchedContinuationCanPassAway) {
            bool resumed = false;
            TAsyncTestActor::TState state;
            TAsyncTestActorRuntime runtime;
            auto actor = runtime.StartAsyncActor(state, [&](auto* self) -> async<void> {
                co_await ActorWaitForEvent<TEvents::TEvWakeup>(123);
                self->PassAway();
                resumed = true;
            });

            actor.Receive(new TEvents::TEvWakeup, 123);
            UNIT_ASSERT(resumed);
            UNIT_ASSERT(state.Destroyed);
        }

        Y_UNIT_TEST(Cancel) {
            TVector<TString> sequence;

            TAsyncTestActor::TState state;
            TAsyncTestActorRuntime runtime;

            auto actor = runtime.StartAsyncActor(state, [&](auto*) -> async<void> {
                sequence.push_back("started");
                Y_DEFER { sequence.push_back("finished"); };

                ASYNC_ASSERT_NO_RETURN(co_await ActorWaitForEvent<TEvents::TEvWakeup>(123));
            });

            ASYNC_ASSERT_SEQUENCE(sequence, "started");

            actor.Poison();
            ASYNC_ASSERT_SEQUENCE(sequence, "finished");
        }

        Y_UNIT_TEST(CancelledAlready) {
            TVector<TString> sequence;

            TAsyncTestActor::TState state;
            TAsyncTestActorRuntime runtime;

            auto actor = runtime.StartAsyncActor(state, [&](auto* self) -> async<void> {
                sequence.push_back("started");
                Y_DEFER { sequence.push_back("finished"); };

                self->PassAway();

                ASYNC_ASSERT_NO_RETURN(co_await ActorWaitForEvent<TEvents::TEvWakeup>(123));
            });

            ASYNC_ASSERT_SEQUENCE(sequence, "started", "finished");
        }

        Y_UNIT_TEST(CancelThenDeliver) {
            TVector<TString> sequence;
            TAsyncCancellationScope scope;

            TAsyncTestActor::TState state;
            TAsyncTestActorRuntime runtime;

            auto handler = [&](IEventHandle::TPtr& ev) {
                sequence.push_back(TStringBuilder() << "received event " << Hex(ev->GetTypeRewrite()));
                return true;
            };

            auto actor = runtime.StartAsyncActor(state, [&](auto*) -> async<void> {
                sequence.push_back("started");
                Y_DEFER { sequence.push_back("finished"); };

                bool success = co_await scope.Wrap([&]() -> async<void> {
                    Y_DEFER { sequence.push_back("callback finished"); };
                    sequence.push_back("waiting");
                    ASYNC_ASSERT_NO_RETURN(co_await ActorWaitForEvent<TEvents::TEvWakeup>(123));
                });

                if (!success) {
                    sequence.push_back("callback was cancelled");
                }
            }, handler);

            ASYNC_ASSERT_SEQUENCE(sequence, "started", "waiting");

            actor.RunSync([&]{ scope.Cancel(); });
            ASYNC_ASSERT_SEQUENCE(sequence, "callback finished", "callback was cancelled", "finished");

            actor.Receive(new TEvents::TEvWakeup, 123);
            ASYNC_ASSERT_SEQUENCE(sequence, "received event 0x00010002");
        }


        Y_UNIT_TEST(AllocatedCookiesAreUniqueAndNonZero) {
            THashSet<ui64> seen;
            for (int i = 0; i < 1000; ++i) {
                const ui64 cookie = AllocateWaitCookie();
                UNIT_ASSERT(cookie != 0);
                UNIT_ASSERT(cookie >> 63);
                UNIT_ASSERT(seen.insert(cookie).second);
            }
        }

        Y_UNIT_TEST(AllocatedCookieRoundTrip) {
            TVector<TString> sequence;
            ui64 cookie = 0;

            TAsyncTestActor::TState state;
            TAsyncTestActorRuntime runtime;

            auto actor = runtime.StartAsyncActor(state, [&](auto*) -> async<void> {
                Y_DEFER { sequence.push_back("finished"); };

                cookie = AllocateWaitCookie();
                auto ev = co_await ActorWaitForEvent<TEvents::TEvWakeup>(cookie);
                UNIT_ASSERT_VALUES_EQUAL(ev->Cookie, cookie);

                sequence.push_back("returning");
            });

            UNIT_ASSERT(cookie != 0);
            actor.Receive(new TEvents::TEvWakeup, cookie);
            ASYNC_ASSERT_SEQUENCE(sequence, "returning", "finished");
        }


        class TEchoPeer : public TActor<TEchoPeer> {
        public:
            struct TRecorded {
                size_t Count = 0;
                ui32 Type = 0;
                ui64 Cookie = 0;
                TActorId Sender;
            };

            TEchoPeer(TRecorded& recorded, bool reply = true)
                : TActor(&TThis::StateWork)
                , Recorded(recorded)
                , Reply(reply)
            {}

            STFUNC(StateWork) {
                ++Recorded.Count;
                Recorded.Type = ev->GetTypeRewrite();
                Recorded.Cookie = ev->Cookie;
                Recorded.Sender = ev->Sender;
                if (Reply) {
                    Send(ev->Sender, new TEvents::TEvWakeup, 0, ev->Cookie);
                }
            }

        private:
            TRecorded& Recorded;
            const bool Reply;
        };

        Y_UNIT_TEST(ActorRequestRoundTrip) {
            TVector<TString> sequence;
            TEchoPeer::TRecorded recorded;

            TAsyncTestActor::TState state;
            TAsyncTestActorRuntime runtime;

            auto peer = runtime.Register(new TEchoPeer(recorded));

            auto handler = [&](IEventHandle::TPtr& ev) {
                sequence.push_back(TStringBuilder() << "received event " << Hex(ev->GetTypeRewrite()));
                return true;
            };

            auto actor = runtime.StartAsyncActor(state, [&](auto* self) -> async<void> {
                sequence.push_back("started");
                Y_DEFER { sequence.push_back("finished"); };

                auto reply = co_await ActorRequest<TEvents::TEvWakeup>(peer, new TEvents::TEvGone);

                UNIT_ASSERT_VALUES_EQUAL(reply->Cookie, recorded.Cookie);
                UNIT_ASSERT_VALUES_EQUAL(reply->Sender, peer);
                UNIT_ASSERT_VALUES_EQUAL(recorded.Sender, self->SelfId());
                sequence.push_back("returning");
            }, handler);

            ASYNC_ASSERT_SEQUENCE(sequence, "started");

            runtime.DispatchEvents();
            ASYNC_ASSERT_SEQUENCE(sequence, "returning", "finished");
            UNIT_ASSERT_VALUES_EQUAL(recorded.Count, 1u);
            UNIT_ASSERT_VALUES_EQUAL(recorded.Type, ui32(TEvents::TEvGone::EventType));
            UNIT_ASSERT(recorded.Cookie != 0);
            UNIT_ASSERT(!state.Destroyed);
        }

        Y_UNIT_TEST(ActorRequestTimeoutThenLateReply) {
            TVector<TString> sequence;
            TEchoPeer::TRecorded recorded;

            TAsyncTestActor::TState state;
            TAsyncTestActorRuntime runtime;

            // the peer records the request but never replies
            auto peer = runtime.Register(new TEchoPeer(recorded, /* reply */ false));

            auto handler = [&](IEventHandle::TPtr& ev) {
                sequence.push_back(TStringBuilder() << "received event " << Hex(ev->GetTypeRewrite()));
                return true;
            };

            auto actor = runtime.StartAsyncActor(state, [&](auto*) -> async<void> {
                sequence.push_back("started");
                Y_DEFER { sequence.push_back("finished"); };

                // the documented way to bound an ActorRequest
                auto reply = co_await WithTimeout(TDuration::MilliSeconds(10), [&]() -> async<TEvents::TEvWakeup::TPtr> {
                    co_return co_await ActorRequest<TEvents::TEvWakeup>(peer, new TEvents::TEvGone);
                });

                if (!reply) {
                    sequence.push_back("timeout");
                    co_return;
                }
                sequence.push_back("reply");
            }, handler);

            ASYNC_ASSERT_SEQUENCE(sequence, "started");

            runtime.SimulateSleep(TDuration::MilliSeconds(15));
            ASYNC_ASSERT_SEQUENCE(sequence, "timeout", "finished");
            UNIT_ASSERT_VALUES_EQUAL(recorded.Count, 1u);
            UNIT_ASSERT(!state.Destroyed);

            // a reply after the timeout is no longer intercepted and reaches the state function
            actor.Receive(new TEvents::TEvWakeup, recorded.Cookie);
            ASYNC_ASSERT_SEQUENCE(sequence, "received event 0x00010002");
        }

        Y_UNIT_TEST(ActorRequestUndeliveredWithTrackDelivery) {
            TVector<TString> sequence;

            TAsyncTestActor::TState state;
            TAsyncTestActorRuntime runtime;

            auto handler = [&](IEventHandle::TPtr& ev) {
                sequence.push_back(TStringBuilder() << "received event " << Hex(ev->GetTypeRewrite()));
                return true;
            };

            auto actor = runtime.StartAsyncActor(state, [&](auto*) -> async<void> {
                sequence.push_back("started");
                Y_DEFER { sequence.push_back("finished"); };

                auto reply = co_await ActorRequest<TEvents::TEvUndelivered>(
                    TActorId(0, "nobody"), new TEvents::TEvWakeup, IEventHandle::FlagTrackDelivery);

                UNIT_ASSERT_VALUES_EQUAL(reply->Get()->SourceType, ui32(TEvents::TEvWakeup::EventType));
                sequence.push_back("returning");
            }, handler);

            ASYNC_ASSERT_SEQUENCE(sequence, "started");

            runtime.DispatchEvents();
            ASYNC_ASSERT_SEQUENCE(sequence, "returning", "finished");
            UNIT_ASSERT(!state.Destroyed);
        }

    } // Y_UNIT_TEST_SUITE(WaitForEvent)

} // namespace NAsyncTest
