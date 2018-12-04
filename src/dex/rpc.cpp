
#include "rpc/server.h"

#include "clientversion.h"
#include "validation.h"
#include "netmessagemaker.h"
#include "net.h"
#include "init.h"
#include "netbase.h"
#include "protocol.h"
#include "sync.h"
#include "timedata.h"
#include "util.h"
#include "utilstrencodings.h"
#include "version.h"
#include <univalue.h>
#include "streams.h"
#include <algorithm>
#include "dexoffer.h"
#include "dexsync.h"
#include "random.h"
#include "dex/db/dexdb.h"
#include "dex.h"
#include "dex/db/dexdto.h"
#include "core_io.h"
#include <boost/foreach.hpp>
#include <boost/algorithm/string.hpp>

#include "dextransaction.h"
#include "parserjsonoffer.h"
#include "dexmanager.h"
#include "db/countryiso.h"
#include "db/currencyiso.h"
#include "db/defaultdatafordb.h"

#ifdef ENABLE_WALLET
#include "wallet/wallet.h"
#endif


using namespace std;

UniValue dexoffers(const JSONRPCRequest& request)
{
    if (!fTxIndex) {
        throw runtime_error(
            "To use this feature please enable -txindex and make -reindex.\n"
        );
    }

    if (dex::DexDB::self() == nullptr) {
        throw runtime_error(
            "DexDB is not initialized.\n"
        );
    }

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 8)
        throw runtime_error(
            "dexoffers [buy|sell|all] [country] [currency] [payment_method] [limit N] [offset N]\n"
            "Get DEX offers list.\n"

            "\nArguments:\n"
            "NOTE: Any of the parameters may be skipped.You must specify at least one parameter.\n"
            "\tcountry         (string, optional) two-letter country code (ISO 3166-1 alpha-2 code).\n"
            "\tcurrency        (string, optional) three-letter currency code (ISO 4217).\n"
            "\tpayment_method  (string, optional, case insensitive) payment method name.\n"
            "\tlimit N         (int, optional) N max output offers, default use global settings"
            "\toffset N        (int, optional) N identify the starting point to return rows, use with limit"

            "\nResult (for example):\n"
            "[\n"
            "   {\n"
            "     \"type\"          : \"sell\",   offer type, buy or sell\n"
            "     \"idTransaction\" : \"<id>\",   transaction with offer fee\n"
            "     \"hash\"          : \"<hash>\", offer hash\n"
            "     \"countryIso\"    : \"RU\",     country (ISO 3166-1 alpha-2)\n"
            "     \"currencyIso\"   : \"RUB\",    currency (ISO 4217)\n"
            "     \"paymentMethod\" : 1,        payment method code (default 1 - cash, 128 - online)\n"
            "     \"price\"         : 10000,\n"
            "     \"minAmount\"     : 1000,\n"
            "     \"timeCreate\"    : 947...3344,\n"
            "     \"timeExpiration\": 947...9344, offer expiration (in seconds)\n"
            "     \"shortInfo\"     : \"...\",    offer short info (max 140 bytes)\n"
            "     \"details\"       : \"...\"     offer details (max 1024 bytes)\n"
            "   },\n"
            "   ...\n"
            "]\n"

            "\nExamples:\n"
            + HelpExampleCli("dexoffers", "all USD")
            + HelpExampleCli("dexoffers", "RU RUB cash")
            + HelpExampleCli("dexoffers", "all USD online")
            + HelpExampleCli("dexoffers", "all USD limit 3")
            + HelpExampleCli("dexoffers", "all USD limit 3 offset 10")
        );

    UniValue result(UniValue::VARR);

    std::string typefilter, countryfilter, currencyfilter;
    std::string methodfilter;
    unsigned char methodfiltertype = 0;
    std::list<std::string> words {"buy", "sell", "all"};
    dex::CountryIso  countryiso;
    dex::CurrencyIso currencyiso;

    int limit = 0;
    int offset = 0;

    for (size_t i = 0; i < request.params.size(); i++) {
        if (request.params[i].get_str() == "limit") {
            if (i == 0 || request.params.size() <= i+1) {
                throw runtime_error("\nnot enough parameters\n");
            }

            std::string maxStr = request.params[i+1].get_str();
            limit = std::stoi(maxStr);

            if (request.params.size() > i+2) {
                i++;
                continue;
            } else {
                break;
            }
        }
        if (request.params[i].get_str() == "offset" && limit > 0) {
            if (i == 0 || request.params.size() <= i+1) {
                throw runtime_error("\nnot enough parameters\n");
            }

            std::string maxStr = request.params[i+1].get_str();
            offset = std::stoi(maxStr);
            break;
        }
        if (i == 0 && typefilter.empty()) {
            if (std::find(words.begin(), words.end(), request.params[0].get_str()) != words.end()) {
                typefilter = request.params[0].get_str();
                continue;
            } else {
                typefilter = "all";
            }
        }
        if (i < 2 && countryfilter.empty()) {
            if (countryiso.isValid(request.params[i].get_str())) {
                countryfilter = request.params[i].get_str();
                continue;
            }
        }
        if (i < 3 && currencyfilter.empty()) {
            if (currencyiso.isValid(request.params[i].get_str())) {
                currencyfilter = request.params[i].get_str();
                continue;
            }
        }
        {
            methodfilter.clear();
            std::string methodname = boost::algorithm::to_lower_copy(request.params[i].get_str());
            std::list<dex::PaymentMethodInfo> pms = dex::DexDB::self()->getPaymentMethodsInfo();
            for (auto j : pms) {
                std::string name = boost::algorithm::to_lower_copy(j.name);
                if (name == methodname) {
                    methodfilter = j.name;
                    methodfiltertype = j.type;
                }
            }

            if (methodfilter.empty()) {
                throw runtime_error("\nwrong parameter: " + request.params[i].get_str() + "\n");
            }
        }
    }

    if (typefilter.empty()) {
        throw runtime_error("\nwrong parameters\n");
    }

    // check country and currency in DB
    if (countryfilter != "") {
        dex::CountryInfo  countryinfo = dex::DexDB::self()->getCountryInfo(countryfilter);
        if (!countryinfo.enabled) {
            throw runtime_error("\nERROR: this country is disabled in DB\n");
        }
    }
    if (currencyfilter != "") {
        dex::CurrencyInfo  currencyinfo = dex::DexDB::self()->getCurrencyInfo(currencyfilter);
        if (!currencyinfo.enabled) {
            throw runtime_error("\nERROR: this currency is disabled in DB\n");
        }
    }

    if (limit == 0) {
        limit = dex::maxOutput();
    }
    int step = 0;

    if (typefilter == "buy" || typefilter == "all") {
        std::list<dex::OfferInfo> offers = dex::DexDB::self()->getOffersBuy(countryfilter, currencyfilter, methodfiltertype, limit, offset);
        for (auto i : offers) {
            dex::CDexOffer o(i, dex::Buy);
            result.push_back(o.getUniValue());

            if (limit > 0) {
                step++;

                if (step == limit) {
                    break;
                }
            }
        }
    }

    if ((typefilter == "sell" || typefilter == "all") && !(limit > 0 && step == limit)) {
        std::list<dex::OfferInfo> offers = dex::DexDB::self()->getOffersSell(countryfilter, currencyfilter, methodfiltertype, limit-step, offset);
        for (auto i : offers) {
            dex::CDexOffer o(i, dex::Sell);
            result.push_back(o.getUniValue());
        }
    }

    return result;
}




