// Copyright (c) 2015-2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <zmq/zmqnotificationinterface.h>

#include <zmq/zmqpublishnotifier.h>
#include <zmq/zmqutil.h>

#include <zmq.h>

#include <validation.h>
#include <util/system.h>

CZMQNotificationInterface::CZMQNotificationInterface() : pcontext(nullptr)
{
}

CZMQNotificationInterface::~CZMQNotificationInterface()
{
    Shutdown();
}

std::list<const CZMQAbstractNotifier*> CZMQNotificationInterface::GetActiveNotifiers() const
{
    std::list<const CZMQAbstractNotifier*> result;
    for (const auto& n : notifiers) {
        result.push_back(n.get());
    }
    return result;
}

CZMQNotificationInterface* CZMQNotificationInterface::Create()
{
    std::map<std::string, CZMQNotifierFactory> factories;
    factories["pubhashblock"] = CZMQAbstractNotifier::Create<CZMQPublishHashBlockNotifier>;
    factories["pubhashtx"] = CZMQAbstractNotifier::Create<CZMQPublishHashTransactionNotifier>;
    factories["pubrawblock"] = CZMQAbstractNotifier::Create<CZMQPublishRawBlockNotifier>;
    factories["pubrawtx"] = CZMQAbstractNotifier::Create<CZMQPublishRawTransactionNotifier>;

    const std::vector<std::string> vTrackedGames = gArgs.GetArgs("-trackgame");
    std::unique_ptr<TrackedGames> trackedGames(new TrackedGames(vTrackedGames));

    ZMQGameBlocksNotifier* gameBlocksNotifier = nullptr;
    factories["pubgameblocks"] = [&trackedGames, &gameBlocksNotifier]() {
        assert (gameBlocksNotifier == nullptr);
        auto res = MakeUnique<ZMQGameBlocksNotifier>(*trackedGames);
        gameBlocksNotifier = res.get();
        return res;
    };

    factories["pubgamepending"] = [&trackedGames]() {
        return MakeUnique<ZMQGamePendingNotifier>(*trackedGames);
    };

    std::list<std::unique_ptr<CZMQAbstractNotifier>> notifiers;
    for (const auto& entry : factories)
    {
        std::string arg("-zmq" + entry.first);
        if (gArgs.IsArgSet(arg))
        {
            const auto& factory = entry.second;
            const std::string address = gArgs.GetArg(arg, "");
            std::unique_ptr<CZMQAbstractNotifier> notifier = factory();
            notifier->SetType(entry.first);
            notifier->SetAddress(address);
            notifier->SetOutboundMessageHighWaterMark(static_cast<int>(gArgs.GetArg(arg + "hwm", CZMQAbstractNotifier::DEFAULT_ZMQ_SNDHWM)));
            notifiers.push_back(std::move(notifier));
        }
    }

    if (!notifiers.empty())
    {
        std::unique_ptr<CZMQNotificationInterface> notificationInterface(new CZMQNotificationInterface());
        notificationInterface->trackedGames = std::move(trackedGames);
        notificationInterface->notifiers = std::move(notifiers);
        notificationInterface->gameBlocksNotifier = gameBlocksNotifier;

        if (notificationInterface->Initialize()) {
            return notificationInterface.release();
        }
    }

    return nullptr;
}

// Called at startup to conditionally set up ZMQ socket(s)
bool CZMQNotificationInterface::Initialize()
{
    int major = 0, minor = 0, patch = 0;
    zmq_version(&major, &minor, &patch);
    LogPrint(BCLog::ZMQ, "zmq: version %d.%d.%d\n", major, minor, patch);

    LogPrint(BCLog::ZMQ, "zmq: Initialize notification interface\n");
    assert(!pcontext);

    pcontext = zmq_ctx_new();

    if (!pcontext)
    {
        zmqError("Unable to initialize context");
        return false;
    }

    for (auto& notifier : notifiers) {
        if (notifier->Initialize(pcontext)) {
            LogPrint(BCLog::ZMQ, "zmq: Notifier %s ready (address = %s)\n", notifier->GetType(), notifier->GetAddress());
        } else {
            LogPrint(BCLog::ZMQ, "zmq: Notifier %s failed (address = %s)\n", notifier->GetType(), notifier->GetAddress());
            return false;
        }
    }

    return true;
}

// Called during shutdown sequence
void CZMQNotificationInterface::Shutdown()
{
    LogPrint(BCLog::ZMQ, "zmq: Shutdown notification interface\n");
    if (pcontext)
    {
        for (auto& notifier : notifiers) {
            LogPrint(BCLog::ZMQ, "zmq: Shutdown notifier %s at %s\n", notifier->GetType(), notifier->GetAddress());
            notifier->Shutdown();
        }
        zmq_ctx_term(pcontext);

        pcontext = nullptr;
    }
}

namespace {

template <typename Function>
void TryForEachAndRemoveFailed(std::list<std::unique_ptr<CZMQAbstractNotifier>>& notifiers, const Function& func)
{
    for (auto i = notifiers.begin(); i != notifiers.end(); ) {
        CZMQAbstractNotifier* notifier = i->get();
        if (func(notifier)) {
            ++i;
        } else {
            notifier->Shutdown();
            i = notifiers.erase(i);
        }
    }
}

} // anonymous namespace

void CZMQNotificationInterface::UpdatedBlockTip(const CBlockIndex *pindexNew, const CBlockIndex *pindexFork, bool fInitialDownload)
{
    if (fInitialDownload || pindexNew == pindexFork) // In IBD or blocks were disconnected without any new ones
        return;

    TryForEachAndRemoveFailed(notifiers, [pindexNew](CZMQAbstractNotifier* notifier) {
        return notifier->NotifyBlock(pindexNew);
    });
}

void CZMQNotificationInterface::NotifyTransaction(const CTransactionRef& ptx)
{
    TryForEachAndRemoveFailed(notifiers, [&ptx](CZMQAbstractNotifier* notifier) {
        return notifier->NotifyTransaction(*ptx);
    });
}

void CZMQNotificationInterface::TransactionAddedToMempool(const CTransactionRef& ptx)
{
    NotifyTransaction(ptx);

    TryForEachAndRemoveFailed(notifiers, [&ptx](CZMQAbstractNotifier* notifier) {
        return notifier->NotifyPendingTx(*ptx);
    });
}

void CZMQNotificationInterface::BlockConnected(const std::shared_ptr<const CBlock>& pblock, const CBlockIndex* pindexConnected)
{
    for (const CTransactionRef& ptx : pblock->vtx) {
        // Do a normal notify for each transaction added in the block
        NotifyTransaction(ptx);
    }

    TryForEachAndRemoveFailed(notifiers, [&pblock](CZMQAbstractNotifier* notifier) {
        return notifier->NotifyBlockAttached(*pblock);
    });
}

void CZMQNotificationInterface::BlockDisconnected(const std::shared_ptr<const CBlock>& pblock, const CBlockIndex* pindexDisconnected)
{
    TryForEachAndRemoveFailed(notifiers, [&pblock](CZMQAbstractNotifier* notifier) {
        return notifier->NotifyBlockDetached(*pblock);
    });

    for (const CTransactionRef& ptx : pblock->vtx) {
        // Do a normal notify for each transaction removed in block disconnection.
        //
        // Note that we want notifications for those transactions as "pending",
        // but those will (typically) be generated anyway from re-adding to
        // the mempool, which then also fires TransactionAddedToMempool.
        NotifyTransaction(ptx);
    }
}

CZMQNotificationInterface* g_zmq_notification_interface = nullptr;
