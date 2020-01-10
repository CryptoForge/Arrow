#include "assert.h"
#include "boost/variant/static_visitor.hpp"
#include "asyncrpcoperation_saplingconsolidation.h"
#include "init.h"
#include "key_io.h"
#include "rpc/protocol.h"
#include "random.h"
#include "sync.h"
#include "tinyformat.h"
#include "transaction_builder.h"
#include "util.h"
#include "utilmoneystr.h"
#include "wallet.h"

const CAmount FEE = 10000;
const int MIGRATION_EXPIRY_DELTA = 450;

AsyncRPCOperation_saplingconsolidation::AsyncRPCOperation_saplingconsolidation(int targetHeight) : targetHeight_(targetHeight) {}

AsyncRPCOperation_saplingconsolidation::~AsyncRPCOperation_saplingconsolidation() {}

void AsyncRPCOperation_saplingconsolidation::main() {
    if (isCancelled())
        return;

    set_state(OperationStatus::EXECUTING);
    start_execution_clock();

    bool success = false;

    try {
        success = main_impl();
    } catch (const UniValue& objError) {
        int code = find_value(objError, "code").get_int();
        std::string message = find_value(objError, "message").get_str();
        set_error_code(code);
        set_error_message(message);
    } catch (const runtime_error& e) {
        set_error_code(-1);
        set_error_message("runtime error: " + string(e.what()));
    } catch (const logic_error& e) {
        set_error_code(-1);
        set_error_message("logic error: " + string(e.what()));
    } catch (const exception& e) {
        set_error_code(-1);
        set_error_message("general exception: " + string(e.what()));
    } catch (...) {
        set_error_code(-2);
        set_error_message("unknown error");
    }

    stop_execution_clock();

    if (success) {
        set_state(OperationStatus::SUCCESS);
    } else {
        set_state(OperationStatus::FAILED);
    }

    std::string s = strprintf("%s: Sprout->Sapling transactions created. (status=%s", getId(), getStateAsString());
    if (success) {
        s += strprintf(", success)\n");
    } else {
        s += strprintf(", error=%s)\n", getErrorMessage());
    }

    LogPrintf("%s", s);
}

bool AsyncRPCOperation_saplingconsolidation::main_impl() {
    LogPrint("zrpcunsafe", "%s: Beginning AsyncRPCOperation_saplingconsolidation.\n", getId());
    auto consensusParams = Params().GetConsensus();
    auto nextActivationHeight = NextActivationHeight(targetHeight_, consensusParams);
    if (nextActivationHeight && targetHeight_ + MIGRATION_EXPIRY_DELTA >= nextActivationHeight.get()) {
        LogPrint("zrpcunsafe", "%s: Migration txs would be created before a NU activation but may expire after. Skipping this round.\n", getId());
        setConsolidationResult(0, 0, std::vector<std::string>());
        return true;
    }

    std::vector<SproutNoteEntry> sproutEntries;
    std::vector<SaplingNoteEntry> saplingEntries;
    std::set<libzcash::SaplingPaymentAddress> addresses;
    {
        LOCK2(cs_main, pwalletMain->cs_wallet);
        // We set minDepth to 11 to avoid unconfirmed notes and in anticipation of specifying
        // an anchor at height N-10 for each Sprout JoinSplit description
        // Consider, should notes be sorted?
        pwalletMain->GetFilteredNotes(sproutEntries, saplingEntries, "", 11);
        pwalletMain->GetSaplingPaymentAddresses(addresses);
    }

    int numTxCreated = 0;
    std::vector<std::string> consolidationTxIds;
    CAmount amountConsolidated = 0;
    CCoinsViewCache coinsView(pcoinsTip);

    for (auto addr : addresses) {
        libzcash::SaplingExtendedSpendingKey extsk;
        if (pwalletMain->GetSaplingExtendedSpendingKey(addr, extsk)) {

            std::vector<SaplingNoteEntry> fromNotes;
            CAmount amountToSend = 0;
            for (const SaplingNoteEntry& saplingEntry : saplingEntries) {

                libzcash::SaplingIncomingViewingKey ivk;
                pwalletMain->GetSaplingIncomingViewingKey(boost::get<libzcash::SaplingPaymentAddress>(saplingEntry.address), ivk);

                //Select Notes from that same addres we will be send to.
                if (ivk == extsk.expsk.full_viewing_key().in_viewing_key()) {
                  amountToSend = CAmount(saplingEntry.note.value());
                  fromNotes.push_back(saplingEntry);
                }

                //Only use up to 25 Notes
                if (fromNotes.size() >= 25)
                  break;

            }

            amountConsolidated += amountToSend;
            auto builder = TransactionBuilder(consensusParams, targetHeight_, pwalletMain, pzcashParams, &coinsView, &cs_main);
            builder.SetExpiryHeight(targetHeight_ + MIGRATION_EXPIRY_DELTA);
            LogPrint("zrpcunsafe", "%s: Beginning creating transaction with Sapling output amount=%s\n", getId(), FormatMoney(amountToSend - FEE));

            // Select Sapling notes
            std::vector<SaplingOutPoint> ops;
            std::vector<libzcash::SaplingNote> notes;
            for (auto fromNote : fromNotes) {
                ops.push_back(fromNote.op);
                notes.push_back(fromNote.note);
            }

            // Fetch Sapling anchor and witnesses
            uint256 anchor;
            std::vector<boost::optional<SaplingWitness>> witnesses;
            {
                LOCK2(cs_main, pwalletMain->cs_wallet);
                pwalletMain->GetSaplingNoteWitnesses(ops, witnesses, anchor);
            }

            // Add Sapling spends
            for (size_t i = 0; i < notes.size(); i++) {
                if (!witnesses[i]) {
                    LogPrint("zrpcunsafe", "%s: Missing Witnesses. Stopping.\n", getId());
                    break;
                }
                builder.AddSaplingSpend(extsk.expsk, notes[i], anchor, witnesses[i].get());
            }

            builder.SetFee(FEE);
            builder.AddSaplingOutput(extsk.expsk.ovk, addr, amountToSend - FEE);
            CTransaction tx = builder.Build().GetTxOrThrow();

            if (isCancelled()) {
                LogPrint("zrpcunsafe", "%s: Canceled. Stopping.\n", getId());
                break;
            }

            pwalletMain->AddPendingSaplingConsolidationTx(tx);
            LogPrint("zrpcunsafe", "%s: Added pending consolidation transaction with txid=%s\n", getId(), tx.GetHash().ToString());
            amountConsolidated += amountToSend - FEE;
            consolidationTxIds.push_back(tx.GetHash().ToString());

        }
    }

    LogPrint("zrpcunsafe", "%s: Created %d transactions with total Sapling output amount=%s\n", getId(), numTxCreated, FormatMoney(amountConsolidated));
    setConsolidationResult(numTxCreated, amountConsolidated, consolidationTxIds);
    return true;

}