UniValue dexmyoffers(const JSONRPCRequest& request)
{
    if (!fTxIndex) {
        throw runtime_error(
            "To use this feature please enable -txindex and make -reindex.\n"
        );
    }
    if (dex::DexDB::self() == nullptr) {
        throw runtime_error(
            "DexDB is not initialized.\n"
        );
    }
    if (request.fHelp || request.params.size() < 1 || request.params.size() > 9)
        throw runtime_error(
            "dexmyoffers [buy|sell|all] [country] [currency] [payment_method] [status] [limit N] [offset N]\n"
            "Return a list of  DEX own offers.\n"

            "\nArguments:\n"
            "NOTE: Any of the parameters may be skipped.You must specify at least one parameter.\n"
            "\tcountry         (string, optional) two-letter country code (ISO 3166-1 alpha-2 code).\n"
            "\tcurrency        (string, optional) three-letter currency code (ISO 4217).\n"
            "\tpayment_method  (string, optional, case insensitive) payment method name.\n"
            "\tstatus          (string, optional, case insensitive) offer status (Active,Draft,Expired,Cancelled,Suspended,Unconfirmed).\n"
            "\tlimit N         (int, optional) N max output offers, default use global settings"
            "\toffset N        (int, optional) N identify the starting point to return rows, use with limit"

            "\nResult (for example):\n"
            "[\n"
            "   {\n"
            "     \"type\"          : \"sell\",   offer type, buy or sell\n"
            "     \"status\"        : \"1\",      offer status\n"
            "     \"statusStr\"     : \"Draft\",  offer status name\n"
            "     \"idTransaction\" : \"<id>\",   transaction with offer fee\n"
            "     \"hash\"          : \"<hash>\", offer hash\n"
            "     \"pubKey\"        : \"<key>\",  offer public key\n"
            "     \"countryIso\"    : \"RU\",     country (ISO 3166-1 alpha-2)\n"
            "     \"currencyIso\"   : \"RUB\",    currency (ISO 4217)\n"
            "     \"paymentMethod\" : 1,        payment method code (default 1 - cash, 128 - online)\n"
            "     \"price\"         : 10000,\n"
            "     \"minAmount\"     : 1000,\n"
            "     \"timeCreate\"    : 947...9344,\n"
            "     \"timeExpiration\": 947...5344, offer expiration\n"
            "     \"shortInfo\"     : \"...\",    offer short info (max 140 bytes)\n"
            "     \"details\"       : \"...\"     offer details (max 1024 bytes)\n"
            "   },\n"
            "   ...\n"
            "]\n"

            "\nExamples:\n"
            + HelpExampleCli("dexmyoffers", "all USD")
            + HelpExampleCli("dexmyoffers", "RU RUB cash")
            + HelpExampleCli("dexmyoffers", "all USD online")
            + HelpExampleCli("dexmyoffers", "all USD limit 3")
            + HelpExampleCli("dexmyoffers", "all USD limit 3 offset 10")
        );

    UniValue result(UniValue::VARR);

    std::string typefilter, countryfilter, currencyfilter, methodfilter;
    int statusfilter = dex::Indefined;
    dex::CStatusOffer status;
    unsigned char methodfiltertype = 0;
    std::map<std::string, int> words;
    words["buy"] = 0;
    words["sell"] = 1;
    words["all"] = -1;
    dex::CountryIso  countryiso;
    dex::CurrencyIso currencyiso;

    int limit = 0;
    int offset = 0;

    for (size_t i = 0; i < request.params.size(); i++) {
        if (request.params[i].get_str() == "limit") {
            if (i == 0 || request.params.size() <= i+1) {
                throw runtime_error("\nnot enough parameters\n");
            }

            std::string maxStr = request.params[i+1].get_str();
            limit = std::stoi(maxStr);

            if (request.params.size() > i+2) {
                i++;
                continue;
            } else {
                break;
            }
        } else if (request.params[i].get_str() == "offset" && limit > 0) {
            if (i == 0 || request.params.size() <= i+1) {
                throw runtime_error("\nnot enough parameters\n");
            }

            std::string maxStr = request.params[i+1].get_str();
            offset = std::stoi(maxStr);
            break;
        } else {
            if (typefilter.empty()) {
                std::string key = boost::algorithm::to_lower_copy(request.params[i].get_str());
                if (words.find(key) != words.end()) {
                    typefilter = key;
                    continue;
                }
            }
            if (countryfilter.empty()) {
                if (countryiso.isValid(request.params[i].get_str())) {
                    countryfilter = request.params[i].get_str();
                    continue;
                }
            }
            if (currencyfilter.empty()) {
                if (currencyiso.isValid(request.params[i].get_str())) {
                    currencyfilter = request.params[i].get_str();
                    continue;
                }
            }
            if (methodfilter.empty()) {
                std::string methodname = boost::algorithm::to_lower_copy(request.params[i].get_str());
                std::list<dex::PaymentMethodInfo> pms = dex::DexDB::self()->getPaymentMethodsInfo();
                for (auto j : pms) {
                    std::string name = boost::algorithm::to_lower_copy(j.name);
                    if (name == methodname) {
                        methodfilter = j.name;
                        methodfiltertype = j.type;
                        continue;
                    }
                }
            }
            if (statusfilter == dex::Indefined) {
                status.set(request.params[i].get_str());
                if (status != dex::Indefined) {
                    statusfilter = status;
                }
            }
        }
    }

    if (typefilter.empty()) {
        typefilter = "all";
    }

    // check country and currency in DB
    if (countryfilter != "") {
        dex::CountryInfo  countryinfo = dex::DexDB::self()->getCountryInfo(countryfilter);
        if (!countryinfo.enabled) {
            throw runtime_error("\nERROR: this country is disabled in DB\n");
        }
    }
    if (currencyfilter != "") {
        dex::CurrencyInfo  currencyinfo = dex::DexDB::self()->getCurrencyInfo(currencyfilter);
        if (!currencyinfo.enabled) {
            throw runtime_error("\nERROR: this currency is disabled in DB\n");
        }
    }

    if (limit == 0) {
        limit = dex::maxOutput();
    }

    std::list<dex::MyOfferInfo> myoffers = dex::DexDB::self()->getMyOffers(countryfilter, currencyfilter, methodfiltertype, words[typefilter], status, limit, offset);

    for (auto i : myoffers) {
        dex::CDexOffer o(i.getOfferInfo(), i.type);
        UniValue v = o.getUniValue();
        v.push_back(Pair("status", i.status));
        v.push_back(Pair("statusStr", status.status2str(i.status)));
        result.push_back(v);
    }

    return result;
}

