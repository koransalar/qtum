#include <qtum/nftconfig.h>
#include <chainparamsbase.h>
#include <util/system.h>
#include <util/strencodings.h>

#include <iostream>
#include <regex>

NftConfig::NftConfig()
{
    std::string chainName = gArgs.GetChainName();
    if(chainName == CBaseChainParams::MAIN)
    {
        nftAddress = uint160(ParseHex("4e4d13a577072f0f5cb6fc1a17c96489de0f533e"));
    }
    else if(chainName == CBaseChainParams::TESTNET)
    {
        nftAddress = uint160(ParseHex("16c98b19e66e931b7ada0d5ca41006f33cea5c29"));
    }
    else
    {
        nftAddress = uint160(ParseHex("0000000000000000000000000000000000000000"));
    }

    urlRegex = "(h|H)(t|T)(t|T)(p|P)(s|S)?://.*";
    nUrlMaxLength = 2048;
}

uint160 NftConfig::GetNftAddress() const
{
    return nftAddress;
}

void NftConfig::SetNftAddress(const uint160 &value)
{
    nftAddress = value;
}

const NftConfig &NftConfig::Instance()
{
    static NftConfig config;
    return config;
}

void UpdateNftAddress(const uint160& address)
{
    const_cast<NftConfig&>(NftConfig::Instance()).SetNftAddress(address);
}

bool NftConfig::IsUrlValid(const std::string &sUrl) const
{
    if(sUrl.length() > nUrlMaxLength)
    {
        return false;
    }

    return std::regex_match (sUrl, std::regex(urlRegex));
}

std::string NftConfig::GetUriRegex() const
{
    return urlRegex;
}
