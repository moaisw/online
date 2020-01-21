/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 100 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <config.h>

#include "ProofKey.hpp"
#include "LOOLWSD.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <vector>

#include <Poco/Base64Decoder.h>
#include <Poco/Base64Encoder.h>
#include <Poco/BinaryWriter.h>
#include <Poco/Crypto/RSADigestEngine.h>
#include <Poco/Crypto/RSAKey.h>
#include <Poco/Dynamic/Var.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>
#include <Poco/LineEndingConverter.h>
#include <Poco/Net/HTTPClientSession.h>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/Net/HTTPResponse.h>
#include <Poco/Net/NetException.h>
#include <Poco/StringTokenizer.h>
#include <Poco/Timestamp.h>
#include <Poco/URI.h>
#include <Poco/Util/Application.h>

#include <Log.hpp>
#include <Util.hpp>

namespace{

std::vector<unsigned char> getBytesLE(const unsigned char* bytesInSystemOrder, const size_t n)
{
    std::vector<unsigned char> ret(n);
#if !defined __BYTE_ORDER__
    static_assert(false, "Byte order is not detected on this platform!");
#elif __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    std::copy_n(bytesInSystemOrder, n, ret.begin());
#else
    std::copy_n(bytesInSystemOrder, n, ret.rbegin());
#endif
    return ret;
}

// Returns a number as vector of bytes (little-endian)
template <typename T>
std::vector<unsigned char> getBytesLE(const T& x)
{
    return getBytesLE(reinterpret_cast<const unsigned char*>(&x), sizeof(x));
}

// https://docs.microsoft.com/en-us/openspecs/windows_protocols/ms-mqqb/ade9efde-3ec8-4e47-9ae9-34b64d8081bb
std::vector<unsigned char> RSA2CapiBlob(const std::vector<unsigned char>& modulus,
                                        const std::vector<unsigned char>& exponent)
{
    std::vector<unsigned char> capiBlob = {
        0x06, 0x02, 0x00, 0x00,
        0x00, 0xA4, 0x00, 0x00,
        0x52, 0x53, 0x41, 0x31,
    };
    // modulus size in bits - 4 bytes (little-endian)
    const auto bitLen = getBytesLE<std::uint32_t>(modulus.size() * 8);
    capiBlob.reserve(capiBlob.size() + bitLen.size() + exponent.size() + modulus.size());
    std::copy(bitLen.begin(), bitLen.end(), std::back_inserter(capiBlob));
    // exponent - 4 bytes (little-endian)
    std::copy(exponent.rbegin(), exponent.rend(), std::back_inserter(capiBlob));
    // modulus (little-endian)
    std::copy(modulus.rbegin(), modulus.rend(), std::back_inserter(capiBlob));
    return capiBlob;
}

std::string BytesToBase64(const std::vector<unsigned char>& bytes)
{
    std::ostringstream oss;
    // The signature generated contains CRLF line endings.
    // Use a line ending converter to remove these CRLF
    Poco::OutputLineEndingConverter lineEndingConv(oss, "");
    Poco::Base64Encoder encoder(lineEndingConv);
    encoder << std::string(bytes.begin(), bytes.end());
    encoder.close();
    return oss.str();
}

class Proof {
public:
    Proof();
    VecOfStringPairs GetProofHeaders(const std::string& access_token, const std::string& uri) const;
    const VecOfStringPairs& GetProofKeyAttributes() const { return m_aAttribs; }
private:
    static std::string ProofKeyPath();

    // Returns .Net tick (=100ns) count since 0001-01-01 00:00:00 Z
    // See https://docs.microsoft.com/en-us/dotnet/api/system.datetime.ticks
    static int64_t DotNetTicks(const std::chrono::system_clock::time_point& utc);
    // Returns string of bytes to sign and base64-encode
    // See http://www.wictorwilen.se/sharepoint-2013-building-your-own-wopi-client-part-2
    static std::string GetProof(const std::string& access_token, const std::string& uri, int64_t ticks);
    // Signs string of bytes and returns base64-encoded string
    std::string SignProof(const std::string& proof) const;