UniValue dexofferscount(const JSONRPCRequest& request)
{
    if (!fTxIndex) {
        throw runtime_error(
            "To use this feature please enable -txindex and make -reindex.\n"
        );
    }

    if (dex::DexDB::self() == nullptr) {
        throw runtime_error(
            "DexDB is not initialized.\n"
        );
    }

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 8)
        throw runtime_error(
            "dexofferscount [buy|sell|all] [country] [currency] [payment_method]\n"
            "Get DEX offers count.\n"

            "\nArguments:\n"
            "NOTE: Any of the parameters may be skipped.You must specify at least one parameter.\n"
            "\tcountry         (string, optional) two-letter country code (ISO 3166-1 alpha-2 code).\n"
            "\tcurrency        (string, optional) three-letter currency code (ISO 4217).\n"
            "\tpayment_method  (string, optional, case insensitive) payment method name.\n"

            "\nResult offers count\n"

            "\nExamples:\n"
            + HelpExampleCli("dexofferscount", "all USD")
            + HelpExampleCli("dexofferscount", "RU RUB cash")
            + HelpExampleCli("dexofferscount", "all USD online")
        );

    std::string typefilter, countryfilter, currencyfilter;
    std::string methodfilter;
    unsigned char methodfiltertype = 0;
    std::list<std::string> words {"buy", "sell", "all"};
    dex::CountryIso  countryiso;
    dex::CurrencyIso currencyiso;

    for (size_t i = 0; i < request.params.size(); i++) {
        if (i == 0 && typefilter.empty()) {
            if (std::find(words.begin(), words.end(), request.params[0].get_str()) != words.end()) {
                typefilter = request.params[0].get_str();
                continue;
            } else {
                typefilter = "all";
            }
        }
        if (i < 2 && countryfilter.empty()) {
            if (countryiso.isValid(request.params[i].get_str())) {
                countryfilter = request.params[i].get_str();
                continue;
            }
        }
        if (i < 3 && currencyfilter.empty()) {
            if (currencyiso.isValid(request.params[i].get_str())) {
                currencyfilter = request.params[i].get_str();
                continue;
            }
        }
        {
            methodfilter.clear();
            std::string methodname = boost::algorithm::to_lower_copy(request.params[i].get_str());
            std::list<dex::PaymentMethodInfo> pms = dex::DexDB::self()->getPaymentMethodsInfo();
            for (auto j : pms) {
                std::string name = boost::algorithm::to_lower_copy(j.name);
                if (name == methodname) {
                    methodfilter = j.name;
                    methodfiltertype = j.type;
                }
            }

            if (methodfilter.empty()) {
                throw runtime_error("\nwrong parameter: " + request.params[i].get_str() + "\n");
            }
        }
    }

    if (typefilter.empty()) {
        throw runtime_error("\nwrong parameters\n");
    }

    if (countryfilter != "") {
        dex::CountryInfo  countryinfo = dex::DexDB::self()->getCountryInfo(countryfilter);
        if (!countryinfo.enabled) {
            throw runtime_error("\nERROR: this country is disabled in DB\n");
        }
    }
    if (currencyfilter != "") {
        dex::CurrencyInfo  currencyinfo = dex::DexDB::self()->getCurrencyInfo(currencyfilter);
        if (!currencyinfo.enabled) {
            throw runtime_error("\nERROR: this currency is disabled in DB\n");
        }
    }

    size_t count = 0;
    if (typefilter == "buy") {
        count = dex::DexDB::self()->countOffersBuy(countryfilter, currencyfilter, methodfiltertype);
    } else if (typefilter == "sell") {
        count = dex::DexDB::self()->countOffersSell(countryfilter, currencyfilter, methodfiltertype);
    } else {
        count = dex::DexDB::self()->countOffersBuy(countryfilter, currencyfilter, methodfiltertype);
        count += dex::DexDB::self()->countOffersSell(countryfilter, currencyfilter, methodfiltertype);
    }

    UniValue result(UniValue::VOBJ);
    result.push_back(Pair("count", static_cast<uint64_t>(count)));

    return result;
}

