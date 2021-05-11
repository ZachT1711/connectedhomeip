/*
 *
 *    Copyright (c) 2021 Project CHIP Authors
 *    All rights reserved.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

/**
 *    @file
 *      This file implements the the CHIP CASE Session object that provides
 *      APIs for constructing a secure session using a certificate from the device's
 *      operational credentials.
 *
 */
#include <protocols/secure_channel/CASESession.h>

#include <inttypes.h>
#include <string.h>

#include <core/CHIPEncoding.h>
#include <core/CHIPSafeCasts.h>
#include <protocols/Protocols.h>
#include <support/BufferWriter.h>
#include <support/CHIPMem.h>
#include <support/CodeUtils.h>
#include <support/SafeInt.h>
#include <transport/SecureSessionMgr.h>

namespace chip {

// TODO: Remove Later
static P256ECDHDerivedSecret fabricSecret;

constexpr uint8_t kIPKInfo[] = { 0x49, 0x64, 0x65, 0x6e, 0x74, 0x69, 0x74, 0x79, 0x50, 0x72, 0x6f,
                                 0x74, 0x65, 0x63, 0x74, 0x69, 0x6f, 0x6e, 0x4b, 0x65, 0x79 };

constexpr uint8_t kKDFSR2Info[]   = { 0x53, 0x69, 0x67, 0x6d, 0x61, 0x52, 0x32 };
constexpr uint8_t kKDFSR3Info[]   = { 0x53, 0x69, 0x67, 0x6d, 0x61, 0x52, 0x33 };
constexpr size_t kKDFInfoLength   = sizeof(kKDFSR2Info);
constexpr uint8_t kKDFSEInfo[]    = { 0x53, 0x65, 0x73, 0x73, 0x69, 0x6f, 0x6e, 0x4b, 0x65, 0x79, 0x73 };
constexpr size_t kKDFSEInfoLength = sizeof(kKDFSEInfo);

constexpr uint8_t kIVSR2[] = { 0x4e, 0x43, 0x41, 0x53, 0x45, 0x5f, 0x53, 0x69, 0x67, 0x6d, 0x61, 0x52, 0x32 };
constexpr uint8_t kIVSR3[] = { 0x4e, 0x43, 0x41, 0x53, 0x45, 0x5f, 0x53, 0x69, 0x67, 0x6d, 0x61, 0x52, 0x33 };
constexpr size_t kIVLength = sizeof(kIVSR2);

constexpr size_t kTAGSize = 16;

using namespace Crypto;
using namespace Credentials;
using namespace Messaging;

// Wait at most 30 seconds for the response from the peer.
// This timeout value assumes the underlying transport is reliable.
// The session establishment fails if the response is not received within timeout window.
static constexpr ExchangeContext::Timeout kSigma_Response_Timeout = 30000;

CASESession::CASESession()
{
    mTrustedRootId.mId = nullptr;
    // dummy initialization REMOVE LATER
    for (size_t i = 0; i < fabricSecret.Capacity(); i++)
    {
        fabricSecret[i] = static_cast<uint8_t>(i);
    }
    fabricSecret.SetLength(fabricSecret.Capacity());
}

CASESession::~CASESession()
{
    // Let's clear out any security state stored in the object, before destroying it.
    Clear();
}

void CASESession::Clear()
{
    // This function zeroes out and resets the memory used by the object.
    // It's done so that no security related information will be leaked.
    mNextExpectedMsg = Protocols::SecureChannel::MsgType::CASE_SigmaErr;
    mCommissioningHash.Clear();
    mPairingComplete = false;
    mConnectionState.Reset();
    if (mTrustedRootId.mId != nullptr)
    {
        chip::Platform::MemoryFree(const_cast<uint8_t *>(mTrustedRootId.mId));
        mTrustedRootId.mId = nullptr;
    }

    if (mExchangeCtxt != nullptr)
    {
        mExchangeCtxt->Close();
        mExchangeCtxt = nullptr;
    }
}

CHIP_ERROR CASESession::Serialize(CASESessionSerialized & output)
{
    uint16_t serializedLen = 0;
    CASESessionSerializable serializable;

    VerifyOrReturnError(BASE64_ENCODED_LEN(sizeof(serializable)) <= sizeof(output.inner), CHIP_ERROR_INVALID_ARGUMENT);

    ReturnErrorOnFailure(ToSerializable(serializable));

    serializedLen = chip::Base64Encode(Uint8::to_const_uchar(reinterpret_cast<uint8_t *>(&serializable)),
                                       static_cast<uint16_t>(sizeof(serializable)), Uint8::to_char(output.inner));
    VerifyOrReturnError(serializedLen > 0, CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrReturnError(serializedLen < sizeof(output.inner), CHIP_ERROR_INVALID_ARGUMENT);
    output.inner[serializedLen] = '\0';

    return CHIP_NO_ERROR;
}

CHIP_ERROR CASESession::Deserialize(CASESessionSerialized & input)
{
    CASESessionSerializable serializable;
    size_t maxlen            = BASE64_ENCODED_LEN(sizeof(serializable));
    size_t len               = strnlen(Uint8::to_char(input.inner), maxlen);
    uint16_t deserializedLen = 0;

    VerifyOrReturnError(len < sizeof(CASESessionSerialized), CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrReturnError(CanCastTo<uint16_t>(len), CHIP_ERROR_INVALID_ARGUMENT);

    memset(&serializable, 0, sizeof(serializable));
    deserializedLen =
        Base64Decode(Uint8::to_const_char(input.inner), static_cast<uint16_t>(len), Uint8::to_uchar((uint8_t *) &serializable));

    VerifyOrReturnError(deserializedLen > 0, CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrReturnError(deserializedLen <= sizeof(serializable), CHIP_ERROR_INVALID_ARGUMENT);

    ReturnErrorOnFailure(FromSerializable(serializable));

    return CHIP_NO_ERROR;
}

CHIP_ERROR CASESession::ToSerializable(CASESessionSerializable & serializable)
{
    const NodeId peerNodeId = mConnectionState.GetPeerNodeId();
    VerifyOrReturnError(CanCastTo<uint16_t>(mSharedSecret.Length()), CHIP_ERROR_INTERNAL);
    VerifyOrReturnError(CanCastTo<uint16_t>(sizeof(mMessageDigest)), CHIP_ERROR_INTERNAL);
    VerifyOrReturnError(CanCastTo<uint64_t>(peerNodeId), CHIP_ERROR_INTERNAL);

    memset(&serializable, 0, sizeof(serializable));
    serializable.mSharedSecretLen  = static_cast<uint16_t>(mSharedSecret.Length());
    serializable.mMessageDigestLen = static_cast<uint16_t>(sizeof(mMessageDigest));
    serializable.mPairingComplete  = (mPairingComplete) ? 1 : 0;
    serializable.mPeerNodeId       = peerNodeId;
    serializable.mLocalKeyId       = mConnectionState.GetLocalKeyID();
    serializable.mPeerKeyId        = mConnectionState.GetPeerKeyID();

    memcpy(serializable.mSharedSecret, mSharedSecret, mSharedSecret.Length());
    memcpy(serializable.mMessageDigest, mMessageDigest, sizeof(mMessageDigest));

    return CHIP_NO_ERROR;
}

CHIP_ERROR CASESession::FromSerializable(const CASESessionSerializable & serializable)
{
    mPairingComplete = (serializable.mPairingComplete == 1);
    ReturnErrorOnFailure(mSharedSecret.SetLength(static_cast<size_t>(serializable.mSharedSecretLen)));

    VerifyOrReturnError(serializable.mMessageDigestLen <= sizeof(mMessageDigest), CHIP_ERROR_INVALID_ARGUMENT);

    memset(mSharedSecret, 0, sizeof(mSharedSecret.Capacity()));
    memcpy(mSharedSecret, serializable.mSharedSecret, mSharedSecret.Length());
    memcpy(mMessageDigest, serializable.mMessageDigest, serializable.mMessageDigestLen);

    mConnectionState.SetPeerNodeId(serializable.mPeerNodeId);
    mConnectionState.SetLocalKeyID(serializable.mLocalKeyId);
    mConnectionState.SetPeerKeyID(serializable.mPeerKeyId);

    return CHIP_NO_ERROR;
}

CHIP_ERROR CASESession::Init(OperationalCredentialSet * operationalCredentialSet, uint16_t myKeyId,
                             SessionEstablishmentDelegate * delegate)
{
    VerifyOrReturnError(delegate != nullptr, CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrReturnError(operationalCredentialSet->GetCertCount() > 0, CHIP_ERROR_CERT_NOT_FOUND);

    Clear();

    ReturnErrorOnFailure(mCommissioningHash.Begin());

    mDelegate = delegate;
    mConnectionState.SetLocalKeyID(myKeyId);
    mOpCredSet = operationalCredentialSet;

    mValidContext.Reset();
    mValidContext.mRequiredKeyUsages.Set(KeyUsageFlags::kDigitalSignature);
    mValidContext.mRequiredKeyPurposes.Set(KeyPurposeFlags::kServerAuth);

    return CHIP_NO_ERROR;
}

CHIP_ERROR
CASESession::WaitForSessionEstablishment(OperationalCredentialSet * operationalCredentialSet, uint16_t myKeyId,
                                         SessionEstablishmentDelegate * delegate)
{
    ReturnErrorOnFailure(Init(operationalCredentialSet, myKeyId, delegate));

    mNextExpectedMsg = Protocols::SecureChannel::MsgType::CASE_SigmaR1;
    mPairingComplete = false;

    ChipLogDetail(Inet, "Waiting for SigmaR1 msg");

    return CHIP_NO_ERROR;
}

CHIP_ERROR CASESession::EstablishSession(const Transport::PeerAddress peerAddress,
                                         OperationalCredentialSet * operationalCredentialSet, NodeId peerNodeId, uint16_t myKeyId,
                                         ExchangeContext * exchangeCtxt, SessionEstablishmentDelegate * delegate)
{
    CHIP_ERROR err = CHIP_NO_ERROR;

    ReturnErrorCodeIf(exchangeCtxt == nullptr, CHIP_ERROR_INVALID_ARGUMENT);

    err = Init(operationalCredentialSet, myKeyId, delegate);

    // We are setting the exchange context specifically before checking for error.
    // This is to make sure the exchange will get closed if Init() returned an error.
    mExchangeCtxt = exchangeCtxt;
    SuccessOrExit(err);

    mExchangeCtxt->SetResponseTimeout(kSigma_Response_Timeout);
    mConnectionState.SetPeerAddress(peerAddress);
    mConnectionState.SetPeerNodeId(peerNodeId);

    err = SendSigmaR1();
    SuccessOrExit(err);

exit:
    if (err != CHIP_NO_ERROR)
    {
        Clear();
    }
    return err;
}

void CASESession::OnResponseTimeout(ExchangeContext * ec)
{
    VerifyOrReturn(ec != nullptr, ChipLogError(Inet, "CASESession::OnResponseTimeout was called by null exchange"));
    VerifyOrReturn(mExchangeCtxt == ec, ChipLogError(Inet, "CASESession::OnResponseTimeout exchange doesn't match"));
    ChipLogError(Inet, "CASESession timed out while waiting for a response from the peer. Expected message type was %d",
                 mNextExpectedMsg);
    mDelegate->OnSessionEstablishmentError(CHIP_ERROR_TIMEOUT);
    Clear();
}

CHIP_ERROR CASESession::DeriveSecureSession(SecureSession & session, SecureSession::SessionRole role)
{
    System::PacketBufferHandle msg_salt;
    uint16_t saltlen;

    (void) kKDFSEInfo;
    (void) kKDFSEInfoLength;

    VerifyOrReturnError(mPairingComplete, CHIP_ERROR_INCORRECT_STATE);

    // Generate Salt for Encryption keys
    saltlen = kSHA256_Hash_Length;

    msg_salt = System::PacketBufferHandle::New(saltlen);
    VerifyOrReturnError(!msg_salt.IsNull(), CHIP_SYSTEM_ERROR_NO_MEMORY);
    {
        Encoding::LittleEndian::BufferWriter bbuf(msg_salt->Start(), saltlen);
        // TODO: Add IPK to Salt
        bbuf.Put(mMessageDigest, sizeof(mMessageDigest));

        VerifyOrReturnError(bbuf.Fit(), CHIP_ERROR_NO_MEMORY);
    }

    ReturnErrorOnFailure(session.InitFromSecret(ByteSpan(mSharedSecret, mSharedSecret.Length()),
                                                ByteSpan(msg_salt->Start(), saltlen),
                                                SecureSession::SessionInfoType::kSessionEstablishment, role));

    return CHIP_NO_ERROR;
}

CHIP_ERROR CASESession::SendSigmaR1()
{
    uint16_t data_len = static_cast<uint16_t>(kSigmaParamRandomNumberSize + sizeof(uint16_t) + sizeof(uint16_t) +
                                              mOpCredSet->GetCertCount() * kTrustedRootIdSize + kP256_PublicKey_Length);

    System::PacketBufferHandle msg_R1;
    uint8_t * msg = nullptr;

    msg_R1 = System::PacketBufferHandle::New(data_len);
    VerifyOrReturnError(!msg_R1.IsNull(), CHIP_SYSTEM_ERROR_NO_MEMORY);

    msg = msg_R1->Start();

    // Step 1
    // Fill in the random value
    ReturnErrorOnFailure(DRBG_get_bytes(msg, kSigmaParamRandomNumberSize));

    // Step 4
    ReturnErrorOnFailure(mEphemeralKey.Initialize());

    // Step 5
    // Let's construct the rest of the message using Encoding::LittleEndian::BufferWriter
    {
        uint16_t n_trusted_roots = mOpCredSet->GetCertCount();
        Encoding::LittleEndian::BufferWriter bbuf(&msg[kSigmaParamRandomNumberSize], data_len - kSigmaParamRandomNumberSize);
        // Initiator's session ID
        bbuf.Put16(mConnectionState.GetLocalKeyID());
        // Step 2/3
        bbuf.Put16(n_trusted_roots);
        for (uint16_t i = 0; i < n_trusted_roots; ++i)
        {
            bbuf.Put(mOpCredSet->GetTrustedRootId(i)->mId, kTrustedRootIdSize);
        }
        bbuf.Put(mEphemeralKey.Pubkey(), mEphemeralKey.Pubkey().Length());
        VerifyOrReturnError(bbuf.Fit(), CHIP_ERROR_NO_MEMORY);
    }

    msg_R1->SetDataLength(data_len);

    ReturnErrorOnFailure(mCommissioningHash.AddData(msg_R1->Start(), msg_R1->DataLength()));

    ReturnErrorOnFailure(ComputeIPK(mConnectionState.GetLocalKeyID(), mIPK, sizeof(mIPK)));

    mNextExpectedMsg = Protocols::SecureChannel::MsgType::CASE_SigmaR2;

    // Call delegate to send the msg to peer
    ReturnErrorOnFailure(mExchangeCtxt->SendMessage(Protocols::SecureChannel::MsgType::CASE_SigmaR1, std::move(msg_R1),
                                                    SendFlags(SendMessageFlags::kExpectResponse)));

    ChipLogDetail(Inet, "Sent SigmaR1 msg");

    return CHIP_NO_ERROR;
}

CHIP_ERROR CASESession::HandleSigmaR1_and_SendSigmaR2(const System::PacketBufferHandle & msg)
{
    ReturnErrorOnFailure(HandleSigmaR1(msg));
    ReturnErrorOnFailure(SendSigmaR2());

    return CHIP_NO_ERROR;
}

CHIP_ERROR CASESession::HandleSigmaR1(const System::PacketBufferHandle & msg)
{
    CHIP_ERROR err = CHIP_NO_ERROR;

    uint16_t encryptionKeyId = 0;

    const uint8_t * buf = msg->Start();
    uint16_t buflen     = msg->DataLength();
    uint16_t fixed_buflen =
        kSigmaParamRandomNumberSize + sizeof(encryptionKeyId) + sizeof(uint16_t) + kTrustedRootIdSize + kP256_PublicKey_Length;
    uint32_t n_trusted_roots;

    Encoding::LittleEndian::BufferWriter bbuf(mRemotePubKey, mRemotePubKey.Length());

    VerifyOrExit(buf != nullptr, err = CHIP_ERROR_MESSAGE_INCOMPLETE);
    VerifyOrExit(buflen >= fixed_buflen, err = CHIP_ERROR_INVALID_MESSAGE_LENGTH);

    ChipLogDetail(Inet, "Received SigmaR1 msg");

    err = mCommissioningHash.AddData(msg->Start(), msg->DataLength());
    SuccessOrExit(err);

    // Let's skip the random number portion of the message
    buf += kSigmaParamRandomNumberSize;

    encryptionKeyId = chip::Encoding::LittleEndian::Read16(buf);
    n_trusted_roots = chip::Encoding::LittleEndian::Read16(buf);
    // Step 1/2
    err = FindValidTrustedRoot(&buf, n_trusted_roots);
    SuccessOrExit(err);
    // write public key from message
    bbuf.Put(buf, kP256_PublicKey_Length);
    VerifyOrExit(bbuf.Fit(), err = CHIP_ERROR_NO_MEMORY);

    ChipLogDetail(Inet, "Peer assigned session key ID %d", encryptionKeyId);
    mConnectionState.SetPeerKeyID(encryptionKeyId);

exit:

    if (err == CHIP_ERROR_CERT_NOT_TRUSTED)
    {
        SendErrorMsg(SigmaErrorType::kNoSharedTrustRoots);
    }
    else if (err != CHIP_NO_ERROR)
    {
        SendErrorMsg(SigmaErrorType::kUnexpected);
    }
    return err;
}

CHIP_ERROR CASESession::SendSigmaR2()
{
    CHIP_ERROR err = CHIP_NO_ERROR;

    System::PacketBufferHandle msg_R2;
    uint16_t data_len;

    System::PacketBufferHandle msg_rand;

    System::PacketBufferHandle msg_R2_Signed;
    uint16_t msg_r2_signed_len;

    System::PacketBufferHandle msg_R2_Encrypted;
    size_t msg_r2_signed_enc_len;

    System::PacketBufferHandle msg_salt;
    uint16_t saltlen;

    uint8_t sr2k[kAEADKeySize];
    P256ECDSASignature sigmaR2Signature;

    uint8_t tag[kTAGSize];

    saltlen = kIPKSize + kSigmaParamRandomNumberSize + kP256_PublicKey_Length + kSHA256_Hash_Length;

    msg_salt = System::PacketBufferHandle::New(saltlen);
    VerifyOrExit(!msg_salt.IsNull(), err = CHIP_SYSTEM_ERROR_NO_MEMORY);
    msg_salt->SetDataLength(saltlen);

    msg_rand = System::PacketBufferHandle::New(kSigmaParamRandomNumberSize);
    VerifyOrExit(!msg_rand.IsNull(), err = CHIP_SYSTEM_ERROR_NO_MEMORY);

    // Step 1
    // Fill in the random value
    err = DRBG_get_bytes(msg_rand->Start(), kSigmaParamRandomNumberSize);
    SuccessOrExit(err);

    msg_rand->SetDataLength(kSigmaParamRandomNumberSize);

    // Step 3
    // hardcoded to use a p256keypair
    err = mEphemeralKey.Initialize();
    SuccessOrExit(err);

    // Step 4
    err = mEphemeralKey.ECDH_derive_secret(mRemotePubKey, mSharedSecret);
    SuccessOrExit(err);

    err = ComputeIPK(mConnectionState.GetLocalKeyID(), mIPK, sizeof(mIPK));
    SuccessOrExit(err);

    // Step 5
    err = ConstructSaltSigmaR2(msg_rand, mEphemeralKey.Pubkey(), mIPK, sizeof(mIPK), msg_salt);
    SuccessOrExit(err);

    err = HKDF_SHA256(mSharedSecret, mSharedSecret.Length(), msg_salt->Start(), saltlen, kKDFSR2Info, kKDFInfoLength, sr2k,
                      kAEADKeySize);
    SuccessOrExit(err);

    // Step 6
    msg_r2_signed_len =
        static_cast<uint16_t>(sizeof(uint16_t) + mOpCredSet->GetDevOpCredLen(mTrustedRootId) + kP256_PublicKey_Length * 2);

    msg_R2_Signed = System::PacketBufferHandle::New(msg_r2_signed_len);
    VerifyOrExit(!msg_R2_Signed.IsNull(), err = CHIP_SYSTEM_ERROR_NO_MEMORY);

    {
        Encoding::LittleEndian::BufferWriter bbuf(msg_R2_Signed->Start(), msg_r2_signed_len);

        bbuf.Put(mEphemeralKey.Pubkey(), mEphemeralKey.Pubkey().Length());
        bbuf.Put16(mOpCredSet->GetDevOpCredLen(mTrustedRootId));
        bbuf.Put(mOpCredSet->GetDevOpCred(mTrustedRootId), mOpCredSet->GetDevOpCredLen(mTrustedRootId));
        bbuf.Put(mRemotePubKey, mRemotePubKey.Length());

        VerifyOrExit(bbuf.Fit(), err = CHIP_ERROR_NO_MEMORY);
    }

    msg_R2_Signed->SetDataLength(msg_r2_signed_len);

    // Step 7
    err = mOpCredSet->SignMsg(mTrustedRootId, msg_R2_Signed->Start(), msg_R2_Signed->DataLength(), sigmaR2Signature);
    SuccessOrExit(err);

    // Step 8
    msg_r2_signed_enc_len = sizeof(uint16_t) + mOpCredSet->GetDevOpCredLen(mTrustedRootId) + sigmaR2Signature.Length();

    msg_R2_Encrypted = System::PacketBufferHandle::New(msg_r2_signed_enc_len);
    VerifyOrExit(!msg_R2_Encrypted.IsNull(), err = CHIP_SYSTEM_ERROR_NO_MEMORY);

    {
        Encoding::LittleEndian::BufferWriter bbuf(msg_R2_Encrypted->Start(), msg_r2_signed_enc_len);

        bbuf.Put16(mOpCredSet->GetDevOpCredLen(mTrustedRootId));
        bbuf.Put(mOpCredSet->GetDevOpCred(mTrustedRootId), mOpCredSet->GetDevOpCredLen(mTrustedRootId));
        bbuf.Put(sigmaR2Signature, sigmaR2Signature.Length());

        VerifyOrExit(bbuf.Fit(), err = CHIP_ERROR_NO_MEMORY);
    }

    msg_R2_Encrypted->SetDataLength(static_cast<uint16_t>(msg_r2_signed_enc_len));

    // Step 9
    err = AES_CCM_encrypt(msg_R2_Encrypted->Start(), msg_r2_signed_enc_len, nullptr, 0, sr2k, kAEADKeySize, kIVSR2, kIVLength,
                          msg_R2_Encrypted->Start(), tag, sizeof(tag));
    SuccessOrExit(err);

    data_len = static_cast<uint16_t>(kSigmaParamRandomNumberSize + sizeof(uint16_t) + kTrustedRootIdSize + kP256_PublicKey_Length +
                                     msg_r2_signed_enc_len + sizeof(tag));

    msg_R2 = System::PacketBufferHandle::New(data_len);
    VerifyOrExit(!msg_R2.IsNull(), err = CHIP_SYSTEM_ERROR_NO_MEMORY);

    // Step 10
    // now construct sigmaR2
    {
        Encoding::LittleEndian::BufferWriter bbuf(msg_R2->Start(), data_len);

        bbuf.Put(msg_rand->Start(), kSigmaParamRandomNumberSize);
        // Responder's session ID
        bbuf.Put16(mConnectionState.GetLocalKeyID());
        // Step 2
        bbuf.Put(mTrustedRootId.mId, mTrustedRootId.mLen);
        bbuf.Put(mEphemeralKey.Pubkey(), mEphemeralKey.Pubkey().Length());
        bbuf.Put(msg_R2_Encrypted->Start(), msg_r2_signed_enc_len);
        bbuf.Put(tag, sizeof(tag));

        VerifyOrExit(bbuf.Fit(), err = CHIP_ERROR_NO_MEMORY);
    }

    msg_R2->SetDataLength(data_len);

    err = mCommissioningHash.AddData(msg_R2->Start(), msg_R2->DataLength());
    SuccessOrExit(err);

    mNextExpectedMsg = Protocols::SecureChannel::MsgType::CASE_SigmaR3;

    // Call delegate to send the msg to peer
    err = mExchangeCtxt->SendMessage(Protocols::SecureChannel::MsgType::CASE_SigmaR2, std::move(msg_R2),
                                     SendFlags(SendMessageFlags::kExpectResponse));
    SuccessOrExit(err);

    ChipLogDetail(Inet, "Sent SigmaR2 msg");

exit:

    if (err != CHIP_NO_ERROR)
    {
        SendErrorMsg(SigmaErrorType::kUnexpected);
    }
    return err;
}

CHIP_ERROR CASESession::HandleSigmaR2_and_SendSigmaR3(const System::PacketBufferHandle & msg)
{
    ReturnErrorOnFailure(HandleSigmaR2(msg));
    ReturnErrorOnFailure(SendSigmaR3());

    return CHIP_NO_ERROR;
}

CHIP_ERROR CASESession::HandleSigmaR2(const System::PacketBufferHandle & msg)
{
    CHIP_ERROR err = CHIP_NO_ERROR;

    const uint8_t * buf = msg->Start();
    size_t buflen       = msg->DataLength();

    System::PacketBufferHandle msg_salt;
    uint16_t saltlen;

    System::PacketBufferHandle msg_R2_Signed;
    uint16_t msg_r2_signed_len;

    uint8_t sr2k[kAEADKeySize];

    P256ECDSASignature sigmaR2SignedData;
    size_t sigLen;

    P256PublicKey remoteCredential;

    const uint8_t * remoteDeviceOpCert;
    uint16_t remoteDeviceOpCertLen;

    uint8_t * msg_r2_encrypted;

    const uint8_t * tag = buf + buflen - kTAGSize;

    uint16_t encryptionKeyId = 0;

    VerifyOrExit(buf != nullptr, err = CHIP_ERROR_MESSAGE_INCOMPLETE);

    ChipLogDetail(Inet, "Received SigmaR2 msg");

    // Step 1
    // skip random part
    buf += kSigmaParamRandomNumberSize;

    encryptionKeyId = chip::Encoding::LittleEndian::Read16(buf);

    ChipLogDetail(Inet, "Peer assigned session key ID %d", encryptionKeyId);
    mConnectionState.SetPeerKeyID(encryptionKeyId);

    err = FindValidTrustedRoot(&buf, 1);
    SuccessOrExit(err);

    // fill in Remote Public Key
    {
        Encoding::LittleEndian::BufferWriter bbuf(mRemotePubKey, mRemotePubKey.Length());
        bbuf.Put(buf, mRemotePubKey.Length());
        buf += kP256_PublicKey_Length;

        VerifyOrExit(bbuf.Fit(), err = CHIP_ERROR_NO_MEMORY);
    }

    // Step 2
    err = mEphemeralKey.ECDH_derive_secret(mRemotePubKey, mSharedSecret);
    SuccessOrExit(err);

    // Step 3
    saltlen = kIPKSize + kSigmaParamRandomNumberSize + kP256_PublicKey_Length + kSHA256_Hash_Length;

    msg_salt = System::PacketBufferHandle::New(saltlen);
    VerifyOrExit(!msg_salt.IsNull(), err = CHIP_SYSTEM_ERROR_NO_MEMORY);
    msg_salt->SetDataLength(saltlen);

    err = ComputeIPK(mConnectionState.GetPeerKeyID(), mRemoteIPK, sizeof(mRemoteIPK));
    SuccessOrExit(err);

    err = ConstructSaltSigmaR2(msg, mRemotePubKey, mRemoteIPK, sizeof(mRemoteIPK), msg_salt);
    SuccessOrExit(err);

    err = HKDF_SHA256(mSharedSecret, mSharedSecret.Length(), msg_salt->Start(), saltlen, kKDFSR2Info, kKDFInfoLength, sr2k,
                      kAEADKeySize);
    SuccessOrExit(err);

    err = mCommissioningHash.AddData(msg->Start(), msg->DataLength());
    SuccessOrExit(err);

    // Step 4
    msg_r2_encrypted = const_cast<uint8_t *>(buf);

    err = AES_CCM_decrypt(msg_r2_encrypted,
                          buflen - kSigmaParamRandomNumberSize - sizeof(encryptionKeyId) - kTrustedRootIdSize -
                              kP256_PublicKey_Length - kTAGSize,
                          nullptr, 0, tag, kTAGSize, sr2k, kAEADKeySize, kIVSR2, kIVLength, msg_r2_encrypted);
    SuccessOrExit(err);

    // Step 5
    // Validate responder identity located in msg_r2_encrypted
    // Constructing responder identity
    err = Validate_and_RetrieveResponderID(&buf, remoteCredential, &remoteDeviceOpCert, remoteDeviceOpCertLen);
    SuccessOrExit(err);

    // Step 6 - Construct msg_R2_Signed and validate the signature in msg_r2_encrypted
    msg_r2_signed_len = static_cast<uint16_t>(sizeof(uint16_t) + remoteDeviceOpCertLen + kP256_PublicKey_Length * 2);

    msg_R2_Signed = System::PacketBufferHandle::New(msg_r2_signed_len);
    VerifyOrExit(!msg_R2_Signed.IsNull(), err = CHIP_SYSTEM_ERROR_NO_MEMORY);
    msg_R2_Signed->SetDataLength(msg_r2_signed_len);

    sigLen = buflen - kSigmaParamRandomNumberSize - sizeof(encryptionKeyId) - kTrustedRootIdSize - kP256_PublicKey_Length -
        sizeof(uint16_t) - remoteDeviceOpCertLen - kTAGSize;
    err = ConstructSignedCredentials(&buf, remoteDeviceOpCert, remoteDeviceOpCertLen, msg_R2_Signed, sigmaR2SignedData, sigLen);
    SuccessOrExit(err);

    err = remoteCredential.ECDSA_validate_msg_signature(msg_R2_Signed->Start(), msg_r2_signed_len, sigmaR2SignedData);
    SuccessOrExit(err);

exit:
    if (err == CHIP_ERROR_INVALID_SIGNATURE)
    {
        SendErrorMsg(SigmaErrorType::kInvalidSignature);
    }
    else if (err == CHIP_ERROR_CERT_NOT_TRUSTED)
    {
        SendErrorMsg(SigmaErrorType::kNoSharedTrustRoots);
    }
    else if (err != CHIP_NO_ERROR)
    {
        SendErrorMsg(SigmaErrorType::kUnexpected);
    }
    return err;
}

CHIP_ERROR CASESession::SendSigmaR3()
{
    CHIP_ERROR err = CHIP_NO_ERROR;

    System::PacketBufferHandle msg_R3;
    uint16_t data_len;

    System::PacketBufferHandle msg_R3_Encrypted;
    uint16_t msg_r3_encrypted_len;

    System::PacketBufferHandle msg_salt;
    uint16_t saltlen;

    uint8_t sr3k[kAEADKeySize];

    System::PacketBufferHandle msg_R3_Signed;
    uint16_t msg_r3_signed_len;

    P256ECDSASignature sigmaR3Signature;

    uint8_t tag[kTAGSize];

    // Step 1
    saltlen = kIPKSize + kSHA256_Hash_Length;

    msg_salt = System::PacketBufferHandle::New(saltlen);
    VerifyOrExit(!msg_salt.IsNull(), err = CHIP_SYSTEM_ERROR_NO_MEMORY);
    msg_salt->SetDataLength(saltlen);

    err = ConstructSaltSigmaR3(mIPK, sizeof(mIPK), msg_salt);
    SuccessOrExit(err);

    err = HKDF_SHA256(mSharedSecret, mSharedSecret.Length(), msg_salt->Start(), saltlen, kKDFSR3Info, kKDFInfoLength, sr3k,
                      kAEADKeySize);
    SuccessOrExit(err);

    // Step 2
    msg_r3_signed_len =
        static_cast<uint16_t>(sizeof(uint16_t) + mOpCredSet->GetDevOpCredLen(mTrustedRootId) + kP256_PublicKey_Length * 2);

    msg_R3_Signed = System::PacketBufferHandle::New(msg_r3_signed_len);
    VerifyOrExit(!msg_R3_Signed.IsNull(), err = CHIP_SYSTEM_ERROR_NO_MEMORY);

    {
        Encoding::LittleEndian::BufferWriter bbuf(msg_R3_Signed->Start(), msg_r3_signed_len);

        bbuf.Put(mEphemeralKey.Pubkey(), mEphemeralKey.Pubkey().Length());
        bbuf.Put16(mOpCredSet->GetDevOpCredLen(mTrustedRootId));
        bbuf.Put(mOpCredSet->GetDevOpCred(mTrustedRootId), mOpCredSet->GetDevOpCredLen(mTrustedRootId));
        bbuf.Put(mRemotePubKey, mRemotePubKey.Length());

        VerifyOrExit(bbuf.Fit(), err = CHIP_ERROR_NO_MEMORY);
    }

    msg_R3_Signed->SetDataLength(msg_r3_signed_len);

    // Step 3
    err = mOpCredSet->SignMsg(mTrustedRootId, msg_R3_Signed->Start(), msg_R3_Signed->DataLength(), sigmaR3Signature);
    SuccessOrExit(err);

    // Step 4
    msg_r3_encrypted_len = static_cast<uint16_t>(sizeof(uint16_t) + mOpCredSet->GetDevOpCredLen(mTrustedRootId) +
                                                 static_cast<uint16_t>(sigmaR3Signature.Length()));

    msg_R3_Encrypted = System::PacketBufferHandle::New(msg_r3_encrypted_len);
    VerifyOrExit(!msg_R3_Encrypted.IsNull(), err = CHIP_SYSTEM_ERROR_NO_MEMORY);

    {
        Encoding::LittleEndian::BufferWriter bbuf(msg_R3_Encrypted->Start(), msg_r3_encrypted_len);

        bbuf.Put16(mOpCredSet->GetDevOpCredLen(mTrustedRootId));
        bbuf.Put(mOpCredSet->GetDevOpCred(mTrustedRootId), mOpCredSet->GetDevOpCredLen(mTrustedRootId));
        bbuf.Put(sigmaR3Signature, sigmaR3Signature.Length());

        VerifyOrExit(bbuf.Fit(), err = CHIP_ERROR_NO_MEMORY);
    }

    msg_R3_Encrypted->SetDataLength(msg_r3_encrypted_len);

    // Step 5
    err = AES_CCM_encrypt(msg_R3_Encrypted->Start(), msg_r3_encrypted_len, nullptr, 0, sr3k, kAEADKeySize, kIVSR3, kIVLength,
                          msg_R3_Encrypted->Start(), tag, sizeof(tag));
    SuccessOrExit(err);

    // Step 6
    data_len = static_cast<uint16_t>(sizeof(tag) + msg_r3_encrypted_len);

    msg_R3 = System::PacketBufferHandle::New(data_len);
    VerifyOrExit(!msg_R3.IsNull(), err = CHIP_SYSTEM_ERROR_NO_MEMORY);

    {
        Encoding::LittleEndian::BufferWriter bbuf(msg_R3->Start(), data_len);

        bbuf.Put(msg_R3_Encrypted->Start(), msg_R3_Encrypted->DataLength());
        bbuf.Put(tag, sizeof(tag));

        VerifyOrExit(bbuf.Fit(), err = CHIP_ERROR_NO_MEMORY);
    }

    msg_R3->SetDataLength(data_len);

    err = mCommissioningHash.AddData(msg_R3->Start(), msg_R3->DataLength());
    SuccessOrExit(err);

    // Call delegate to send the Msg3 to peer
    err = mExchangeCtxt->SendMessage(Protocols::SecureChannel::MsgType::CASE_SigmaR3, std::move(msg_R3));
    SuccessOrExit(err);

    ChipLogDetail(Inet, "Sent SigmaR3 msg");

    err = mCommissioningHash.Finish(mMessageDigest);
    SuccessOrExit(err);

    mPairingComplete = true;

    // Call delegate to indicate pairing completion
    mDelegate->OnSessionEstablished();

exit:

    if (err != CHIP_NO_ERROR)
    {
        SendErrorMsg(SigmaErrorType::kUnexpected);
    }
    return err;
}

CHIP_ERROR CASESession::HandleSigmaR3(const System::PacketBufferHandle & msg)
{
    CHIP_ERROR err = CHIP_NO_ERROR;

    const uint8_t * buf = msg->Start();

    System::PacketBufferHandle msg_R3_Signed;
    uint16_t msg_r3_signed_len;

    uint8_t sr3k[kAEADKeySize];

    P256ECDSASignature sigmaR3SignedData;
    size_t sigLen;

    P256PublicKey remoteCredential;

    const uint8_t * remoteDeviceOpCert;
    uint16_t remoteDeviceOpCertLen;

    System::PacketBufferHandle msg_salt;
    uint16_t saltlen;

    uint8_t * tag = msg->Start() + msg->DataLength() - kTAGSize;

    ChipLogDetail(Inet, "Received SigmaR3 msg");

    mNextExpectedMsg = Protocols::SecureChannel::MsgType::CASE_SigmaErr;

    // Step 1
    saltlen = kIPKSize + kSHA256_Hash_Length;

    msg_salt = System::PacketBufferHandle::New(saltlen);
    VerifyOrExit(!msg_salt.IsNull(), err = CHIP_SYSTEM_ERROR_NO_MEMORY);
    msg_salt->SetDataLength(saltlen);

    err = ComputeIPK(mConnectionState.GetPeerKeyID(), mRemoteIPK, sizeof(mRemoteIPK));
    SuccessOrExit(err);

    err = ConstructSaltSigmaR3(mRemoteIPK, sizeof(mRemoteIPK), msg_salt);
    SuccessOrExit(err);

    err = HKDF_SHA256(mSharedSecret, mSharedSecret.Length(), msg_salt->Start(), saltlen, kKDFSR3Info, kKDFInfoLength, sr3k,
                      kAEADKeySize);
    SuccessOrExit(err);

    err = mCommissioningHash.AddData(msg->Start(), msg->DataLength());
    SuccessOrExit(err);

    // Step 2
    err = AES_CCM_decrypt(msg->Start(), msg->DataLength() - kTAGSize, nullptr, 0, tag, kTAGSize, sr3k, kAEADKeySize, kIVSR3,
                          kIVLength, msg->Start());
    SuccessOrExit(err);

    // Step 3
    // Validate initiator identity located in msg->Start()
    // Constructing responder identity
    err = Validate_and_RetrieveResponderID(&buf, remoteCredential, &remoteDeviceOpCert, remoteDeviceOpCertLen);
    SuccessOrExit(err);

    // Step 4
    msg_r3_signed_len = static_cast<uint16_t>(sizeof(uint16_t) + remoteDeviceOpCertLen + kP256_PublicKey_Length * 2);

    msg_R3_Signed = System::PacketBufferHandle::New(msg_r3_signed_len);
    VerifyOrExit(!msg_R3_Signed.IsNull(), err = CHIP_SYSTEM_ERROR_NO_MEMORY);
    msg_R3_Signed->SetDataLength(msg_r3_signed_len);

    sigLen = msg->DataLength() - sizeof(uint16_t) - remoteDeviceOpCertLen - kTAGSize;
    err    = ConstructSignedCredentials(&buf, remoteDeviceOpCert, remoteDeviceOpCertLen, msg_R3_Signed, sigmaR3SignedData, sigLen);
    SuccessOrExit(err);

    err = remoteCredential.ECDSA_validate_msg_signature(msg_R3_Signed->Start(), msg_r3_signed_len, sigmaR3SignedData);
    SuccessOrExit(err);

    err = mCommissioningHash.Finish(mMessageDigest);
    SuccessOrExit(err);

    mPairingComplete = true;

    // Call delegate to indicate pairing completion
    mDelegate->OnSessionEstablished();

exit:
    if (err == CHIP_ERROR_INVALID_SIGNATURE)
    {
        SendErrorMsg(SigmaErrorType::kInvalidSignature);
    }
    else if (err != CHIP_NO_ERROR)
    {
        SendErrorMsg(SigmaErrorType::kUnexpected);
    }
    return err;
}

void CASESession::SendErrorMsg(SigmaErrorType errorCode)
{
    CHIP_ERROR err = CHIP_NO_ERROR;

    System::PacketBufferHandle msg;
    uint16_t msglen      = sizeof(SigmaErrorMsg);
    SigmaErrorMsg * pMsg = nullptr;

    msg = System::PacketBufferHandle::New(msglen);
    VerifyOrExit(!msg.IsNull(), err = CHIP_SYSTEM_ERROR_NO_MEMORY);

    pMsg        = reinterpret_cast<SigmaErrorMsg *>(msg->Start());
    pMsg->error = errorCode;

    msg->SetDataLength(msglen);

    err = mExchangeCtxt->SendMessage(Protocols::SecureChannel::MsgType::CASE_SigmaErr, std::move(msg));
    SuccessOrExit(err);

exit:
    Clear();
}

CHIP_ERROR CASESession::FindValidTrustedRoot(const uint8_t ** msgIterator, uint32_t nTrustedRoots)
{
    CertificateKeyId trustedRoot[kMaxTrustedRootIds];

    for (uint32_t i = 0; i < nTrustedRoots; ++i)
    {
        trustedRoot[i].mId  = *msgIterator;
        trustedRoot[i].mLen = kTrustedRootIdSize;
        *msgIterator += kTrustedRootIdSize;

        if (mOpCredSet->IsTrustedRootIn(trustedRoot[i]))
        {
            mTrustedRootId.mId = reinterpret_cast<const uint8_t *>(chip::Platform::MemoryAlloc(kTrustedRootIdSize));
            VerifyOrReturnError(mTrustedRootId.mId != nullptr, CHIP_ERROR_NO_MEMORY);

            memcpy(const_cast<uint8_t *>(mTrustedRootId.mId), trustedRoot[i].mId, trustedRoot[i].mLen);
            mTrustedRootId.mLen = trustedRoot[i].mLen;

            break;
        }
    }
    VerifyOrReturnError(mTrustedRootId.mId != nullptr, CHIP_ERROR_CERT_NOT_TRUSTED);

    return CHIP_NO_ERROR;
}

CHIP_ERROR CASESession::ConstructSaltSigmaR2(const System::PacketBufferHandle & rand, const P256PublicKey & pubkey,
                                             const uint8_t * ipk, size_t ipkLen, System::PacketBufferHandle & salt)
{
    uint8_t md[kSHA256_Hash_Length];
    Encoding::LittleEndian::BufferWriter bbuf(salt->Start(), salt->DataLength());

    bbuf.Put(ipk, ipkLen);
    bbuf.Put(rand->Start(), kSigmaParamRandomNumberSize);
    bbuf.Put(pubkey, pubkey.Length());
    ReturnErrorOnFailure(mCommissioningHash.Finish(md));
    bbuf.Put(md, kSHA256_Hash_Length);

    VerifyOrReturnError(bbuf.Fit(), CHIP_ERROR_NO_MEMORY);

    return CHIP_NO_ERROR;
}

CHIP_ERROR CASESession::ConstructSaltSigmaR3(const uint8_t * ipk, size_t ipkLen, System::PacketBufferHandle & salt)
{
    uint8_t md[kSHA256_Hash_Length];
    Encoding::LittleEndian::BufferWriter bbuf(salt->Start(), salt->DataLength());

    bbuf.Put(ipk, ipkLen);
    ReturnErrorOnFailure(mCommissioningHash.Finish(md));
    bbuf.Put(md, kSHA256_Hash_Length);

    VerifyOrReturnError(bbuf.Fit(), CHIP_ERROR_NO_MEMORY);

    return CHIP_NO_ERROR;
}

CHIP_ERROR CASESession::Validate_and_RetrieveResponderID(const uint8_t ** msgIterator, P256PublicKey & responderID,
                                                         const uint8_t ** responderOpCert, uint16_t & responderOpCertLen)
{
    ChipCertificateData chipCertData;
    ChipCertificateData * resultCert = nullptr;

    responderOpCertLen = chip::Encoding::LittleEndian::Read16(*msgIterator);
    *responderOpCert   = *msgIterator;
    *msgIterator += responderOpCertLen;

    Encoding::LittleEndian::BufferWriter bbuf(responderID, responderID.Length());
    ReturnErrorOnFailure(DecodeChipCert(*responderOpCert, responderOpCertLen, chipCertData));

    bbuf.Put(chipCertData.mPublicKey, chipCertData.mPublicKeyLen);

    VerifyOrReturnError(bbuf.Fit(), CHIP_ERROR_NO_MEMORY);

    // Validate responder identity located in msg_r2_encrypted
    ReturnErrorOnFailure(
        mOpCredSet->FindCertSet(mTrustedRootId)
            ->LoadCert(*responderOpCert, responderOpCertLen, BitFlags<CertDecodeFlags>(CertDecodeFlags::kGenerateTBSHash)));

    ReturnErrorOnFailure(SetEffectiveTime());
    // Locate the subject DN and key id that will be used as input the FindValidCert() method.
    {
        const ChipDN & subjectDN              = chipCertData.mSubjectDN;
        const CertificateKeyId & subjectKeyId = chipCertData.mSubjectKeyId;

        ReturnErrorOnFailure(mOpCredSet->FindValidCert(mTrustedRootId, subjectDN, subjectKeyId, mValidContext, resultCert));
    }

    return CHIP_NO_ERROR;
}

CHIP_ERROR CASESession::ConstructSignedCredentials(const uint8_t ** msgIterator, const uint8_t * responderOpCert,
                                                   uint16_t responderOpCertLen, System::PacketBufferHandle & signedCredentials,
                                                   P256ECDSASignature & signature, size_t sigLen)
{
    {
        Encoding::LittleEndian::BufferWriter bbuf(signedCredentials->Start(), signedCredentials->DataLength());

        bbuf.Put(mRemotePubKey, mRemotePubKey.Length());
        bbuf.Put16(responderOpCertLen);
        bbuf.Put(responderOpCert, responderOpCertLen);
        bbuf.Put(mEphemeralKey.Pubkey(), mEphemeralKey.Pubkey().Length());

        VerifyOrReturnError(bbuf.Fit(), CHIP_ERROR_NO_MEMORY);
    }
    {
        signature.SetLength(sigLen);
        Encoding::LittleEndian::BufferWriter bbuf(signature, signature.Length());
        bbuf.Put(*msgIterator, signature.Length());

        VerifyOrReturnError(bbuf.Fit(), CHIP_ERROR_NO_MEMORY);
    }
    return CHIP_NO_ERROR;
}

CHIP_ERROR CASESession::ComputeIPK(const uint16_t sessionID, uint8_t * ipk, size_t ipkLen)
{
    ReturnErrorOnFailure(HKDF_SHA256(fabricSecret, fabricSecret.Length(), reinterpret_cast<const uint8_t *>(&sessionID),
                                     sizeof(sessionID), kIPKInfo, sizeof(kIPKInfo), ipk, ipkLen));

    return CHIP_NO_ERROR;
}

// TODO: Remove this and replace with system method to retrieve current time
CHIP_ERROR CASESession::SetEffectiveTime(void)
{
    using namespace ASN1;
    ASN1UniversalTime effectiveTime;

    effectiveTime.Year   = 2021;
    effectiveTime.Month  = 2;
    effectiveTime.Day    = 12;
    effectiveTime.Hour   = 10;
    effectiveTime.Minute = 10;
    effectiveTime.Second = 10;

    return ASN1ToChipEpochTime(effectiveTime, mValidContext.mEffectiveTime);
}

void CASESession::HandleErrorMsg(const System::PacketBufferHandle & msg)
{
    // Error message processing
    const uint8_t * buf  = msg->Start();
    size_t buflen        = msg->DataLength();
    SigmaErrorMsg * pMsg = nullptr;

    VerifyOrExit(buf != nullptr, ChipLogError(Inet, "Null error msg received during pairing"));
    static_assert(sizeof(SigmaErrorMsg) == sizeof(uint8_t),
                  "Assuming size of SigmaErrorMsg message is 1 octet, so that endian-ness conversion is not needed");
    VerifyOrExit(buflen == sizeof(SigmaErrorMsg), ChipLogError(Inet, "Error msg with incorrect length received during pairing"));

    pMsg = reinterpret_cast<SigmaErrorMsg *>(msg->Start());
    ChipLogError(Inet, "Received error (%d) during CASE pairing process", pMsg->error);

exit:
    Clear();
}

CHIP_ERROR CASESession::ValidateReceivedMessage(ExchangeContext * ec, const PacketHeader & packetHeader,
                                                const PayloadHeader & payloadHeader, System::PacketBufferHandle & msg)
{
    VerifyOrReturnError(ec != nullptr, CHIP_ERROR_INVALID_ARGUMENT);

    // mExchangeCtxt can be nullptr if this is the first message (CASE_SigmaR1) received by CASESession
    // via UnsolicitedMessageHandler. The exchange context is allocated by exchange manager and provided
    // to the handler (CASESession object).
    if (mExchangeCtxt != nullptr)
    {
        if (mExchangeCtxt != ec)
        {
            // Close the incoming exchange explicitly, as the cleanup code only closes mExchangeCtxt
            ec->Close();
            ReturnErrorOnFailure(CHIP_ERROR_INVALID_ARGUMENT);
        }
    }
    else
    {
        mExchangeCtxt = ec;
        mExchangeCtxt->SetResponseTimeout(kSigma_Response_Timeout);
    }

    VerifyOrReturnError(!msg.IsNull(), CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrReturnError(payloadHeader.HasMessageType(mNextExpectedMsg) ||
                            payloadHeader.HasMessageType(Protocols::SecureChannel::MsgType::CASE_SigmaErr),
                        CHIP_ERROR_INVALID_MESSAGE_TYPE);

    if (packetHeader.GetSourceNodeId().HasValue())
    {
        if (mConnectionState.GetPeerNodeId() == kUndefinedNodeId)
        {
            mConnectionState.SetPeerNodeId(packetHeader.GetSourceNodeId().Value());
        }
        else
        {
            VerifyOrReturnError(packetHeader.GetSourceNodeId().Value() == mConnectionState.GetPeerNodeId(),
                                CHIP_ERROR_WRONG_NODE_ID);
        }
    }

    return CHIP_NO_ERROR;
}

void CASESession::OnMessageReceived(ExchangeContext * ec, const PacketHeader & packetHeader, const PayloadHeader & payloadHeader,
                                    System::PacketBufferHandle msg)
{
    CHIP_ERROR err = ValidateReceivedMessage(ec, packetHeader, payloadHeader, msg);

    if (err != CHIP_NO_ERROR)
    {
        Clear();
        SuccessOrExit(err);
    }

    mConnectionState.SetPeerAddress(mMessageDispatch.GetPeerAddress());

    switch (static_cast<Protocols::SecureChannel::MsgType>(payloadHeader.GetMessageType()))
    {
    case Protocols::SecureChannel::MsgType::CASE_SigmaR1:
        err = HandleSigmaR1_and_SendSigmaR2(msg);
        break;

    case Protocols::SecureChannel::MsgType::CASE_SigmaR2:
        err = HandleSigmaR2_and_SendSigmaR3(msg);
        break;

    case Protocols::SecureChannel::MsgType::CASE_SigmaR3:
        err = HandleSigmaR3(msg);
        break;

    case Protocols::SecureChannel::MsgType::CASE_SigmaErr:
        HandleErrorMsg(msg);
        break;

    default:
        SendErrorMsg(SigmaErrorType::kUnexpected);
        err = CHIP_ERROR_INVALID_MESSAGE_TYPE;
        break;
    };

exit:

    // Call delegate to indicate session establishment failure.
    if (err != CHIP_NO_ERROR)
    {
        mDelegate->OnSessionEstablishmentError(err);
    }
}

} // namespace chip