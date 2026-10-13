#ifndef MOD_HS_CLOUD_H
#define MOD_HS_CLOUD_H

#include "hs_queue.h" // HsReplyRequest

#include <functional>

// Cloud facts (Claude/PLAN-CLOUD-FACTS.md). A real player asks a bot something
// that is not about WoW; a cloud model (HearthsideChat.Cloud.Model, Claude
// Haiku by default) supplies one fact sentence or "unknown", never the reply.
// The fact rides in the request back to the queue worker, which puts it in the
// prompt under kHsCloudFactPrefix, and the tuned model voices it like any RAG
// block. Every failure -- feature off, limit reached, timeout, "unknown", a
// reply in the wrong shape -- leaves cloudFact empty, which is exactly the
// reply the bot would have given before this feature existed.
//
// Bots never reach the cloud. hs_handler.cpp sets cloudAccountId only for a
// real player's own chat, Hs_TryEnqueue re-checks the sender on the world
// thread, and the cloud thread refuses (logging an error) any request that is
// not a plain player reply. Three independent checks, so a later code path
// that forgets one still cannot send a bot's line out.

// The heading the fact goes under in the reply prompt. Not the RAG block's
// "Things you know about Azeroth: ": the fact is not about Azeroth, and the
// plain heading measured better (48/60 vs 42/60, Tests/offtopic_relay_probe.py).
// Not yet in the training rows: see the plan's follow-ups.
constexpr char kHsCloudFactPrefix[] = "Things you know: ";

// Loads unexpired saved answers from hside_cloud_fact and starts the cloud
// thread. World thread, once.
void Hs_CloudStartup();
void Hs_CloudShutdown();

// Queue worker thread. True when this request should go to the cloud before
// it is answered: the feature is on and has a key, the request passes the
// structural fail-safe, the line is a fact question (hs_cloud_fact.h), and the
// router (Cloud.Router) says it is not about the game. With Router = model
// that last test is a call to the tuned model in its ROUTE mode, made on this
// thread so the GPU still sees one request at a time.
bool Hs_CloudWantsRequest(HsReplyRequest const& req);

// Queue worker thread. Hands the request to the cloud thread; `resume` is
// called exactly once, from that thread (or inline, if the cloud queue is full
// or shut down), with cloudDone set and cloudFact filled when there is a fact.
void Hs_CloudSubmit(HsReplyRequest req, std::function<void(HsReplyRequest)> resume);

#endif // MOD_HS_CLOUD_H