UniValue dexmyofferscount(const JSONRPCRequest& request)
{
    if (!fTxIndex) {
        throw runtime_error(
            "To use this feature please enable -txindex and make -reindex.\n"
        );
    }
    if (dex::DexDB::self() == nullptr) {
        throw runtime_error(
            "DexDB is not initialized.\n"
        );
    }
    if (request.fHelp || request.params.size() < 1 || request.params.size() > 9)
        throw runtime_error(
            "dexmyoffers [buy|sell|all] [country] [currency] [payment_method] [status] [limit N] [offset N]\n"
            "Return count DEX own offers.\n"

            "\nArguments:\n"
            "NOTE: Any of the parameters may be skipped.You must specify at least one parameter.\n"
            "\tcountry         (string, optional) two-letter country code (ISO 3166-1 alpha-2 code).\n"
            "\tcurrency        (string, optional) three-letter currency code (ISO 4217).\n"
            "\tpayment_method  (string, optional, case insensitive) payment method name.\n"
            "\tstatus          (string, optional, case insensitive) offer status (Active,Draft,Expired,Cancelled,Suspended,Unconfirmed).\n"

            "\nResult offers count\n"

            "\nExamples:\n"
            + HelpExampleCli("dexmyofferscount", "all USD")
            + HelpExampleCli("dexmyofferscount", "RU RUB cash")
            + HelpExampleCli("dexmyofferscount", "all USD online")
        );


    std::string typefilter, countryfilter, currencyfilter, methodfilter;
    int statusfilter = dex::Indefined;
    dex::CStatusOffer status;
    unsigned char methodfiltertype = 0;
    std::map<std::string, int> words;
    words["buy"] = 0;
    words["sell"] = 1;
    words["all"] = -1;
    dex::CountryIso  countryiso;
    dex::CurrencyIso currencyiso;

    for (size_t i = 0; i < request.params.size(); i++) {
        if (typefilter.empty()) {
            std::string key = boost::algorithm::to_lower_copy(request.params[i].get_str());
            if (words.find(key) != words.end()) {
                typefilter = key;
                continue;
            }
        }
        if (countryfilter.empty()) {
            if (countryiso.isValid(request.params[i].get_str())) {
                countryfilter = request.params[i].get_str();
                continue;
            }
        }
        if (currencyfilter.empty()) {
            if (currencyiso.isValid(request.params[i].get_str())) {
                currencyfilter = request.params[i].get_str();
                continue;
            }
        }
        if (methodfilter.empty()) {
            std::string methodname = boost::algorithm::to_lower_copy(request.params[i].get_str());
            std::list<dex::PaymentMethodInfo> pms = dex::DexDB::self()->getPaymentMethodsInfo();
            for (auto j : pms) {
                std::string name = boost::algorithm::to_lower_copy(j.name);
                if (name == methodname) {
                    methodfilter = j.name;
                    methodfiltertype = j.type;
                    continue;
                }
            }
        }
        if (statusfilter == dex::Indefined) {
            status.set(request.params[i].get_str());
            if (status != dex::Indefined) {
                statusfilter = status;
            }
        }
    }

    if (typefilter.empty()) {
        typefilter = "all";
    }

    if (countryfilter != "") {
        dex::CountryInfo  countryinfo = dex::DexDB::self()->getCountryInfo(countryfilter);
        if (!countryinfo.enabled) {
            throw runtime_error("\nERROR: this country is disabled in DB\n");
        }
    }
    if (currencyfilter != "") {
        dex::CurrencyInfo  currencyinfo = dex::DexDB::self()->getCurrencyInfo(currencyfilter);
        if (!currencyinfo.enabled) {
            throw runtime_error("\nERROR: this currency is disabled in DB\n");
        }
    }

    auto count = dex::DexDB::self()->countMyOffers(countryfilter, currencyfilter, methodfiltertype, words[typefilter], status);

    UniValue result(UniValue::VOBJ);
    result.push_back(Pair("count", static_cast<uint64_t>(count)));

    return result;
}