void AsyncRPCOperation_saplingconsolidation::setConsolidationResult(int numTxCreated, const CAmount& amountConsolidated, const std::vector<std::string>& consolidationTxIds) {
    UniValue res(UniValue::VOBJ);
    res.push_back(Pair("num_tx_created", numTxCreated));
    res.push_back(Pair("amount_consolidated", FormatMoney(amountConsolidated)));
    UniValue txIds(UniValue::VARR);
    for (const std::string& txId : consolidationTxIds) {
        txIds.push_back(txId);
    }
    res.push_back(Pair("consolidation_txids", txIds));
    set_result(res);
}

CAmount AsyncRPCOperation_saplingconsolidation::chooseAmount(const CAmount& availableFunds) {
    CAmount amount = 0;
    do {
        // 1. Choose an integer exponent uniformly in the range 6 to 8 inclusive.
        int exponent = GetRand(3) + 6;
        // 2. Choose an integer mantissa uniformly in the range 1 to 99 inclusive.
        uint64_t mantissa = GetRand(99) + 1;
        // 3. Calculate amount := (mantissa * 10^exponent) zatoshi.
        int pow = std::pow(10, exponent);
        amount = mantissa * pow;
        // 4. If amount is greater than the amount remaining to send, repeat from step 1.
    } while (amount > availableFunds);
    return amount;
}

// Unless otherwise specified, the consolidation destination address is the address for Sapling account 0
libzcash::SaplingPaymentAddress AsyncRPCOperation_saplingconsolidation::getConsolidationDestAddress(const HDSeed& seed) {
    if (mapArgs.count("-consolidationdestaddress")) {
        std::string consolidationDestAddress = mapArgs["-consolidationdestaddress"];
        auto address = DecodePaymentAddress(consolidationDestAddress);
        auto saplingAddress = boost::get<libzcash::SaplingPaymentAddress>(&address);
        assert(saplingAddress != nullptr); // This is checked in init.cpp
        return *saplingAddress;
    }
    // Derive the address for Sapling account 0
    auto m = libzcash::SaplingExtendedSpendingKey::Master(seed);
    uint32_t bip44CoinType = Params().BIP44CoinType();

    // We use a fixed keypath scheme of m/32'/coin_type'/account'
    // Derive m/32'
    auto m_32h = m.Derive(32 | ZIP32_HARDENED_KEY_LIMIT);
    // Derive m/32'/coin_type'
    auto m_32h_cth = m_32h.Derive(bip44CoinType | ZIP32_HARDENED_KEY_LIMIT);

    // Derive m/32'/coin_type'/0'
    libzcash::SaplingExtendedSpendingKey xsk = m_32h_cth.Derive(0 | ZIP32_HARDENED_KEY_LIMIT);

    libzcash::SaplingPaymentAddress toAddress = xsk.DefaultAddress();

    // Refactor: this is similar logic as in the visitor HaveSpendingKeyForPaymentAddress and is used elsewhere
    libzcash::SaplingIncomingViewingKey ivk;
    libzcash::SaplingFullViewingKey fvk;
    if (!(pwalletMain->GetSaplingIncomingViewingKey(toAddress, ivk) &&
        pwalletMain->GetSaplingFullViewingKey(ivk, fvk) &&
        pwalletMain->HaveSaplingSpendingKey(fvk))) {
        // Sapling account 0 must be the first address returned by GenerateNewSaplingZKey
        assert(pwalletMain->GenerateNewSaplingZKey() == toAddress);
    }

    return toAddress;
}

void AsyncRPCOperation_saplingconsolidation::cancel() {
    set_state(OperationStatus::CANCELLED);
}

UniValue AsyncRPCOperation_saplingconsolidation::getStatus() const {
    UniValue v = AsyncRPCOperation::getStatus();
    UniValue obj = v.get_obj();
    obj.push_back(Pair("method", "saplingconsolidation"));
    obj.push_back(Pair("target_height", targetHeight_));
    return obj;
}
