#include "amount.h"
#include "asyncrpcoperation.h"
#include "univalue.h"
#include "zcash/Address.hpp"
#include "zcash/zip32.h"

class AsyncRPCOperation_saplingconsolidation : public AsyncRPCOperation
{
public:
    AsyncRPCOperation_saplingconsolidation(int targetHeight);
    virtual ~AsyncRPCOperation_saplingconsolidation();

    // We don't want to be copied or moved around
    AsyncRPCOperation_saplingconsolidation(AsyncRPCOperation_saplingconsolidation const&) = delete;            // Copy construct
    AsyncRPCOperation_saplingconsolidation(AsyncRPCOperation_saplingconsolidation&&) = delete;                 // Move construct
    AsyncRPCOperation_saplingconsolidation& operator=(AsyncRPCOperation_saplingconsolidation const&) = delete; // Copy assign
    AsyncRPCOperation_saplingconsolidation& operator=(AsyncRPCOperation_saplingconsolidation&&) = delete;      // Move assign

    static libzcash::SaplingPaymentAddress getConsolidationDestAddress(const HDSeed& seed);

    virtual void main();

    virtual void cancel();

    virtual UniValue getStatus() const;

private:
    int targetHeight_;

    bool main_impl();

    void setConsolidationResult(int numTxCreated, const CAmount& amountConsolidated, const std::vector<std::string>& consolidationTxIds);

    CAmount chooseAmount(const CAmount& availableFunds);
};