UniValue deldexoffer(const JSONRPCRequest& request)
{
    if (!fTxIndex) {
        throw runtime_error(
            "To use this feature please enable -txindex and make -reindex.\n"
        );
    }
    if (dex::DexDB::self() == nullptr) {
        throw runtime_error(
            "DexDB is not initialized.\n"
        );
    }
    if (request.fHelp || request.params.size() != 1)
        throw runtime_error(
            "deldexoffer <hash>\n\n"
            "Delete offer from local DB and broadcast message.\n"
            "To do this, you should have a private key in your wallet that matches the public key in the offer.\n"

            "\nArgument:\n"
            "\thash         (string) offer hash, hex digest.\n"

            "\nExample:\n"
            + HelpExampleCli("deldexoffer", "AABB...CCDD")
        );

    std::string strOfferHash = request.params[0].get_str();
    if (strOfferHash.empty()) {
        throw runtime_error("\nERROR: offer hash is empty");
    }

    uint256 hash = uint256S(strOfferHash);
    if (hash.IsNull()) {
        throw runtime_error("\nERROR: offer hash error\n");
    }

    //dex::DexDB db(strDexDbFile);

    dex::CDexOffer offer;
    if (dex::DexDB::self()->isExistMyOfferByHash(hash)) {
        dex::MyOfferInfo myoffer = dex::DexDB::self()->getMyOfferByHash(hash);
        offer = dex::CDexOffer(myoffer);
    } else if (dex::DexDB::self()->isExistOfferBuyByHash(hash)) {
        offer = dex::CDexOffer(dex::DexDB::self()->getOfferBuyByHash(hash), dex::Buy);
    } else if (dex::DexDB::self()->isExistOfferSellByHash(hash)) {
        offer = dex::CDexOffer(dex::DexDB::self()->getOfferSellByHash(hash), dex::Sell);
    } else {
        throw runtime_error("\nERROR: offer not found in DB\n");
    }

    dex::CDex dex(offer);
    std::string error;
    CKey key;
    if (!dex.FindKey(key, error)) {
        throw runtime_error(error.c_str());
    }

    std::vector<unsigned char> vchSign;
    if (!dex.SignOffer(key, vchSign, error)) {
        throw runtime_error(error.c_str());
    }

    int sended = 0;
    if (offer.status != dex::Draft) {
        auto vNodesCopy = g_connman->CopyNodeVector();
        for (auto pNode : vNodesCopy) {
            uint64_t bytes = pNode->nSendBytes;
            g_connman->PushMessage(pNode, CNetMsgMaker(pNode->GetSendVersion()).Make(NetMsgType::DEXDELOFFER, offer, vchSign));
            if (pNode->nSendBytes > bytes) sended++;
        }

        g_connman->ReleaseNodeVector(vNodesCopy);
    }

    if (sended > 1 || offer.status == dex::Draft || offer.status == dex::Indefined) {
        if (offer.isBuy()  && offer.status != dex::Draft) dex::DexDB::self()->deleteOfferBuyByHash(offer.hash);
        if (offer.isSell() && offer.status != dex::Draft) dex::DexDB::self()->deleteOfferSellByHash(offer.hash);
        if (offer.isMyOffer()) dex::DexDB::self()->deleteMyOfferByHash(offer.hash);
    }

    throw runtime_error("\nsuccess\n");

    return NullUniValue;
}



UniValue adddexoffer(const JSONRPCRequest& request)
{
    if (!fTxIndex) {
        throw runtime_error(
            "To use this feature please enable -txindex and make -reindex.\n"
        );
    }
    if (dex::DexDB::self() == nullptr) {
        throw runtime_error(
            "DexDB is not initialized.\n"
        );
    }
    if (request.fHelp || request.params.size() != 1)
        throw runtime_error(

            "adddexoffer <json-data>\n\n"

            "\nArgument:\n"
            "\tjson-data    (string) offer data in format json.\n"

            "\njson attributes:\n"
            "\ttype             (string) offer type, 'buy' or 'sell'\n"
            "\tcountryIso       (string) two-letter country code (ISO 3166-1 alpha-2 code)\n"
            "\tcurrencyIso      (string) three-letter currency code (ISO 4217)\n"
            "\tpaymentMethod    (number) payment method, correct values: 1(cash payment), 128(online payment)\n"
            "\tprice            (string) offer price, max 8 digits after the decimal point\n"
            "\tminAmount        (string) offer minAmount, max 8 digits after the decimal point\n"
            "\ttimeToExpiration (number) period valid offer, correct values: 10, 20, 30\n"
            "\tshortInfo        (string) short info, max 140 symbols\n"
            "\tdetails          (string) detail info\n"

            "\nExample:\n"
            + HelpExampleCli("adddexoffer", "\"{"
                                            "\\\"type\\\": \\\"sell\\\","
                                            "\\\"countryIso\\\": \\\"RU\\\","
                                            "\\\"currencyIso\\\": \\\"RUB\\\","
                                            "\\\"paymentMethod\\\": 1,"
                                            "\\\"price\\\": \\\"100.05\\\","
                                            "\\\"minAmount\\\": \\\"10.005\\\","
                                            "\\\"timeToExpiration\\\": 30,"
                                            "\\\"shortInfo\\\": \\\"test offer\\\","
                                            "\\\"details\\\": \\\"test offer details\\\""
                                            "}\"")
        );

    std::string jsonData = request.params[0].get_str();
    std::string error;

    dex::MyOfferInfo offer = dex::jsonToMyOfferInfo(jsonData, error);
    offer.status = dex::Draft;
    offer.editingVersion = 0;

    if (error.length() > 0) {
        throw runtime_error("\nERROR: " + error);
    }

    dex::CDexOffer cOffer;
    CKey key = pwalletMain->GeneratePrivKey();
    CPubKey pkey = key.GetPubKey();
    if (!pwalletMain->AddKeyPubKey(key, pkey)) {
        throw runtime_error("\nERROR: add key to wallet error");
    }

    offer.pubKey = HexStr(pkey.begin(), pkey.end());

    if (!cOffer.Create(offer)) {
        throw runtime_error("\nERROR: error create offer");
    }

    dex::DexDB::self()->addMyOffer(dex::MyOfferInfo(cOffer));

    if (!dex::DexDB::self()->isExistMyOfferByHash(cOffer.hash)) {
        throw runtime_error("\nERROR: the operation failed");
    }

    UniValue result(UniValue::VOBJ);
    result.push_back(Pair("hash", cOffer.hash.GetHex()));
    return result;
}

