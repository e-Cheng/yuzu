// Copyright 2017 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <cstring>

#include "core/hle/service/nwm/nwm_uds.h"
#include "core/hle/service/nwm/uds_beacon.h"
#include "core/hle/service/nwm/uds_data.h"
#include "core/hw/aes/key.h"

#include <cryptopp/ccm.h>
#include <cryptopp/filters.h>
#include <cryptopp/md5.h>
#include <cryptopp/modes.h>

namespace Service {
namespace NWM {

// AES Keyslot used to generate the UDS data frame CCMP key.
constexpr size_t UDSDataCryptoAESKeySlot = 0x2D;

/*
 * Generates a SNAP-enabled 802.2 LLC header for the specified protocol.
 * @returns a buffer with the bytes of the generated header.
 */
static std::vector<u8> GenerateLLCHeader(EtherType protocol) {
    LLCHeader header{};
    header.protocol = static_cast<u16>(protocol);

    std::vector<u8> buffer(sizeof(header));
    memcpy(buffer.data(), &header, sizeof(header));

    return buffer;
}

/*
 * Generates a Nintendo UDS SecureData header with the specified parameters.
 * @returns a buffer with the bytes of the generated header.
 */
static std::vector<u8> GenerateSecureDataHeader(u16 data_size, u8 channel, u16 dest_node_id,
    u16 src_node_id, u16 sequence_number) {
    SecureDataHeader header{};
    header.protocol_size = data_size + sizeof(SecureDataHeader);
    // Note: This size includes everything except the first 4 bytes of the structure,
    // reinforcing the hypotheses that the first 4 bytes are actually the header of
    // another container protocol.
    header.securedata_size = data_size + sizeof(SecureDataHeader) - 4;
    header.is_management = 0; // Frames sent by the emulated application are never UDS management frames
    header.data_channel = channel;
    header.sequence_number = sequence_number;
    header.dest_node_id = dest_node_id;
    header.src_node_id = src_node_id;

    std::vector<u8> buffer(sizeof(header));
    memcpy(buffer.data(), &header, sizeof(header));

    return buffer;
}

/*
 * Calculates the CTR used for the AES-CTR process that calculates
  * the CCMP crypto key for data frames.
 * @returns The CTR used for data frames crypto key generation.
 */
static std::array<u8, CryptoPP::MD5::DIGESTSIZE> GetDataCryptoCTR(const NetworkInfo& network_info) {
    DataFrameCryptoCTR data{};

    data.host_mac = network_info.host_mac_address;
    data.wlan_comm_id = network_info.wlan_comm_id;
    data.id = network_info.id;
    data.network_id = network_info.network_id;

    std::array<u8, CryptoPP::MD5::DIGESTSIZE> hash;
    CryptoPP::MD5().CalculateDigest(hash.data(), reinterpret_cast<u8*>(&data), sizeof(data));

    return hash;
}

/*
 * Generates the key used for encrypting the 802.11 data frames generated by UDS.
 * @returns The key used for data frames crypto.
 */
static std::array<u8, CryptoPP::AES::BLOCKSIZE> GenerateDataCCMPKey(const std::vector<u8>& passphrase,
    const NetworkInfo& network_info) {
    // Calculate the MD5 hash of the input passphrase.
    std::array<u8, CryptoPP::MD5::DIGESTSIZE> passphrase_hash;
    CryptoPP::MD5().CalculateDigest(passphrase_hash.data(), passphrase.data(), passphrase.size());

    std::array<u8, CryptoPP::AES::BLOCKSIZE> ccmp_key;

    // The CCMP key is the result of encrypting the MD5 hash of the passphrase with AES-CTR using keyslot 0x2D.
    using CryptoPP::AES;
    std::array<u8, CryptoPP::MD5::DIGESTSIZE> counter = GetDataCryptoCTR(network_info);
    std::array<u8, AES::BLOCKSIZE> key = HW::AES::GetNormalKey(UDSDataCryptoAESKeySlot);
    CryptoPP::CTR_Mode<AES>::Encryption aes;
    aes.SetKeyWithIV(key.data(), AES::BLOCKSIZE, counter.data());
    aes.ProcessData(ccmp_key.data(), passphrase_hash.data(), passphrase_hash.size());

    return ccmp_key;
}

/*
 * Generates the Additional Authenticated Data (AAD) for an UDS 802.11 encrypted data frame.
 * @returns a buffer with the bytes of the AAD.
 */
static std::vector<u8> GenerateCCMPAAD(const MacAddress& sender, const MacAddress& receiver) {
    // Reference: IEEE 802.11-2007

    // 8.3.3.3.2 Construct AAD (22-30 bytes)
    // The AAD is constructed from the MPDU header. The AAD does not include the header Duration
    // field, because the Duration field value can change due to normal IEEE 802.11 operation (e.g.,
    // a rate change during retransmission). For similar reasons, several subfields in the Frame
    // Control field are masked to 0.
    struct {
        u16_be FC; // MPDU Frame Control field
        MacAddress receiver;
        MacAddress transmitter;
        MacAddress destination;
        u16_be SC; // MPDU Sequence Control field
    } aad_struct{};

    // Default FC value of DataFrame | Protected | ToDS
    constexpr u16 DefaultFrameControl = 0x0841;

    aad_struct.FC = DefaultFrameControl;
    aad_struct.SC = 0;
    aad_struct.transmitter = sender;
    aad_struct.receiver = receiver;
    aad_struct.destination = receiver;

    std::vector<u8> aad(sizeof(aad_struct));
    std::memcpy(aad.data(), &aad_struct, sizeof(aad_struct));

    return aad;
}

/*
 * Decrypts the payload of an encrypted 802.11 data frame using the specified key.
 * @returns The decrypted payload.
 */
static std::vector<u8> DecryptDataFrame(const std::vector<u8>& encrypted_payload, const std::array<u8, CryptoPP::AES::BLOCKSIZE>& ccmp_key,
    const MacAddress& sender, const MacAddress& receiver, u16 sequence_number) {

    // Reference: IEEE 802.11-2007

    std::vector<u8> aad = GenerateCCMPAAD(sender, receiver);

    std::vector<u8> packet_number{0, 0, 0, 0,
                                  static_cast<u8>((sequence_number >> 8) & 0xFF),
                                  static_cast<u8>(sequence_number & 0xFF)};

    // 8.3.3.3.3 Construct CCM nonce (13 bytes)
    std::vector<u8> nonce;
    nonce.push_back(0); // priority
    nonce.insert(nonce.end(), sender.begin(), sender.end()); // Address 2
    nonce.insert(nonce.end(), packet_number.begin(), packet_number.end()); // PN

    try {
        CryptoPP::CCM<CryptoPP::AES, 8>::Decryption d;
        d.SetKeyWithIV(ccmp_key.data(), ccmp_key.size(), nonce.data(), nonce.size());
        d.SpecifyDataLengths(aad.size(), encrypted_payload.size() - 8, 0);

        CryptoPP::AuthenticatedDecryptionFilter df(d, nullptr,
            CryptoPP::AuthenticatedDecryptionFilter::MAC_AT_END |
                CryptoPP::AuthenticatedDecryptionFilter::THROW_EXCEPTION);
        // put aad
        df.ChannelPut(CryptoPP::AAD_CHANNEL, aad.data(), aad.size());

        // put cipher with mac
        df.ChannelPut(CryptoPP::DEFAULT_CHANNEL, encrypted_payload.data(), encrypted_payload.size() - 8);
        df.ChannelPut(CryptoPP::DEFAULT_CHANNEL, encrypted_payload.data() + encrypted_payload.size() - 8, 8);

        df.ChannelMessageEnd(CryptoPP::AAD_CHANNEL);
        df.ChannelMessageEnd(CryptoPP::DEFAULT_CHANNEL);
        df.SetRetrievalChannel(CryptoPP::DEFAULT_CHANNEL);

        int size = df.MaxRetrievable();

        std::vector<u8> pdata(size);
        df.Get(pdata.data(), size);
        return pdata;
    } catch (CryptoPP::Exception&) {
        LOG_ERROR(Service_NWM, "failed to decrypt");
    }

    return {};
}

/*
 * Encrypts the payload of an 802.11 data frame using the specified key.
 * @returns The encrypted payload.
 */
static std::vector<u8> EncryptDataFrame(const std::vector<u8>& payload, const std::array<u8, CryptoPP::AES::BLOCKSIZE>& ccmp_key,
                                 const MacAddress& sender, const MacAddress& receiver, u16 sequence_number) {
    // Reference: IEEE 802.11-2007

    std::vector<u8> aad = GenerateCCMPAAD(sender, receiver);

    std::vector<u8> packet_number{0, 0, 0, 0,
        static_cast<u8>((sequence_number >> 8) & 0xFF),
        static_cast<u8>(sequence_number & 0xFF)};

    // 8.3.3.3.3 Construct CCM nonce (13 bytes)
    std::vector<u8> nonce;
    nonce.push_back(0); // priority
    nonce.insert(nonce.end(), sender.begin(), sender.end()); // Address 2
    nonce.insert(nonce.end(), packet_number.begin(), packet_number.end()); // PN

    try {
        CryptoPP::CCM<CryptoPP::AES, 8>::Encryption d;
        d.SetKeyWithIV(ccmp_key.data(), ccmp_key.size(), nonce.data(), nonce.size());
        d.SpecifyDataLengths(aad.size(), payload.size(), 0);

        CryptoPP::AuthenticatedEncryptionFilter df(d);
        // put aad
        df.ChannelPut(CryptoPP::AAD_CHANNEL, aad.data(), aad.size());
        df.ChannelMessageEnd(CryptoPP::AAD_CHANNEL);

        // put plaintext
        df.ChannelPut(CryptoPP::DEFAULT_CHANNEL, payload.data(), payload.size());
        df.ChannelMessageEnd(CryptoPP::DEFAULT_CHANNEL);

        df.SetRetrievalChannel(CryptoPP::DEFAULT_CHANNEL);

        int size = df.MaxRetrievable();

        std::vector<u8> cipher(size);
        df.Get(cipher.data(), size);
        return cipher;
    } catch (CryptoPP::Exception&) {
        LOG_ERROR(Service_NWM, "failed to encrypt");
    }

    return {};
}

std::vector<u8> GenerateDataPayload(const std::vector<u8>& data, u8 channel, u16 dest_node, u16 src_node,
    u16 sequence_number) {
    std::vector<u8> buffer = GenerateLLCHeader(EtherType::SecureData);
    std::vector<u8> securedata_header = GenerateSecureDataHeader(data.size(), channel, dest_node, src_node,
                                                                 sequence_number);

    buffer.insert(buffer.end(), securedata_header.begin(), securedata_header.end());
    buffer.insert(buffer.end(), data.begin(), data.end());
    return buffer;
}

} // namespace NWM
} // namespace Service