    const std::unique_ptr<const Poco::Crypto::RSAKey> m_pKey;
    VecOfStringPairs m_aAttribs;
};

Proof::Proof()
    : m_pKey([]() -> Poco::Crypto::RSAKey* {
        try
        {
            return new Poco::Crypto::RSAKey("", ProofKeyPath());
        }
        catch (const Poco::Exception& e)
        {
            LOG_ERR("Could not open proof RSA key: " << e.displayText());
        }
        catch (const std::exception& e)
        {
            LOG_ERR("Could not open proof RSA key: " << e.what());
        }
        catch (...)
        {
            LOG_ERR("Could not open proof RSA key: unknown exception");
        }
        return nullptr;
    }())
{
    if (m_pKey)
    {
        const auto m = m_pKey->modulus();
        const auto e = m_pKey->encryptionExponent();
        const auto capiBlob = RSA2CapiBlob(m, e);

        m_aAttribs.emplace_back("value", BytesToBase64(capiBlob));
        m_aAttribs.emplace_back("modulus", BytesToBase64(m));
        m_aAttribs.emplace_back("exponent", BytesToBase64(e));
    }
}

std::string Proof::ProofKeyPath()
{
    const std::string keyPath = LOOLWSD_CONFIGDIR "/proof_key";
    if (!Poco::File(keyPath).exists())
    {
        std::string msg = "Could not find " + keyPath +
            "\nNo proof-key will be present in discovery."
            "\nGenerate an RSA key using this command line:"
            "\n    ssh-keygen -t rsa -N \"\" -f \"" + keyPath + "\"";
        fprintf(stderr, "%s\n", msg.c_str());
        LOG_WRN(msg);
    }

    return keyPath;
}

int64_t Proof::DotNetTicks(const std::chrono::system_clock::time_point& utc)
{
    // Get time point for Unix epoch; unfortunately from_time_t isn't constexpr
    const auto aUnxEpoch(std::chrono::system_clock::from_time_t(0));
    const auto duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(utc - aUnxEpoch);
    return duration_ns.count() / 100 + 621355968000000000;
}

std::string Proof::GetProof(const std::string& access_token, const std::string& uri, int64_t ticks)
{
    std::string decoded_access_token;
    Poco::URI::decode(access_token, decoded_access_token);
    assert(decoded_access_token.size() <= static_cast<size_t>(std::numeric_limits<int32_t>::max()));
    assert(uri.size() <= static_cast<size_t>(std::numeric_limits<int32_t>::max()));
    const size_t size = 4 + decoded_access_token.size() + 4 + uri.size() + 4 + 8;
    Poco::Buffer<char> buffer(size); // allocate enough size
    buffer.resize(0); // start from empty buffer
    Poco::MemoryBinaryWriter writer(buffer, Poco::BinaryWriter::NETWORK_BYTE_ORDER);
    writer << static_cast<int32_t>(decoded_access_token.size())
           << decoded_access_token
           << static_cast<int32_t>(uri.size())
           << uri
           << int32_t(8)
           << ticks;
    assert(buffer.size() == size);
    return std::string(buffer.begin(), buffer.end());
}

std::string Proof::SignProof(const std::string& proof) const
{
    assert(m_pKey);
    std::ostringstream ostr;
    static Poco::Crypto::RSADigestEngine digestEngine(*m_pKey, "SHA256");
    digestEngine.update(proof.c_str(), proof.length());
    Poco::Crypto::DigestEngine::Digest digest = digestEngine.signature();
    // The signature generated contains CRLF line endings.
    // Use a line ending converter to remove these CRLF
    Poco::OutputLineEndingConverter lineEndingConv(ostr, "");
    Poco::Base64Encoder encoder(lineEndingConv);
    encoder << std::string(digest.begin(), digest.end());
    encoder.close();
    return ostr.str();
}

VecOfStringPairs Proof::GetProofHeaders(const std::string& access_token, const std::string& uri) const
{
    VecOfStringPairs vec;
    if (m_pKey)
    {
        int64_t ticks = DotNetTicks(std::chrono::system_clock::now());
        vec.emplace_back("X-WOPI-TimeStamp", std::to_string(ticks));
        vec.emplace_back("X-WOPI-Proof", SignProof(GetProof(access_token, uri, ticks)));
    }
    return vec;
}

const Proof& GetProof()
{
    static const Proof proof;
    return proof;
}

}

VecOfStringPairs GetProofHeaders(const std::string& access_token, const std::string& uri)
{
    return GetProof().GetProofHeaders(access_token, uri);
}

const VecOfStringPairs& GetProofKeyAttributes()
{
    return GetProof().GetProofKeyAttributes();
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