UniValue editdexoffer(const JSONRPCRequest& request)
{
    if (!fTxIndex) {
        throw runtime_error(
            "To use this feature please enable -txindex and make -reindex.\n"
        );
    }
    if (dex::DexDB::self() == nullptr) {
        throw runtime_error(
            "DexDB is not initialized.\n"
        );
    }
    if (request.fHelp || request.params.size() != 2)
        throw runtime_error(

            "editdexoffer <hash> <json-data>\n\n"

            "\nArgument:\n"
            "\thash         (string) offer hash, hex digest.\n"
            "\tjson-data    (string) offer data in format json.\n"

            "\nWARNING: If offer have status Active, you can change only price, shortInfo, details"

            "\njson attributes:\n"
            "\ttype             (string) offer type, 'buy' or 'sell'\n"
            "\tcountryIso       (string) two-letter country code (ISO 3166-1 alpha-2 code)\n"
            "\tcurrencyIso      (string) three-letter currency code (ISO 4217)\n"
            "\tpaymentMethod    (number) payment method, correct values: 1(cash payment), 128(online payment)\n"
            "\tprice            (string) offer price, max 8 digits after the decimal point\n"
            "\tminAmount        (string) offer minAmount, max 8 digits after the decimal point\n"
            "\ttimeToExpiration (number) period valid offer, correct values: 10, 20, 30\n"
            "\tshortInfo        (string) short info, max 140 symbols\n"
            "\tdetails          (string) detail info\n"

            "\nExample:\n"
            + HelpExampleCli("editdexoffer", "AABB...CCDD \"{"
                                            "\\\"type\\\": \\\"sell\\\","
                                            "\\\"countryIso\\\": \\\"RU\\\","
                                            "\\\"currencyIso\\\": \\\"RUB\\\","
                                            "\\\"paymentMethod\\\": 1,"
                                            "\\\"price\\\": \\\"100.03\\\","
                                            "\\\"minAmount\\\": \\\"10.005\\\","
                                            "\\\"timeToExpiration\\\": 30,"
                                            "\\\"shortInfo\\\": \\\"test offer\\\","
                                            "\\\"details\\\": \\\"test offer details\\\""
                                            "}\"")
        );

    std::string strOfferHash = request.params[0].get_str();
    if (strOfferHash.empty()) {
        throw runtime_error("\nERROR: offer hash is empty");
    }

    uint256 hash = uint256S(strOfferHash);
    if (hash.IsNull()) {
        throw runtime_error("\nERROR: offer hash error\n");
    }

    if (!dex::DexDB::self()->isExistMyOfferByHash(hash)) {
        throw runtime_error("\nERROR: offer not found in DB\n");
    }

    std::string jsonData = request.params[1].get_str();
    std::string error;
    dex::MyOfferInfo offer = dex::jsonToMyOfferInfo(jsonData, error);

    if (error.length() > 0) {
        throw runtime_error("\nERROR: " + error);
    }

    dex::MyOfferInfo currentMyOffer = dex::DexDB::self()->getMyOfferByHash(hash);
    if (currentMyOffer.status == dex::Draft) {
        offer.status = dex::Draft;
        offer.editingVersion = 0;

        dex::dexman.addOrEditDraftMyOffer(offer);
        if (!dex::DexDB::self()->isExistMyOfferByHash(offer.hash)) {
            throw runtime_error("\nERROR: the operation failed");
        }

        UniValue result(UniValue::VOBJ);
        result.push_back(Pair("new hash", offer.hash.GetHex()));
        return result;
    } else if (currentMyOffer.status == dex::Active) {
        if (currentMyOffer.type != offer.type) {
            throw runtime_error("\nERROR: unchanged data doesn't match");
        }

        if (currentMyOffer.countryIso != offer.countryIso) {
            throw runtime_error("\nERROR: unchanged data doesn't match");
        }

        if (currentMyOffer.currencyIso != offer.currencyIso) {
            throw runtime_error("\nERROR: unchanged data doesn't match");
        }

        if (currentMyOffer.paymentMethod != offer.paymentMethod) {
            throw runtime_error("\nERROR: unchanged data doesn't match");
        }

        if (currentMyOffer.minAmount != offer.minAmount) {
            throw runtime_error("\nERROR: unchanged data doesn't match");
        }

        int shelfLife = ((offer.timeToExpiration - offer.timeCreate - 1) / 86400) +1;
        int currentShelfLife = ((currentMyOffer.timeToExpiration - currentMyOffer.timeCreate - 1) / 86400) +1;

        if (shelfLife != currentShelfLife) {
            throw runtime_error("\nERROR: unchanged data doesn't match");
        }

        currentMyOffer.price = offer.price;
        currentMyOffer.shortInfo = offer.shortInfo;
        currentMyOffer.details = offer.details;

        std::string error;
        dex::dexman.prepareAndSendMyOffer(currentMyOffer, error);

        if (!dex::DexDB::self()->isExistMyOfferByHash(currentMyOffer.hash)) {
            throw runtime_error("\nERROR: the operation failed");
        }

        if (!error.empty()) {
            throw runtime_error("\nERROR: " + error + "\n");
        }
    }

    return NullUniValue;
}

UniValue senddexoffer(const JSONRPCRequest& request)
{
    if (!fTxIndex) {
        throw runtime_error(
            "To use this feature please enable -txindex and make -reindex.\n"
        );
    }

    if (dex::DexDB::self() == nullptr) {
        throw runtime_error(
            "DexDB is not initialized.\n"
        );
    }

    if (request.fHelp || request.params.size() != 1)
        throw runtime_error(

            "senddexoffer <hash>\n\n"

            "\nArgument:\n"
            "\thash    (string) offer hash, hex digest.\n"

            "\nExample:\n"
            + HelpExampleCli("senddexoffer", "AABB...CCDD")
        );

    std::string strOfferHash = request.params[0].get_str();
    if (strOfferHash.empty()) {
        throw runtime_error("\nERROR: offer hash is empty");
    }

    uint256 hash = uint256S(strOfferHash);
    if (hash.IsNull()) {
        throw runtime_error("\nERROR: offer hash error\n");
    }

    if (!dex::DexDB::self()->isExistMyOfferByHash(hash)) {
        throw runtime_error("\nERROR: offer not found in DB\n");
    }

    dex::MyOfferInfo myOffer = dex::DexDB::self()->getMyOfferByHash(hash);

    std::string error;
    //myOffer.timeCreate = GetTime(); error with change hash!!!!
    dex::dexman.prepareAndSendMyOffer(myOffer, error);

    if (!error.empty()) {
        throw runtime_error("\nERROR: " + error + "\n");
    }

    UniValue result(UniValue::VOBJ);
    result.push_back(Pair("new hash", myOffer.hash.GetHex()));
    return result;
}

UniValue dexsync(const JSONRPCRequest& request)
{
    if (!fTxIndex) {
        throw runtime_error(
            "To use this feature please enable -txindex and make -reindex.\n"
        );
    }

    if (dex::DexDB::self() == nullptr) {
        throw runtime_error(
            "DexDB is not initialized.\n"
        );
    }

    if (request.fHelp || request.params.size() != 1)
        throw runtime_error(
                "dexsync [status|reset]\n"
                "if status that returns status synchronization dex\n"

                "\nExample:\n"
                + HelpExampleCli("dexsync", "status")
                );

    UniValue result(UniValue::VOBJ);

    std::string key = request.params[0].get_str();
    if (key == "status") {
        auto status =  dex::dexsync.getSyncStatus();
        result.push_back(Pair("status", status));
    } else if (key == "reset") {
        if (dex::dexsync.reset()) {
           result.push_back(Pair("status", "reset sunc"));
        } else {
           result.push_back(Pair("status", "reset is not available now"));
        }
    } else if (key == "force-synced") {
        dex::dexsync.forceSynced();
        result.push_back(Pair("status", "force synced"));
    } else {
        throw runtime_error("\nwrong parameter " + key + "\n");
    }

    return result;
}

UniValue dexsettings(const JSONRPCRequest& request)
{
    if (!fTxIndex) {
        throw runtime_error(
            "To use this feature please enable -txindex and make -reindex.\n"
        );
    }

    if (dex::DexDB::self() == nullptr) {
        throw runtime_error(
            "DexDB is not initialized.\n"
        );
    }

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2)
        throw runtime_error(
                "dexsettings [maxoutput num]\n"
                "maxoutput return max number output offer dex\n"
                "num - (number, optional) if num not empty changed max number output, if num == 0 show all"

                "\nExample:\n"
                + HelpExampleCli("dexsettings", "maxoutput 100")
                );

    UniValue result(UniValue::VOBJ);

    std::string key = request.params[0].get_str();
    if (key == "maxoutput") {
        int num;
        if (request.params.size() == 2) {
            num = request.params[1].get_int();
            dex::changedMaxOutput(num);
        } else {
            num = dex::maxOutput();
        }

        if (num == 0) {
            result.push_back(Pair("maxoutput", "all"));
        } else {
            result.push_back(Pair("maxoutput", num));
        }
    } else {
        throw runtime_error("\nwrong parameter " + key + "\n");
    }

    return result;
}



UniValue getdexinfo(const JSONRPCRequest& request)
{
    if (!fTxIndex) {
        throw runtime_error(
            "To use this feature please enable -txindex and make -reindex.\n"
        );
    }

    if (dex::DexDB::self() == nullptr) {
        throw runtime_error(
            "DexDB is not initialized.\n"
        );
    }

    if (request.fHelp)
        throw runtime_error(
            "getdexinfo\n"
            "Return short info about offers count in DB."
        );


    UniValue result(UniValue::VOBJ);
    result.push_back(Pair("offersSell", static_cast<uint64_t>(dex::DexDB::self()->countOffersSell())));
    result.push_back(Pair("offersBuy", static_cast<uint64_t>(dex::DexDB::self()->countOffersBuy())));
    result.push_back(Pair("myOffers", static_cast<uint64_t>(dex::DexDB::self()->countMyOffers())));
    result.push_back(Pair("uncOffers", static_cast<uint64_t>(dex::dexman.getUncOffers()->getSize())));
    result.push_back(Pair("uncBcstOffers", static_cast<uint64_t>(dex::dexman.getBcstUncOffers()->getSize())));
    return result;
}

UniValue dexunconfirmed(const JSONRPCRequest& request)
{
    if (!fTxIndex) {
        throw runtime_error(
            "To use this feature please enable -txindex and make -reindex.\n"
        );
    }

    if (dex::DexDB::self() == nullptr) {
        throw runtime_error(
            "DexDB is not initialized.\n"
        );
    }

    if (request.fHelp)
        throw runtime_error(
            "dexunconfirmed\n"
            "Return list pair{hash, idTransaction} unconfirmed offers.\n"
        );

    UniValue result(UniValue::VARR);

    auto bunc = dex::dexman.getBcstUncOffers()->getAllOffers();
    auto unc  = dex::dexman.getUncOffers()->getAllOffers();

    for (auto i : bunc) {
        UniValue v(UniValue::VOBJ);
        v.push_back(Pair("hash", i.hash.GetHex()));
        v.push_back(Pair("txid", i.idTransaction.GetHex()));
        result.push_back(v);
    }

    for (auto i : unc) {
        UniValue v(UniValue::VOBJ);
        v.push_back(Pair("hash", i.hash.GetHex()));
        v.push_back(Pair("txid", i.idTransaction.GetHex()));
        result.push_back(v);
    }


    return result;
}

UniValue getdexoffer(const JSONRPCRequest& request)
{
    if (!fTxIndex) {
        throw runtime_error(
            "To use this feature please enable -txindex and make -reindex.\n"
        );
    }

    if (dex::DexDB::self() == nullptr) {
        throw runtime_error(
            "DexDB is not initialized.\n"
        );
    }

    if (request.fHelp)
        throw runtime_error(
            "getdexoffer <hash>\n"
            "Return detail info about offer.\n"

            "nResult (for example):\n"
            "[\n"
            "   {\n"
            "     \"type\"          : \"sell\",   offer type, buy or sell\n"
            "     \"idTransaction\" : \"<id>\",   transaction with offer fee\n"
            "     \"hash\"          : \"<hash>\", offer hash\n"
            "     \"pubKey\"        : \"<key>\",  offer public key\n"
            "     \"countryIso\"    : \"RU\",     country (ISO 3166-1 alpha-2)\n"
            "     \"currencyIso\"   : \"RUB\",    currency (ISO 4217)\n"
            "     \"paymentMethod\" : 1,        payment method code (default 1 - cash, 128 - online)\n"
            "     \"price\"         : 10000,\n"
            "     \"minAmount\"     : 1000,\n"
            "     \"timeCreate\"    : 947...9344,\n"
            "     \"timeExpiration\": 947...5344, offer expiration\n"
            "     \"shortInfo\"     : \"...\",    offer short info (max 140 bytes)\n"
            "     \"details\"       : \"...\"     offer details (max 1024 bytes)\n"
            "   },\n"
            "   ...\n"
            "]\n"

            "\nExamples:\n"
            + HelpExampleCli("getdexoffer", "AABB...CCDD")
        );

    std::string strOfferHash = request.params[0].get_str();
    if (strOfferHash.empty()) {
        throw runtime_error("\nERROR: offer hash is empty");
    }

    uint256 hash = uint256S(strOfferHash);
    if (hash.IsNull()) {
        throw runtime_error("\nERROR: offer hash error\n");
    }

    dex::CDexOffer offer;
    if (dex::DexDB::self()->isExistOfferSellByHash(hash)) {
        auto info = dex::DexDB::self()->getOfferSellByHash(hash);
        offer = dex::CDexOffer(info, dex::TypeOffer::Sell);
    } else if (dex::DexDB::self()->isExistOfferBuyByHash(hash)) {
        auto info = dex::DexDB::self()->getOfferBuyByHash(hash);
        offer = dex::CDexOffer(info, dex::TypeOffer::Buy);
    } else {
        offer = dex::dexman.getBcstUncOffers()->getOfferByHash(hash);
        if (offer.IsNull()) {
            offer = dex::dexman.getUncOffers()->getOfferByHash(hash);
        }
    }

    if (offer.IsNull()) {
        UniValue result(UniValue::VOBJ);
        return result;
    } else {
        return offer.getUniValue();
    }
}


static const CRPCCommand commands[] = // WARNING: check affter merge branches (add parameters if need)
{ //  category  name                      actor (function)         okSafeMode
  //  --------  ----------------------    ---------------------    ----------
    { "dex",    "dexoffers",              &dexoffers,              true,  {"country","currency","payment_method","limit","offset"} },
    { "dex",    "dexmyoffers",            &dexmyoffers,            true,  {"country","currency","payment_method","status","limit","offset"} },
    { "dex",    "dexofferscount",         &dexofferscount,         true,  {"country","currency","payment_method"} },
    { "dex",    "dexmyofferscount",       &dexmyofferscount,       true,  {"country","currency","payment_method","status"} },
    { "dex",    "deldexoffer",            &deldexoffer,            true,  {"hash"} },
    { "dex",    "adddexoffer",            &adddexoffer,            true,  {"type","countryIso","currencyIso","paymentMethod","price","minAmount","timeToExpiration","shortInfo","details"} },
    { "dex",    "editdexoffer",           &editdexoffer,           true,  {"type","countryIso","currencyIso","paymentMethod","price","minAmount","timeToExpiration","shortInfo","details"} },
    { "dex",    "senddexoffer",           &senddexoffer,           true,  {"hash"} },
    { "dex",    "dexsync",                &dexsync,                true,  {} },
    { "dex",    "dexsettings",            &dexsettings,            true,  {"maxoutput","num"} },
    { "dex",    "getdexinfo",             &getdexinfo,             true,  {} },
    { "dex",    "dexunconfirmed",         &dexunconfirmed,         true,  {} },
    { "dex",    "getdexoffer",            &getdexoffer,            true,  {"hash"} }
};

void RegisterDexRPCCommands(CRPCTable &t)
{
    for (unsigned int vcidx = 0; vcidx < ARRAYLEN(commands); vcidx++)
        t.appendCommand(commands[vcidx].name, &commands[vcidx]);
}

